#include "moonlive_asm_host.h"

#include <cstring>

// MoonLive host assembler — arm64 and x86-64 encodings, chosen at compile time (the same
// #if-by-arch shape moonlive_emit.cpp uses). Each named instruction is encoded ONCE here; the
// IR lowering composes them. Branch displacements are resolved by patchBranches() against
// bound labels, so no offset is ever hand-computed (the crash class the spike avoided by never
// composing now stays avoided by back-patching).

namespace mm::moonlive {

#if defined(__aarch64__)

// arm64 register map: R0..R3 = the host-ABI arg registers x0..x3 (buf, nLights, cpl, t);
// R4..R9 = caller-saved scratch x9..x14 (free across our leaf code). Index math uses the
// 64-bit views (xN) for addresses, 32-bit (wN) for counters/colours — but the same register
// number, so one map suffices.
// R0..R3 = x0..x3 (the host-ABI args); R4..R13 = caller-saved scratch x9..x14 then x4..x7.
// x15 is reserved for call()'s address/immediate scratch, so it's not in the pool.
static const uint8_t kArm64Reg[kRegCount] = {0, 1, 2, 3, 9, 10, 11, 12, 13, 14, 4, 5, 6, 7};
static uint8_t mr(Reg r) { return kArm64Reg[r]; }

Label HostAssembler::newLabel() {
    if (labelCount_ == 0) for (auto& p : labelPos_) p = -1;
    Label l = labelCount_++;
    labelPos_[l] = -1;
    return l;
}
void HostAssembler::bind(Label l) { labelPos_[l] = static_cast<int32_t>(len_); }

void HostAssembler::emit32(uint32_t w) {
    if (len_ + 4 > kCap) { overflow_ = true; return; }
    buf_[len_++] = uint8_t(w); buf_[len_++] = uint8_t(w >> 8);
    buf_[len_++] = uint8_t(w >> 16); buf_[len_++] = uint8_t(w >> 24);
}
void HostAssembler::emitBytes(const uint8_t* p, size_t n) {
    if (len_ + n > kCap) { overflow_ = true; return; }
    std::memcpy(buf_ + len_, p, n); len_ += n;
}

void HostAssembler::movImm(Reg d, int32_t imm) {           // movz wD, #imm16
    emit32(0x52800000u | ((uint32_t(imm) & 0xffff) << 5) | mr(d));
}
void HostAssembler::addImm(Reg d, Reg a, int32_t imm) {    // add xD, xA, #imm12 (64-bit)
    emit32(0x91000000u | ((uint32_t(imm) & 0xfff) << 10) | (mr(a) << 5) | mr(d));
}
void HostAssembler::addReg(Reg d, Reg a, Reg b) {          // add wD, wA, wB (32-bit)
    emit32(0x0b000000u | (mr(b) << 16) | (mr(a) << 5) | mr(d));
}
void HostAssembler::mulImm(Reg d, Reg a, int32_t imm) {    // d = a * imm via mov x15 + mul
    // Small-immediate multiply: load imm into x15 (not in the Reg map, so it clobbers no
    // vreg) then mul. x15 is caller-saved scratch on the host ABI.
    emit32(0x52800000u | ((uint32_t(imm) & 0xffff) << 5) | 15);          // movz w15, #imm
    emit32(0x1b007c00u | (15 << 16) | (mr(a) << 5) | mr(d));             // mul wD, wA, w15
}
void HostAssembler::mulReg(Reg d, Reg a, Reg b) {         // mul wD, wA, wB
    emit32(0x1b007c00u | (mr(b) << 16) | (mr(a) << 5) | mr(d));
}
void HostAssembler::store8(Reg base, Reg off, Reg val) {   // strb wVal, [xBase, xOff]
    emit32(0x38206800u | (mr(off) << 16) | (mr(base) << 5) | mr(val));
}
void HostAssembler::cmp(Reg a, Reg b) {                    // cmp wA, wB  (subs wzr, wA, wB)
    emit32(0x6b00001fu | (mr(b) << 16) | (mr(a) << 5));
}
void HostAssembler::branchIfZero(Reg a, Label l) {         // cbz wA, l  (offset patched)
    fixups_[fixupCount_++] = {len_, l, 0};
    emit32(0x34000000u | mr(a));
}
void HostAssembler::branchIf(Cond c, Label l) {            // b.cond l  (offset patched)
    uint8_t cond = (c == Cond::Lo) ? 0x3 : 0x2;            // LO=cc(3), HS=cs(2)
    fixups_[fixupCount_++] = {len_, l, static_cast<uint8_t>(1u | (cond << 4))};
    emit32(0x54000000u | cond);
}
void HostAssembler::call(Reg d, Reg a, const void* fn) {
    // Preserve EVERY register that may hold a live value across the call: the host args
    // (x0/x1/x2), the link register x30 (blr overwrites it; our function is a leaf), and the
    // whole vreg scratch pool (x4-x7, x9-x14) — because a value computed before the call (e.g.
    // a first random16's result) can be live across a SECOND call. Saving the full pool makes
    // the live-vreg-across-call contract hold for any expression; it's a cold path (once per
    // call, not per pixel). 112-byte frame (7 pairs) keeps sp 16-aligned.
    emit32(0xa9b907e0u);   // stp x0, x1,  [sp, #-112]!
    emit32(0xa9017be2u);   // stp x2, x30, [sp, #16]
    emit32(0xa90217e4u);   // stp x4, x5,  [sp, #32]
    emit32(0xa9031fe6u);   // stp x6, x7,  [sp, #48]
    emit32(0xa9042be9u);   // stp x9, x10, [sp, #64]
    emit32(0xa90533ebu);   // stp x11,x12, [sp, #80]
    emit32(0xa9063bedu);   // stp x13,x14, [sp, #96]
    // arg into x0 (the built-in's first parameter)
    emit32(0xaa0003e0u | (uint32_t(mr(a)) << 16));        // mov x0, x<arg>
    // materialise the 64-bit absolute fn address into x15 (movz + 3×movk)
    uint64_t addr = reinterpret_cast<uint64_t>(fn);
    emit32(0xd2800000u | ((uint32_t(addr) & 0xffff) << 5) | 15);                 // movz x15, #b0
    emit32(0xf2800000u | (1u << 21) | (((uint32_t(addr >> 16)) & 0xffff) << 5) | 15);  // movk x15,#b1,lsl16
    emit32(0xf2800000u | (2u << 21) | (((uint32_t(addr >> 32)) & 0xffff) << 5) | 15);  // movk x15,#b2,lsl32
    emit32(0xf2800000u | (3u << 21) | (((uint32_t(addr >> 48)) & 0xffff) << 5) | 15);  // movk x15,#b3,lsl48
    emit32(0xd63f0000u | (15u << 5));                     // blr x15
    // Stash the result (x0) in x15 — a non-pool scratch — BEFORE restoring, since x0 and the
    // dst register are both in the saved set the restore overwrites.
    emit32(0xaa0003efu);   // mov x15, x0   (result → x15)
    // restore the full saved set (reverse order)
    emit32(0xa9463bedu);   // ldp x13,x14, [sp, #96]
    emit32(0xa94533ebu);   // ldp x11,x12, [sp, #80]
    emit32(0xa9442be9u);   // ldp x9, x10, [sp, #64]
    emit32(0xa9431fe6u);   // ldp x6, x7,  [sp, #48]
    emit32(0xa94217e4u);   // ldp x4, x5,  [sp, #32]
    emit32(0xa9417be2u);   // ldp x2, x30, [sp, #16]
    emit32(0xa8c707e0u);   // ldp x0, x1,  [sp], #112
    // now move the stashed result into dst (dst is restored/valid; x15 holds the result)
    emit32(0xaa0f03e0u | uint32_t(mr(d)));   // mov x<dst>, x15
}
void HostAssembler::ret() { emit32(0xd65f03c0u); }

void HostAssembler::patchBranches() {
    for (uint8_t i = 0; i < fixupCount_; i++) {
        const Fixup& f = fixups_[i];
        int32_t target = labelPos_[f.label];
        int32_t rel = (target - static_cast<int32_t>(f.at)) >> 2;     // PC-relative, /4
        uint32_t w; std::memcpy(&w, buf_ + f.at, 4);
        w |= (uint32_t(rel) & 0x7ffff) << 5;                          // imm19 field (cbz & b.cond)
        std::memcpy(buf_ + f.at, &w, 4);
    }
}

#elif defined(__x86_64__)
#error "MoonLive host assembler: x86-64 backend not yet implemented (CI is arm64 for now)"
#else
#error "MoonLive host assembler: unsupported host ISA"
#endif

}  // namespace mm::moonlive
