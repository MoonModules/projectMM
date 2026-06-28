#include "moonlive_asm_riscv.h"

#include <cstring>

#if defined(__riscv)   // the RISC-V assembler is only built for RISC-V targets (ESP32-P4)

// MoonLive RISC-V assembler — RV32 named instructions, encodings verified against
// riscv32-esp-elf-as (see the plan / commit). Fixed 4-byte little-endian instructions; the
// standard call ABI (args a0.., result a0, ra = return). Branch offsets are back-patched.

namespace mm::moonlive {

// R0..R4 → a0..a4 (10..14, the host args: buf, nLights, cpl, t, ctrls — a4=kArg4 the controls
// arena pointer). R5..R11 → t0,t1,t2,t3,t4,t5,a5 (caller-saved temps). t6(31) and a6(16) are the
// internal scratch (store8 address, call address build), not vregs.
static const uint8_t kRvReg[kRegCount] = {10, 11, 12, 13, 14, 5, 6, 7, 28, 29, 30, 15};
static uint8_t xr(Reg r) { return kRvReg[r]; }
static constexpr uint8_t kScratchAddr = 31;   // t6 — store8 address temp
static constexpr uint8_t kScratchFn   = 16;   // a6 — call address build / result stash

void RiscvAssembler::emit32(uint32_t w) {
    if (len_ + 4 > kCap) { overflow_ = true; return; }
    buf_[len_++] = uint8_t(w); buf_[len_++] = uint8_t(w >> 8);
    buf_[len_++] = uint8_t(w >> 16); buf_[len_++] = uint8_t(w >> 24);
}

Label RiscvAssembler::newLabel() {
    if (labelCount_ == 0) for (auto& p : labelPos_) p = -1;
    if (labelCount_ >= kMaxLabels) { overflow_ = true; return 0; }   // same overflow signal as emit32
    Label l = labelCount_++; labelPos_[l] = -1; return l;
}
void RiscvAssembler::bind(Label l) { if (l < kMaxLabels) labelPos_[l] = static_cast<int32_t>(len_); }

// Record a pending branch fixup, guarding the fixed table (overflow_ rather than an OOB write).
void RiscvAssembler::addFixup(size_t at, Label label) {
    if (fixupCount_ >= kMaxFixups) { overflow_ = true; return; }
    fixups_[fixupCount_++] = {at, label};
}

// --- I/R/S-type encoders (rd/rs1/rs2 are real x-register numbers) ---
static uint32_t encAddi(uint8_t rd, uint8_t rs1, int32_t imm) {
    return ((uint32_t(imm) & 0xfff) << 20) | (rs1 << 15) | (0 << 12) | (rd << 7) | 0x13;
}
static uint32_t encAdd(uint8_t rd, uint8_t rs1, uint8_t rs2) {
    return (rs2 << 20) | (rs1 << 15) | (0 << 12) | (rd << 7) | 0x33;
}
static uint32_t encMul(uint8_t rd, uint8_t rs1, uint8_t rs2) {
    return (1u << 25) | (rs2 << 20) | (rs1 << 15) | (0 << 12) | (rd << 7) | 0x33;
}
static uint32_t encSb(uint8_t rs2, uint8_t rs1, int32_t imm) {   // sb rs2, imm(rs1)
    return (((uint32_t(imm) >> 5) & 0x7f) << 25) | (rs2 << 20) | (rs1 << 15) | (0 << 12) |
           ((uint32_t(imm) & 0x1f) << 7) | 0x23;
}
static uint32_t encSw(uint8_t rs2, uint8_t rs1, int32_t imm) {   // sw rs2, imm(rs1)
    return (((uint32_t(imm) >> 5) & 0x7f) << 25) | (rs2 << 20) | (rs1 << 15) | (2 << 12) |
           ((uint32_t(imm) & 0x1f) << 7) | 0x23;
}
static uint32_t encLw(uint8_t rd, uint8_t rs1, int32_t imm) {    // lw rd, imm(rs1)
    return ((uint32_t(imm) & 0xfff) << 20) | (rs1 << 15) | (2 << 12) | (rd << 7) | 0x03;
}
static uint32_t encLui(uint8_t rd, uint32_t imm20) {
    return (imm20 << 12) | (rd << 7) | 0x37;
}
static uint32_t encBranch(uint8_t rs1, uint8_t rs2, uint8_t f3, int32_t off) {  // B-type
    uint32_t o = uint32_t(off) & 0x1fff;
    return (((o >> 12) & 1) << 31) | (((o >> 5) & 0x3f) << 25) | (rs2 << 20) | (rs1 << 15) |
           (f3 << 12) | (((o >> 1) & 0xf) << 8) | (((o >> 11) & 1) << 7) | 0x63;
}

void RiscvAssembler::movImm(Reg d, int32_t imm) {
    // addi sign-extends a 12-bit immediate, so it alone covers only -2048..2047. For wider
    // constants (a uint16 like 65535) materialise the full value with lui (high 20 bits) + addi
    // (low 12), the hi/lo split — without this, larger Const values truncate. Single addi when
    // the value fits, to keep the common small-constant case one instruction.
    if (imm >= -2048 && imm <= 2047) {
        emit32(encAddi(xr(d), 0, imm));                    // li = addi rd, x0, imm
        return;
    }
    uint32_t v  = static_cast<uint32_t>(imm);
    uint32_t hi = (v + 0x800) >> 12;                       // round for the sign-extended addi
    int32_t  lo = static_cast<int32_t>(v) - static_cast<int32_t>(hi << 12);
    emit32(encLui(xr(d), hi & 0xfffff));                   // lui rd, hi
    emit32(encAddi(xr(d), xr(d), lo));                     // addi rd, rd, lo
}
void RiscvAssembler::movReg(Reg d, Reg a)       { emit32(encAddi(xr(d), xr(a), 0)); }   // mv = addi rd,ra,0
void RiscvAssembler::addImm(Reg d, Reg a, int32_t imm) { emit32(encAddi(xr(d), xr(a), imm)); }
void RiscvAssembler::addReg(Reg d, Reg a, Reg b) { emit32(encAdd(xr(d), xr(a), xr(b))); }
void RiscvAssembler::mulReg(Reg d, Reg a, Reg b) { emit32(encMul(xr(d), xr(a), xr(b))); }

void RiscvAssembler::store8(Reg base, Reg off, Reg val) {
    emit32(encAdd(kScratchAddr, xr(base), xr(off)));   // t6 = base + off
    emit32(encSb(xr(val), kScratchAddr, 0));           // sb val, 0(t6)
}
void RiscvAssembler::load8(Reg d, Reg base, int32_t imm) {   // lbu rDst, imm(rBase) — control read
    emit32(((uint32_t(imm) & 0xfff) << 20) | (xr(base) << 15) | (4 << 12) | (xr(d) << 7) | 0x03);
}
void RiscvAssembler::branchIfZero(Reg a, Label l) {    // a == 0  ⇔  bgeu x0, a (unsigned 0 >= a)
    addFixup(len_, l);
    emit32(encBranch(0, xr(a), 7, 0));                 // bgeu x0, a, l  (patched)
}
void RiscvAssembler::branchGeU(Reg a, Reg b, Label l) {
    addFixup(len_, l);
    emit32(encBranch(xr(a), xr(b), 7, 0));             // bgeu a, b, l
}
void RiscvAssembler::branchNe(Reg a, Reg b, Label l) {
    addFixup(len_, l);
    emit32(encBranch(xr(a), xr(b), 1, 0));             // bne a, b, l
}

// Standard call to a host built-in: d = fn(a). All vreg temps are caller-saved, so a value
// live across the call must be preserved — save the whole pool + ra + the host args around the
// call (mirrors the host backend). The fn address is built with lui+addi (the hi/lo split, +1
// to the upper when the low 12 bits' sign bit is set). 64-byte frame, 16-byte aligned.
void RiscvAssembler::call(Reg d, Reg a, const void* fn) {
    emit32(encAddi(2, 2, -64));                        // addi sp, sp, -64
    emit32(encSw(1, 2, 60));                            // sw ra, 60(sp)
    // save the host args a0..a3 and all pool temps
    static const uint8_t saved[] = {10, 11, 12, 13, 5, 6, 7, 28, 29, 30, 14, 15};
    int off = 0;
    for (uint8_t r : saved) { emit32(encSw(r, 2, off)); off += 4; }
    // arg into a0 (read BEFORE the address build touches a6)
    emit32(encAddi(10, xr(a), 0));                     // mv a0, aArg  (if aArg==a0, no-op)
    // a6 = fn address via lui + addi (hi/lo split)
    uint32_t addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fn));
    uint32_t hi = (addr + 0x800) >> 12;                // round for the sign-extended addi
    int32_t  lo = static_cast<int32_t>(addr) - static_cast<int32_t>(hi << 12);
    emit32(encLui(kScratchFn, hi & 0xfffff));          // lui a6, hi
    emit32(encAddi(kScratchFn, kScratchFn, lo));       // addi a6, a6, lo
    emit32((kScratchFn << 15) | (1 << 7) | 0x67);      // jalr ra, a6, 0
    // stash result (a0) in a6 before restoring (a0 is restored to the old buf)
    emit32(encAddi(kScratchFn, 10, 0));                // mv a6, a0
    // restore
    off = 0;
    for (uint8_t r : saved) { emit32(encLw(r, 2, off)); off += 4; }
    emit32(encLw(1, 2, 60));                            // lw ra, 60(sp)
    emit32(encAddi(2, 2, 64));                          // addi sp, sp, 64
    emit32(encAddi(xr(d), kScratchFn, 0));             // mv dst, a6  (the result)
}

void RiscvAssembler::ret() { emit32(0x00008067u); }    // ret = jalr x0, ra, 0

void RiscvAssembler::patchBranches() {
    for (uint8_t i = 0; i < fixupCount_; i++) {
        const Fixup& f = fixups_[i];
        if (labelPos_[f.label] < 0) continue;                  // unbound label — leave as-is (overflow_ already failed the compile)
        int32_t off = labelPos_[f.label] - static_cast<int32_t>(f.at);
        uint32_t w; std::memcpy(&w, buf_ + f.at, 4);
        // re-scatter the offset into the B-type immediate fields, keeping the rest.
        w &= ~((1u<<31) | (0x3fu<<25) | (0xfu<<8) | (1u<<7));
        uint32_t o = uint32_t(off) & 0x1fff;
        w |= (((o>>12)&1)<<31) | (((o>>5)&0x3f)<<25) | (((o>>1)&0xf)<<8) | (((o>>11)&1)<<7);
        std::memcpy(buf_ + f.at, &w, 4);
    }
}

}  // namespace mm::moonlive

#endif  // __riscv
