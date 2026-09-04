#include "moonlive_asm_host.h"
#include "core/moonlive/moonlive_lower.h"   // the one IR walk, shared by every backend

#include <cstring>

// MoonLive arm64 host assembler (Apple Silicon, arm64 Linux). Each named instruction is encoded
// ONCE here; the shared IR lowering composes them. Branch displacements are resolved by
// patchBranches() against bound labels, so no offset is ever hand-computed.
//
// One ISA per file, self-guarding, the shape the ESP32 backends already use: the file is always
// compiled and its body disappears on a host this is not. It also carries its own lowerToBytes,
// which is the two-line binding of core's IR walk to THIS assembler and can live nowhere else.

namespace mm::moonlive {

#if defined(__aarch64__) && !defined(MM_MOONLIVE_FORCE_NO_HOST_JIT)

// arm64 register map: R0..R4 = the host-ABI arg registers x0..x4 (buf, nLights, cpl, t, ctrls —
// the control-values arena pointer, kArg4). R5..R13 = caller-saved scratch x9..x14 then x5..x7.
// Index math uses the 64-bit views (xN) for addresses, 32-bit (wN) for counters/colors — same
// register number, so one map suffices. x15 is the call() address/immediate scratch (not a vreg).
static constexpr uint8_t kArm64Reg[kRegCount] = {0, 1, 2, 3, 4, 9, 10, 11, 12, 13, 14, 5, 6, 7};
// BOUNDS-CHECKED. The inline ops address their scratch as `vregsUsed + n`, so an index one past the
// map is reachable whenever the reservation and the map disagree — and an out-of-bounds read returns
// whatever byte follows the array, making the emitted instruction name a register chosen by
// accident. Clamping turns that into a wrong-but-safe register instead of undefined behaviour; the
// static_assert below and the lowerer's reservation are what stop it happening at all.
static uint8_t mr(Reg r) { return kArm64Reg[r < kRegCount ? r : kRegCount - 1]; }

// A scratch register that is ALSO a vreg silently corrupts values — see the RISC-V backend, where
// kScratchFn aliased vreg R12 and every call returned a stale value. Checked here so the map can
// never grow over a scratch.
constexpr bool armScratchOutsideMap() {
    constexpr uint8_t scratch[] = {15, 16, 17};
    for (uint8_t r : kArm64Reg) for (uint8_t s : scratch) if (r == s) return false;
    return true;
}
static_assert(armScratchOutsideMap(), "a scratch register is also a vreg — calls will corrupt it");


Label HostAssembler::newLabel() {
    if (labelCount_ == 0) for (auto& p : labelPos_) p = -1;
    if (labelCount_ >= kMaxLabels) { overflow_ = true; return 0; }   // same overflow signal as emit32
    Label l = labelCount_++;
    labelPos_[l] = -1;
    return l;
}
void HostAssembler::bind(Label l) { if (l < kMaxLabels) labelPos_[l] = static_cast<int32_t>(len_); }

// Record a pending branch fixup, guarding the fixed table — a script with too many branches sets
// overflow_ rather than writing past fixups_ (the same failure path as a full code buffer).
void HostAssembler::addFixup(size_t at, Label label, FixKind kind) {
    if (fixupCount_ >= kMaxFixups) { overflow_ = true; return; }
    fixups_[fixupCount_++] = {at, label, kind};
}

void HostAssembler::emit32(uint32_t w) {
    if (!buf_ || len_ + 4 > kCap) { overflow_ = true; return; }
    buf_[len_++] = uint8_t(w); buf_[len_++] = uint8_t(w >> 8);
    buf_[len_++] = uint8_t(w >> 16); buf_[len_++] = uint8_t(w >> 24);
}
void HostAssembler::emitBytes(const uint8_t* p, size_t n) {
    if (!buf_ || len_ + n > kCap) { overflow_ = true; return; }
    std::memcpy(buf_ + len_, p, n); len_ += n;
}

// --- the call frame: the register allocator's overflow storage ---------------------------------
//
// x29 is the AAPCS frame pointer, callee-saved and outside both the vreg map and the {x15,x16,x17}
// scratch set, so nothing this backend emits can disturb it. Slots are addressed from x29 rather
// than sp precisely because call() moves sp by 128 bytes around every host call: sp-relative offsets
// would be wrong for the duration of the call, and a script whose spilled value is read after a
// random16() is the ordinary case, not an exotic one. It is also the layout a nested call needs —
// each activation gets its own x29 — which is why the frame pointer is here now rather than added
// later when script-defined functions arrive.
//
// Layout: [x29+0] = saved x29, [x29+8] = saved x30, slot n at [x29 + 16 + n*8].
static constexpr uint16_t kSlotBase = 16;

void HostAssembler::prologue(uint8_t slots) {
    if (slots == 0) return;                       // no spilling: no frame, no cost
    if (slots > kMaxSpillSlots) { overflow_ = true; return; }
    // 16-byte aligned, as the AAPCS requires of sp at every instruction boundary — an unaligned sp
    // faults on the first stp a callee executes, which would surface as a crash inside random16.
    const uint16_t bytes = static_cast<uint16_t>((kSlotBase + slots * 8 + 15) & ~15);
    frameBytes_ = bytes;
    emit32(0xa9800000u | ((uint32_t((-int32_t(bytes)) / 8) & 0x7f) << 15) | (30u << 10) | (31u << 5) | 29u);
    emit32(0x910003fdu);                          // mov x29, sp
}
void HostAssembler::epilogue() {
    if (frameBytes_ != 0) {
        emit32(0x910003bfu);                      // mov sp, x29   (drop anything call() left behind)
        emit32(0xa8c00000u | ((uint32_t(frameBytes_) / 8) << 15) | (30u << 10) | (31u << 5) | 29u);
    }
    ret();
}
// str/ldr with a 12-bit SCALED unsigned offset (imm12 counts 8-byte units for the 64-bit form).
// 64-bit, not 32: a vreg can hold a pointer — kArg0 is the buffer — and truncating one to 32 bits on
// the way to a slot would produce a wild store the moment a spilled pointer came back.
void HostAssembler::spillStore(Reg r, uint8_t slot) {
    // No frame means prologue() bailed; emitting would address the CALLER's stack.
    if (slot >= kMaxSpillSlots || frameBytes_ == 0) { overflow_ = true; return; }
    emit32(0xf9000000u | ((uint32_t(kSlotBase + slot * 8) / 8) << 10) | (29u << 5) | mr(r));
}
void HostAssembler::spillLoad(Reg r, uint8_t slot) {
    // No frame means prologue() bailed; emitting would address the CALLER's stack.
    if (slot >= kMaxSpillSlots || frameBytes_ == 0) { overflow_ = true; return; }
    emit32(0xf9400000u | ((uint32_t(kSlotBase + slot * 8) / 8) << 10) | (29u << 5) | mr(r));
}

// The ADDRESS of a frame slot, for a host call that reads its arguments from the frame. The slots
// are already where the arguments live; a call passes where they start rather than the values, which
// is what makes the number of arguments a memory question instead of a register one.
void HostAssembler::slotAddr(Reg d, uint8_t slot) {
    // No frame means prologue() bailed; emitting would address the CALLER's stack.
    if (slot >= kMaxSpillSlots || frameBytes_ == 0) { overflow_ = true; return; }
    const uint32_t off = kSlotBase + uint32_t(slot) * 8;
    emit32(0x91000000u | (off << 10) | (29u << 5) | mr(d));   // add xD, x29, #off
}

void HostAssembler::movImm(Reg d, int32_t imm) {
    // movz builds a ZERO-extended 16-bit constant, so a negative immediate would land as its
    // unsigned counterpart (-1 as 65535). The compiler emits Const(-1) to express subtraction —
    // `a - b` is `a + (b * -1)` — and a wrapped -1 makes every subtraction correct only modulo 256.
    // In a stored colour byte that is invisible; in a bounds-guarded index it silently drops the
    // light, and in a host-call argument it is nonsense. movn is the negative form: it writes
    // ~imm16, so movn #(~imm) materialises the true negative value.
    if (imm < 0) {
        // movn writes ~imm16, reaching -65536..-1 in one instruction. Below that, movk patches
        // the high half over it: movn seeds every bit set, so only the two 16-bit fields need
        // stating. This used to overflow_ instead — the compiler's Const never went that low
        // until a fixed literal could ride one.
        const uint32_t u = uint32_t(imm);
        emit32(0x12800000u | ((uint32_t(~imm) & 0xffff) << 5) | mr(d));   // movn wD, #~imm16 (low)
        if (imm < -65536)
            emit32(0x72a00000u | (((u >> 16) & 0xffff) << 5) | mr(d));    // movk wD, #hi16, lsl 16
        return;
    }
    emit32(0x52800000u | ((uint32_t(imm) & 0xffff) << 5) | mr(d));        // movz wD, #imm16
    if (uint32_t(imm) > 0xffffu)
        // The high half, patched over the movz. Without this every constant above 65535 silently
        // materialized as its low 16 bits — invisible while the language capped literals there,
        // and the first thing a Q16.16 literal (2.0 is 131072) stepped on.
        emit32(0x72a00000u | ((uint32_t(imm) >> 16) << 5) | mr(d));       // movk wD, #hi16, lsl 16
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
// smull xD, wA, wB then lsr xD, xD, #32 — the signed 64-bit product's high word. arm64 also has
// smulh, but that is a 64x64 form: with 32-bit vregs, widening the multiply is both correct and
// one instruction shorter than sign-extending first.
void HostAssembler::mulhi(Reg d, Reg a, Reg b) {
    emit32(0x9b207c00u | (mr(b) << 16) | (mr(a) << 5) | mr(d));   // smull xD, wA, wB
    emit32(0xd360fc00u | (mr(d) << 5) | mr(d));                   // lsr  xD, xD, #32
}
// lsl wD, wA, #n is an alias of ubfm; asr wD, wA, #n of sbfm. Both take the 32-bit immr/imms
// form, which is why the width bit (31) stays clear here.
void HostAssembler::shlImm(Reg d, Reg a, uint8_t n) {
    const uint32_t immr = (32u - n) & 31u, imms = 31u - n;
    emit32(0x53000000u | (immr << 16) | (imms << 10) | (mr(a) << 5) | mr(d));
}
void HostAssembler::sarImm(Reg d, Reg a, uint8_t n) {
    emit32(0x13000000u | (uint32_t(n) << 16) | (31u << 10) | (mr(a) << 5) | mr(d));
}
// lsr is ubfm with imms fixed at 31: the same shape as asr but zero-filling.
void HostAssembler::shrImm(Reg d, Reg a, uint8_t n) {
    emit32(0x53000000u | (uint32_t(n) << 16) | (31u << 10) | (mr(a) << 5) | mr(d));
}
void HostAssembler::store8(Reg base, Reg off, Reg val) {   // strb wVal, [xBase, xOff]
    emit32(0x38206800u | (mr(off) << 16) | (mr(base) << 5) | mr(val));
}
void HostAssembler::load8(Reg d, Reg base, int32_t imm) {  // ldrb wDst, [xBase, #imm12]
    emit32(0x39400000u | ((uint32_t(imm) & 0xfff) << 10) | (mr(base) << 5) | mr(d));
}
// The 4-byte slot access. ldr/str with a 32-bit w destination: the immediate is scaled by 4
// (every arena offset is a multiple of it), and the register-offset forms use the LSL-0 option.
void HostAssembler::load32(Reg d, Reg base, int32_t imm) {
    emit32(0xb9400000u | (((uint32_t(imm) >> 2) & 0xfff) << 10) | (mr(base) << 5) | mr(d));
}
void HostAssembler::store32(Reg base, int32_t imm, Reg val) {
    emit32(0xb9000000u | (((uint32_t(imm) >> 2) & 0xfff) << 10) | (mr(base) << 5) | mr(val));
}
void HostAssembler::load32Idx(Reg d, Reg base, Reg off) {
    emit32(0xb8606800u | (mr(off) << 16) | (mr(base) << 5) | mr(d));
}
void HostAssembler::store32Idx(Reg base, Reg off, Reg val) {
    emit32(0xb8206800u | (mr(off) << 16) | (mr(base) << 5) | mr(val));
}
// ldrb wDst, [xBase, xOff] and ldrh wDst, [xBase, xOff]. The register-offset form takes the index
// UNSCALED for a byte; for a halfword the LSL amount would scale it, and it is left at 0 so the
// index the caller passes is a BYTE offset in both cases. That keeps one rule for the lowering:
// an element index is multiplied by the element width before it gets here, never after.
void HostAssembler::load8Idx(Reg d, Reg base, Reg off) {   // ldrb wDst, [xBase, xOff]
    emit32(0x38606800u | (mr(off) << 16) | (mr(base) << 5) | mr(d));
}
void HostAssembler::cmp(Reg a, Reg b) {                    // cmp wA, wB  (subs wzr, wA, wB)
    emit32(0x6b00001fu | (mr(b) << 16) | (mr(a) << 5));
}
void HostAssembler::branchIfZero(Reg a, Label l) {         // cbz wA, l  (offset patched)
    addFixup(len_, l, FixKind::Branch);
    emit32(0x34000000u | mr(a));
}
void HostAssembler::branchIf(Cond c, Label l) {            // b.cond l  (offset patched)
    // arm64 condition codes: NE=1, HS/CS=2, LO/CC=3, GE=10.
    // Every enumerator is listed rather than falling through to a default: an unhandled one would
    // emit a plausible branch with the WRONG condition, which runs and does the opposite thing.
    const uint8_t cond = (c == Cond::Lo) ? 0x3 : (c == Cond::Ne) ? 0x1
                       : (c == Cond::Ge) ? 0xa : 0x2;
    addFixup(len_, l, FixKind::Branch);   // the condition is already in the instruction
    emit32(0x54000000u | cond);
}
// The fused forms the shared lowering calls. arm64 has no compare-and-branch pair, so each is
// cmp + b.cond here, and one instruction on RISC-V and Xtensa. Both spellings live behind the
// same name, which is what lets the IR walk be written once.
void HostAssembler::movReg(Reg d, Reg a) { addImm(d, a, 0); }    // mov wD, wA (add wD, wA, #0)

// The AAPCS64 return register is x0, which is ALSO vreg R0 (the buf argument): a script that
// returns while R0 still holds buf would emit `mov x0, x0`, which is correct and free. Emitted as
// a 64-bit move rather than a 32-bit one so a returned POINTER (tags() returns a string) keeps its
// top half; a numeric return is unaffected because the host reads it as a uintptr_t either way.
void HostAssembler::retValue(Reg a) {
    emit32(0xaa0003e0u | (uint32_t(mr(a)) << 16));   // mov x0, x<a>
}
void HostAssembler::branchGeU(Reg a, Reg b, Label l) { cmp(a, b); branchIf(Cond::Hs, l); }
void HostAssembler::branchGeS(Reg a, Reg b, Label l) { cmp(a, b); branchIf(Cond::Ge, l); }
void HostAssembler::branchNe(Reg a, Reg b, Label l)  { cmp(a, b); branchIf(Cond::Ne, l); }

// movPtr: a full 64-bit address into a register, movz + three movk.
//
// The same four instructions call() emits for its target, parameterized on the destination. A
// pointer cannot ride an immediate (IrInst::imm is int32_t) and cannot be a PC-relative literal
// either, because the emitted block is copied to its final address after these bytes are built,
// so an absolute materialization is what stays correct across that move.
void HostAssembler::movPtr(Reg d, const void* p) {
    const uint64_t addr = reinterpret_cast<uint64_t>(p);
    const uint8_t r = mr(d);
    emit32(0xd2800000u | ((uint32_t(addr) & 0xffff) << 5) | r);                          // movz xD, #b0
    emit32(0xf2800000u | (1u << 21) | (((uint32_t(addr >> 16)) & 0xffff) << 5) | r);     // movk xD,#b1,lsl16
    emit32(0xf2800000u | (2u << 21) | (((uint32_t(addr >> 32)) & 0xffff) << 5) | r);     // movk xD,#b2,lsl32
    emit32(0xf2800000u | (3u << 21) | (((uint32_t(addr >> 48)) & 0xffff) << 5) | r);     // movk xD,#b3,lsl48
}

void HostAssembler::call(Reg d, Reg a, Reg b, Reg c, const void* fn) {
    // Preserve EVERY register that may hold a live value across the call: the host args
    // (x0/x1/x2/x3), the link register x30 (blr overwrites it; our function is a leaf), and the
    // whole vreg scratch pool (x4-x7, x9-x14) — because a value computed before the call (e.g.
    // a first random16's result) can be live across a SECOND call. Saving the full pool makes
    // the live-vreg-across-call contract hold for any expression; it's a cold path (once per
    // call, not per pixel). 128-byte frame (8 pairs) keeps sp 16-aligned.
    //
    // x3 is kArg3, the elapsed time — which scripts now read as the system variable `t`, so a
    // built-in clobbering it (legal for any callee under the AAPCS) would be a silent wrong-value
    // bug in any animated script that calls anything. Saved before it could become one. It pairs
    // with x8, which this backend never uses, because stp works on pairs.
    emit32(0xa9b807e0u);   // stp x0, x1,  [sp, #-128]!
    emit32(0xa9017be2u);   // stp x2, x30, [sp, #16]
    emit32(0xa90723e3u);   // stp x3, x8,  [sp, #112]
    emit32(0xa90217e4u);   // stp x4, x5,  [sp, #32]
    emit32(0xa9031fe6u);   // stp x6, x7,  [sp, #48]
    emit32(0xa9042be9u);   // stp x9, x10, [sp, #64]
    emit32(0xa90533ebu);   // stp x11,x12, [sp, #80]
    emit32(0xa9063bedu);   // stp x13,x14, [sp, #96]
    // args into x0/x1/x2 (the built-in's three parameters). Order matters: x0 is written first,
    // and a later source register could BE x0 — so read the sources before any of them is clobbered
    // by moving through a scratch that is outside the vreg pool.
    emit32(0xaa0003efu | (uint32_t(mr(a)) << 16));        // mov x15, x<a>
    emit32(0xaa0003f0u | (uint32_t(mr(b)) << 16));        // mov x16, x<b>
    emit32(0xaa0003f1u | (uint32_t(mr(c)) << 16));        // mov x17, x<c>
    emit32(0xaa0f03e0u);                                   // mov x0, x15
    emit32(0xaa1003e1u);                                   // mov x1, x16
    emit32(0xaa1103e2u);                                   // mov x2, x17
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
    emit32(0xa94723e3u);   // ldp x3, x8,  [sp, #112]
    emit32(0xa9463bedu);   // ldp x13,x14, [sp, #96]
    emit32(0xa94533ebu);   // ldp x11,x12, [sp, #80]
    emit32(0xa9442be9u);   // ldp x9, x10, [sp, #64]
    emit32(0xa9431fe6u);   // ldp x6, x7,  [sp, #48]
    emit32(0xa94217e4u);   // ldp x4, x5,  [sp, #32]
    emit32(0xa9417be2u);   // ldp x2, x30, [sp, #16]
    emit32(0xa8c807e0u);   // ldp x0, x1,  [sp], #128
    // now move the stashed result into dst (dst is restored/valid; x15 holds the result)
    emit32(0xaa0f03e0u | uint32_t(mr(d)));   // mov x<dst>, x15
}
void HostAssembler::ret() { emit32(0xd65f03c0u); }

// bl <label>: a call to a function in THIS block: the script-to-script call.
//
// `bl` links the return address into x30, which the callee's prologue saves into its own frame, so
// calls nest and therefore recurse.
//
// Pass the host arguments on (the contract is with IrOp::CallScript in core).
void HostAssembler::callLabel(Label l, Reg d, bool take) {
    // The same preservation call() gives a builtin: the whole vreg pool, the host args and x30 to
    // the stack, the result parked in x15 (outside the pool) across the restore. Without it a value
    // computed before the call and used after it, `a() + b()`, read the second call's result twice.
    emit32(0xa9b807e0u);   // stp x0, x1,  [sp, #-128]!
    emit32(0xa9017be2u);   // stp x2, x30, [sp, #16]
    emit32(0xa90723e3u);   // stp x3, x8,  [sp, #112]
    emit32(0xa90217e4u);   // stp x4, x5,  [sp, #32]
    emit32(0xa9031fe6u);   // stp x6, x7,  [sp, #48]
    emit32(0xa9042be9u);   // stp x9, x10, [sp, #64]
    emit32(0xa90533ebu);   // stp x11,x12, [sp, #80]
    emit32(0xa9063bedu);   // stp x13,x14, [sp, #96]
    // Reloading them is a NO-OP on this backend as long as R0..R4 map onto the ABI argument
    // registers x0..x4 and `bl` leaves them alone, which is why removing these four instructions
    // does not fail a single test here while the same omission crashes an S3. Emitted anyway, so
    // the contract is expressed rather than depending on that mapping staying true.
    for (uint8_t v = 0; v < kHostArgSlots; v++) spillLoad(static_cast<Reg>(v), hostArgSlot(v));
    addFixup(len_, l, FixKind::Call);
    emit32(0x94000000u);   // bl #0: the 26-bit imm is patched below
    if (take) emit32(0xaa0003efu);   // mov x15, x0   (result → x15, outside the pool)
    emit32(0xa94723e3u);   // ldp x3, x8,  [sp, #112]
    emit32(0xa9463bedu);   // ldp x13,x14, [sp, #96]
    emit32(0xa94533ebu);   // ldp x11,x12, [sp, #80]
    emit32(0xa9442be9u);   // ldp x9, x10, [sp, #64]
    emit32(0xa9431fe6u);   // ldp x6, x7,  [sp, #48]
    emit32(0xa94217e4u);   // ldp x4, x5,  [sp, #32]
    emit32(0xa9417be2u);   // ldp x2, x30, [sp, #16]
    emit32(0xa8c807e0u);   // ldp x0, x1,  [sp], #128
    if (take) emit32(0xaa0f03e0u | uint32_t(mr(d)));   // mov x<dst>, x15
}

void HostAssembler::patchBranches() {
    // Nothing was emitted if the buffer never allocated, so there is nothing to patch —
    // stated rather than left to the reader to derive from fixupCount_ being 0. And an
    // OVERFLOWED compile is refused by lowerWith after finalize(), so patching it is pointless —
    // and unsafe: a fixup recorded just before emit32 dropped its instruction points at the
    // buffer's end, and the memcpy below would write past buf_ (found as heap corruption on the
    // x86-64 backend; the pattern is identical here).
    if (!buf_ || overflow_) return;
    for (uint8_t i = 0; i < fixupCount_; i++) {
        const Fixup& f = fixups_[i];
        int32_t target = labelPos_[f.label];
        if (target < 0) continue;                                     // unbound label — leave the branch as-is (overflow_ already failed the compile)
        int32_t rel = (target - static_cast<int32_t>(f.at)) >> 2;     // PC-relative, /4
        uint32_t w; std::memcpy(&w, buf_ + f.at, 4);
        if (f.kind == FixKind::Call) {
            // bl: a 26-bit immediate at bits 0..25, not the 19-bit field the conditional branches
            // use. Sharing their arithmetic would silently retarget the call, which is why the
            // fixup carries a kind.
            if (rel < -33554432 || rel > 33554431) { overflow_ = true; return; }
            w |= uint32_t(rel) & 0x03ffffffu;
        } else {
            w |= (uint32_t(rel) & 0x7ffff) << 5;                      // imm19 field (cbz & b.cond)
        }
        std::memcpy(buf_ + f.at, &w, 4);
    }
}

size_t lowerToBytes(IrProgram& ir, uint8_t* out, size_t cap, const RegBudget* squeeze) {
    return lowerWith<HostAssembler>(ir, out, cap, squeeze, kRegCount);
}

#endif  // __aarch64__

}  // namespace mm::moonlive
