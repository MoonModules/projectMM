#include "core/moonlive/moonlive_lower.h"   // the one IR walk, shared by every backend
#include "moonlive_asm_xtensa.h"

#include <cstring>

#if defined(__XTENSA__)   // the Xtensa assembler is only built for Xtensa targets

// MoonLive Xtensa assembler — named instructions encoded once, composed by the IR lowering.
// Encodings verified against xtensa-esp32s3-elf-as (see the plan / commit). Xtensa is
// little-endian with mixed 24-bit (wide) and 16-bit (narrow) instructions. The register
// convention: R0..R3 → a2..a5 (the windowed-ABI args buf/nLights/cpl/t), R4..R9 → a6..a11.
//
// Branch offset rule (verified): for the 8-bit-offset conditional branches we use, the offset
// byte = target - (branchInstrAddr + 4). All such branches put that byte at instrAddr+2, so one
// fixup kind covers them.

namespace mm::moonlive {

// R0..R3 → a2..a5 (the windowed-ABI args); R4..R11 → a6..a11, a14, a15. a12/a13 are internal
// scratch (store8 address, branchIfZero zero-reg, call result stash), so not in the pool.
static constexpr uint8_t kXtReg[kRegCount] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
// Map a vreg to its machine register. BOUNDS-CHECKED: the inline ops address their scratch as
// `vregsUsed + n`, which walks off the end of the map when a program uses the whole file — and an
// out-of-bounds read here returns whatever byte follows the array, so the emitted instruction names
// a register chosen by accident. That produced a12 (a call8 window register AND this assembler's own
// scratch) in a program that had no business touching it. Clamped to the last real entry so a
// mistake is a wrong-but-safe register rather than undefined behaviour; the static_assert below and
// the reservation in the lowerer are what keep it from happening at all.
static uint8_t ar(Reg r) { return kXtReg[r < kRegCount ? r : kRegCount - 1]; }

// The map, for the codegen test to assert against. Exposed rather than copied into the test, so the
// property being checked cannot drift away from the map it is about.
const uint8_t* xtRegMap(uint8_t& count) { count = kRegCount; return kXtReg; }

// A scratch register that is ALSO a vreg silently corrupts values — see the RISC-V backend, where
// kScratchFn aliased vreg R12 and every call returned a stale value. Checked here so the map can
// never grow over a scratch.
constexpr bool xtScratchOutsideMap() {
    // a12/a13 are call() scratch; a14/a15 carry the retw.n return linkage of our own `entry` frame.
    // Both classes are fatal as vregs, and the second one only faults once a script makes a CALL —
    // which is why it survived every effect and killed every layout.
    constexpr uint8_t scratch[] = {12, 13, 14, 15};
    for (uint8_t r : kXtReg) for (uint8_t s : scratch) if (r == s) return false;
    return true;
}
static_assert(xtScratchOutsideMap(), "a scratch or window register is also a vreg — calls corrupt it");


void XtensaAssembler::emit(const uint8_t* p, size_t n) {
    // !buf_ covers a failed allocation: the compile then fails cleanly at overflowed() instead
    // of writing through a null pointer.
    if (!buf_ || len_ + n > kCap) { overflow_ = true; return; }
    std::memcpy(buf_ + len_, p, n); len_ += n;
}
void XtensaAssembler::emit2(uint16_t w) {
    const uint8_t b[2] = {uint8_t(w), uint8_t(w >> 8)}; emit(b, 2);
}
void XtensaAssembler::emit3(uint32_t w) {
    const uint8_t b[3] = {uint8_t(w), uint8_t(w >> 8), uint8_t(w >> 16)}; emit(b, 3);
}

// entry a1, N — a 48-byte frame leaves room for the call8 window rotation (a routine with no
// call would be fine with 32, but 48 is harmless and lets any program call a built-in). call() uses
// bytes 16..39 of it, so the register allocator's spill slots start at 48 and the frame simply grows
// to hold them: on this ISA the whole-routine frame already exists, so spilling costs a bigger
// immediate on ONE instruction and nothing else. a1 is the frame pointer, and the windowed ABI
// preserves it across callx8 — which is why a slot read after a host call still finds its value, and
// why this addressing carries over unchanged when script functions start nesting frames.
// One word inside call()'s own save area for the return value: offsets 16..28 hold the saved
// vregs, 32 is free, and a register cannot hold a result across a window rotation.
static constexpr uint8_t  kResultSlot = 8;    // byte offset 32
static constexpr uint16_t kFrameBase  = 48;   // first byte past the bytes call() reserves
static constexpr uint16_t kSlotStride = 4;

// The SAVE AREAS the windowed ABI reserves at the TOP of every call8-making routine's frame.
// The window overflow handler (_WindowOverflow8, esp-idf components/xtensa/xtensa_vectors.S)
// writes two 16-byte bands there, addressed from two different stack pointers:
//   top 16 bytes    an OLDER frame's a0..a3, incl. its return address  (s32e aN, a9, -16..-4)
//   next 16 bytes   this routine's OWN a4..a7                          (s32e aN, a0, -32..-20)
// So the top 32 bytes are the hardware's, never ours. GCC obeys the same rule: every
// call8-making function it compiles gets a frame of locals plus exactly 32.
//
// Both bands were bench-found separately. Reserving nothing put the parked host arguments under
// the a0..a3 band: any script at all reset the board with IllegalInstruction and a DATA address
// in A0, the return address overwritten. Reserving only 16 left the highest slot (the parked
// arena pointer) under the a4..a7 band, which is written on ANY interrupt that lands while a
// host call is in flight. Fast leaf calls (random16) rarely coincided with one and worked; the
// deep libm chains of plasma (sin, beat) gave every tick a wide window to hit, and the arena
// came back as an expression temp: LoadProhibited, A11=0, EXCVADDR=1. The corruption is spatial,
// not timed, so no code sequence can dodge it; only the layout can. Never seen on RISC-V or
// arm64: no register window, no hardware-owned frame bytes.
//
// The size follows the WIDEST call this assembler emits, and is derived from that instruction
// below rather than written down twice: a wider call rotates the window further and its extra
// save area grows to match (call4 spills nothing extra, call8 spills a4..a7, call12 also
// a8..a11), so the reserve is 16 + 16 * (windows rotated - 1). A future callx12 with a hand-
// held 32 here would put the top slot back under the spill band and resurrect this bug with
// every static check still green.
static constexpr uint32_t kCallxOpcode = 0x0000e0u;   // callx8 a8, the one call we emit

/// Bytes the window-overflow handler may write at the top of a frame whose widest call is the
/// given CALLX. The window increment is bits 4..5 of the opcode (1 = call4, 2 = call8,
/// 3 = call12), and each step widens the rotation by four registers, so each adds another
/// 16-byte quad above the base save area every `entry` frame already owns. call0 (increment 0)
/// is not a case here: it makes no window rotation and this assembler cannot emit it.
static constexpr uint32_t windowSaveReserveFor(uint32_t callxOpcode) {
    return 16u * ((callxOpcode >> 4) & 0x3u);
}
static constexpr uint32_t kWindowSaveReserve = windowSaveReserveFor(kCallxOpcode);
static_assert(kWindowSaveReserve == 32,
              "callx width changed: the frame reserve moved with it, so re-check the frame "
              "layout and MM_ISA_RESERVED_TOP in the codegen test before accepting this");

// ENTRY is a BRI12-format instruction: op0=6, n=3, s=the base register, and the 12-bit immediate at
// bits 12..23 counts EIGHT-byte units. `entry a1, 48` is therefore 0x006136.
void XtensaAssembler::prologue(uint8_t slots) {
    if (slots > kMaxSpillSlots) { overflow_ = true; return; }
    // Rounded up to 8 because the immediate counts 8-byte units; the ABI additionally wants the
    // frame 16-byte aligned, and 48 + a multiple of 16 keeps that. kWindowSaveReserve is added ON
    // TOP of the slots so the highest slot still ends below the hardware's two save bands.
    const uint32_t bytes =
        (kFrameBase + uint32_t(slots) * kSlotStride + kWindowSaveReserve + 15u) & ~15u;
    emit3(0x000136u | ((bytes / 8u) << 12));
}
void XtensaAssembler::epilogue() { emit2(0xf01du); }     // retw.n

// s32i/l32i aR, a1, #off — the offset field counts 4-byte words, so a slot index maps straight onto
// it. No teardown counterpart: `entry`'s frame is released by retw.n, so unlike the RISC-V and arm64
// backends there is nothing for an epilogue to undo.
void XtensaAssembler::spillStore(Reg r, uint8_t slot) {
    if (slot >= kMaxSpillSlots) { overflow_ = true; return; }
    const uint8_t off4 = static_cast<uint8_t>((kFrameBase + slot * kSlotStride) / 4);
    const uint8_t b[3] = {uint8_t((ar(r) << 4) | 0x2), 0x61, off4};
    emit(b, 3);
}
void XtensaAssembler::spillLoad(Reg r, uint8_t slot) {
    if (slot >= kMaxSpillSlots) { overflow_ = true; return; }
    const uint8_t off4 = static_cast<uint8_t>((kFrameBase + slot * kSlotStride) / 4);
    const uint8_t b[3] = {uint8_t((ar(r) << 4) | 0x2), 0x21, off4};
    emit(b, 3);
}

Label XtensaAssembler::newLabel() {
    if (labelCount_ == 0) for (auto& p : labelPos_) p = -1;
    if (labelCount_ >= kMaxLabels) { overflow_ = true; return 0; }   // same overflow signal as emit
    Label l = labelCount_++; labelPos_[l] = -1; return l;
}
void XtensaAssembler::bind(Label l) { if (l < kMaxLabels) labelPos_[l] = static_cast<int32_t>(len_); }

// Record a pending branch fixup, guarding the fixed table (overflow_ rather than an OOB write).
void XtensaAssembler::addFixup(size_t at, Label label, FixKind kind) {
    if (fixupCount_ >= kMaxFixups) { overflow_ = true; return; }
    fixups_[fixupCount_++] = {at, label, kind};
}

// call8 <label>: a call to a function in THIS block: the script-to-script call.
//
// CALL (not CALLX): the target is a label, so the displacement is encoded in the instruction and
// patched below, with no address to build in a register. That is the whole reason a local call is a
// handful of bytes where the host call is forty lines.
//
// call8, not call0: this assembler emits entry/retw.n, the WINDOWED ABI, and call0 belongs to the
// other one (esp-idf's abi_entry shows the split: windowed emits `entry sp, locsz`, call0 emits an
// explicit sp adjust plus an s32i of a0). Reaching a windowed callee with call0 hands it a frame it
// never allocated. The callee owes the 32-byte window reserve like any other call8 frame, which its
// own per-function prologue provides.
void XtensaAssembler::callLabel(Label l) {
    // Pass the host arguments on (the contract is with IrOp::CallScript in core). The Xtensa
    // delta: call8 ROTATES the window by 8, so the callee's a2..a6 are this routine's a10..a14 and
    // the arguments are written to the OUTGOING window, not to a2..a6, which stay this frame's own.
    for (uint8_t v = 0; v < kHostArgSlots; v++) {
        const uint8_t off4 = static_cast<uint8_t>((kFrameBase + hostArgSlot(v) * kSlotStride) / 4);
        const uint8_t enc[3] = {static_cast<uint8_t>(((10 + v) << 4) | 0x2), 0x21, off4};
        emit(enc, 3);                                      // l32i a(10+v), a1, #slot
    }
    addFixup(len_, l, FixKind::Call);
    // CALL8 is format CALL: the low six bits are 0x25 (op0 = 5, n = 2) and an 18-bit signed offset
    // occupies bits 6..23, counting FOUR-BYTE UNITS from the call's PC rounded down to a 4-byte
    // boundary. Emitted as a placeholder and patched in patchBranches; verified against
    // xtensa-esp32-elf-as, which encodes `call8 target` at pc 6 with target 0 as a5 ff ff.
    const uint8_t enc[3] = {0x25, 0x00, 0x00};
    emit(enc, 3);
}

// movi aD, #imm. The narrow byte form carries only 0..255, so a wider constant (a uint16 like
// 65535) is built as hi8<<8 | lo8: movi aD,hi8 ; slli aD,aD,8 ; movi a13,lo8 ; add.n aD,aD,a13.
// a13 is the assembler's reserved scratch (also kZero in branchIfZero); it holds no live vreg.
// Single movi for the common 0..255 case. Without this, Const values >255 truncate to 8 bits.
// The ADDRESS of a frame slot, for a host call that reads its arguments from the frame. The slots
// already hold the arguments; the call passes where they start rather than the values, which is what
// makes the number of arguments a memory question instead of a register one.
void XtensaAssembler::slotAddr(Reg d, uint8_t slot) {
    if (slot >= kMaxSpillSlots) { overflow_ = true; return; }
    const uint32_t off = kFrameBase + uint32_t(slot) * kSlotStride;
    const uint8_t b[3] = {uint8_t((ar(d) << 4) | 0x2), uint8_t(0xc0 | 1), uint8_t(off)};
    emit(b, 3);                                            // addi aD, a1, #off
}

void XtensaAssembler::movImm(Reg d, int32_t imm) {
    const uint8_t dr = ar(d);
    // The wide `movi` field is 12-bit SIGNED (-2048..2047), which is the only encoding here that can
    // hold a negative constant. The compiler emits Const(-1) to express subtraction — `a - b` is
    // `a + (b * -1)` — and building that through the zero-extended byte path below would materialise
    // 65535, making every subtraction correct only modulo 256: invisible in a stored colour byte,
    // silently fatal in a bounds-guarded index (the light is dropped) or a host-call argument.
    // A negative below the 12-bit field's reach has no encoding here, and falling through to the
    // unsigned path below would materialise a different number in silence — the failure mode that
    // cost this backend a long debugging session. Fail the compile instead.
    // Outside every short encoding below, build the full 32-bit value the way movPtr does: the
    // same byte-at-a-time chain, absolute so it survives the block's copy to its final address.
    // The old positive path MASKED to 16 bits silently — invisible while the language capped
    // literals at 65535, and the first thing a Q16.16 literal (2.0 is 131072) stepped on.
    if (imm < -2048 || imm > 0xffff) {
        movPtr(d, reinterpret_cast<const void*>(static_cast<uintptr_t>(static_cast<uint32_t>(imm))));
        return;
    }
    if (imm < 0) {
        const uint32_t f = static_cast<uint32_t>(imm) & 0xfff;
        const uint8_t b[3] = {uint8_t((dr << 4) | 0x2),
                              uint8_t(0xa0 | ((f >> 8) & 0xf)),
                              uint8_t(f & 0xff)};
        emit(b, 3);                                                      // movi aD, #imm12
        return;
    }
    const uint32_t v = static_cast<uint32_t>(imm);
    if (v <= 0xff) {
        const uint8_t b[3] = {uint8_t((dr << 4) | 0x2), 0xa0, uint8_t(v)};
        emit(b, 3);
        return;
    }
    static constexpr uint8_t kTmp = 13;                                  // a13 (reserved scratch)
    const uint8_t hi[3] = {uint8_t((dr << 4) | 0x2), 0xa0, uint8_t(v >> 8)};
    emit(hi, 3);                                                         // movi aD, hi8
    const uint8_t sl[3] = {0x80, uint8_t((dr << 4) | dr), 0x11};
    emit(sl, 3);                                                         // slli aD, aD, 8
    const uint8_t lo[3] = {uint8_t((kTmp << 4) | 0x2), 0xa0, uint8_t(v & 0xff)};
    emit(lo, 3);                                                         // movi a13, lo8
    emit2(uint16_t((dr << 12) | (dr << 8) | (kTmp << 4) | 0xa));         // add.n aD, aD, a13
}
// movPtr: a 32-bit address into a register, a byte at a time.
//
// The same shape movImm uses for a 16-bit constant and call() uses for its target, extended to
// four bytes: movi the top byte, then three times (slli 8, movi the next byte into the scratch,
// add). a13 is this assembler's reserved scratch, outside the vreg map, so nothing live is
// disturbed.
//
// Byte-at-a-time rather than an l32r literal because a literal needs a pool at a known
// PC-relative distance, and this block is COPIED to its final address after these bytes are
// built. An absolute materialization survives that move; a PC-relative one would have to be
// re-based.
void XtensaAssembler::movPtr(Reg d, const void* p) {
    const uint32_t addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
    const uint8_t dr = ar(d);
    static constexpr uint8_t kTmp = 13;
    const uint8_t top[3] = {uint8_t((dr << 4) | 0x2), 0xa0, uint8_t(addr >> 24)};
    emit(top, 3);                                                        // movi aD, b3
    for (int shift = 16; shift >= 0; shift -= 8) {
        const uint8_t sl[3] = {0x80, uint8_t((dr << 4) | dr), 0x11};
        emit(sl, 3);                                                     // slli aD, aD, 8
        const uint8_t by[3] = {uint8_t((kTmp << 4) | 0x2), 0xa0, uint8_t((addr >> shift) & 0xff)};
        emit(by, 3);                                                     // movi a13, bN
        emit2(uint16_t((dr << 12) | (dr << 8) | (kTmp << 4) | 0xa));     // add.n aD, aD, a13
    }
}

// add.n aD, aA, aB : word (d<<12)|(a<<8)|(b<<4)|0xa
void XtensaAssembler::addReg(Reg d, Reg a, Reg b) {
    emit2(uint16_t((ar(d) << 12) | (ar(a) << 8) | (ar(b) << 4) | 0xa));
}
// mov.n aD, aA : bytes [ (d<<4)|0xd, a ]
void XtensaAssembler::movReg(Reg d, Reg a) {
    const uint8_t b[2] = {uint8_t((ar(d) << 4) | 0xd), ar(a)};
    emit(b, 2);
}

// The windowed ABI returns in a2, which is where R0 lives (R0..R3 map to a2..a5, the host
// arguments). So this is `mov.n a2, aX` and is free when the value is already in R0.
//
// BEFORE retw.n, not after: retw.n rotates the window back to the caller, and a move emitted after
// it would write a register the caller does not see. The lowering calls this then falls into the
// epilogue, which is the only order that works here.
void XtensaAssembler::retValue(Reg a) {
    if (a == R0) return;                    // already in a2
    movReg(R0, a);
}
// addi.n aD, aA, #imm : word (d<<12)|(a<<8)|(imm<<4)|0xb.
//
// The narrow form's 4-bit field encodes 1..15, and the bit pattern 0 means MINUS ONE, not zero.
// So a caller asking to add 0 must emit no add at all, and anything outside 1..15 needs the wide
// `addi` (8-bit signed) instead. Every caller passed a literal 1 until an array whose base offset
// is 0 asked for `+0`; that emitted `addi.n aX, aX, -1` and shifted every element access down a
// byte, which reached a device as a fixture that stayed dark while all host tests passed.
void XtensaAssembler::addImm(Reg d, Reg a, int32_t imm) {
    if (imm == 0) {
        if (ar(d) != ar(a)) movReg(d, a);      // still a move: d = a + 0
        return;
    }
    if (imm >= 1 && imm <= 15) {
        emit2(uint16_t((ar(d) << 12) | (ar(a) << 8) | ((imm & 0xf) << 4) | 0xb));
        return;
    }
    // addi aD, aA, #imm8 (RRI8, signed -128..127): bytes [ (d<<4)|2, 0xc0|a, imm ].
    const uint8_t b[3] = {uint8_t((ar(d) << 4) | 0x2), uint8_t(0xc0 | ar(a)),
                          uint8_t(imm & 0xff)};
    emit(b, 3);
}
// mull aD, aA, aB : 24-bit 0x820000 | (d<<12) | (a<<8) | (b<<4)
void XtensaAssembler::mulReg(Reg d, Reg a, Reg b) {
    emit3(0x820000u | (uint32_t(ar(d)) << 12) | (uint32_t(ar(a)) << 8) | (uint32_t(ar(b)) << 4));
}
// mulsh aD, aA, aB — the SIGNED high 32 bits of the product; with mull it gives the Q16.16
// multiply its middle 32 bits. MUL32_HIGH is present on LX6 and LX7.
//
// Every encoding here is an emit3 WORD, the same shape mull above uses. A first version built the
// memory bytes by hand from an objdump listing — and the two toolchains print differently:
// xtensa-esp32-elf-objdump shows the 24-bit word, xtensa-esp32s3-elf-objdump shows memory byte
// order. Reading word-hex as memory bytes reversed every instruction, and the reversed slli
// decoded as `l32r a1` — a stack-pointer clobber that hung the board hard enough for the system
// watchdog. The host tests can never execute these bytes; only a device shows it.
void XtensaAssembler::mulhi(Reg d, Reg a, Reg b) {
    emit3(0xb20000u | (uint32_t(ar(d)) << 12) | (uint32_t(ar(a)) << 8) | (uint32_t(ar(b)) << 4));
}
// slli aD, aA, #n : the field holds 32-n, split across bits 20-23 (high bit) and 4-7 (low
// nibble). n==0 is unencodable and the lowering never asks.
void XtensaAssembler::shlImm(Reg d, Reg a, uint8_t n) {
    // 1..31 only: the field holds 32-n, so n==0 and n>=32 have no encoding and would emit a
    // shift by some other amount. Refuse, the way shrImm below does.
    if (n == 0 || n >= 32) { overflow_ = true; return; }
    const uint32_t k = 32u - n;
    emit3(((k >> 4) << 20) | 0x010000u | (uint32_t(ar(d)) << 12) | (uint32_t(ar(a)) << 8) |
          ((k & 0x0fu) << 4));
}
// srai aD, aA, #n : arithmetic, sign-filling. The amount rides bits 8-11 (low nibble) and bit 20
// (high bit, folded into the 0x2/0x3 opcode nibble).
void XtensaAssembler::sarImm(Reg d, Reg a, uint8_t n) {
    if (n >= 32) { overflow_ = true; return; }   // the amount field is five bits
    emit3(((0x2u | (uint32_t(n) >> 4)) << 20) | 0x010000u | (uint32_t(ar(d)) << 12) |
          ((uint32_t(n) & 0x0fu) << 8) | (uint32_t(ar(a)) << 4));
}
// The LOGICAL right shift. srli only encodes 1..15; a shift of 16 is spelled extui aD, aA, 16, 16,
// which extracts the top 16 bits — between them they cover every shift the front end emits.
void XtensaAssembler::shrImm(Reg d, Reg a, uint8_t n) {
    if (n >= 1 && n <= 15) {
        emit3(0x410000u | (uint32_t(ar(d)) << 12) | (uint32_t(n) << 8) | (uint32_t(ar(a)) << 4));
        return;
    }
    // extui's width field caps at 16, so 16 is the only wide shift it can express. Anything else
    // has NO encoding here, and falling through to a shift-by-16 would emit a silently wrong
    // constant — the failure mode movImm above was just fixed for. Fail the compile instead.
    if (n != 16) { overflow_ = true; return; }
    emit3(0xf50000u | (uint32_t(ar(d)) << 12) | (uint32_t(ar(a)) << 4));   // extui aD, aA, 16, 16
}
// a12: the dedicated address scratch, OUTSIDE the R0..R9 -> a2..a11 vreg map, so computing an
// address into it can never clobber a live virtual register. Shared by every indexed access.
static constexpr uint8_t kAddrScratch = 12;   // a12

// The 4-byte slot access, in the NARROW forms: l32i.n / s32i.n are 2 bytes where l16ui was 3,
// and they cover offsets 0..60 in steps of 4 — every arena offset, since the arena is 64 bytes.
// RRRN format: imm/4 in the top nibble, then base, then the value/destination, then 0x8 (load)
// or 0x9 (store).
// l32i.n / s32i.n are the NARROW forms, and their offset field is FOUR BITS: it counts 4-byte
// words, so it reaches offset 60 and no further. Past that the field overflows into the neighbouring
// nibbles and the instruction silently addresses somewhere else entirely.
//
// That is not a theoretical bound. The control arena puts the script's members in [0, 64) and the
// HOST SYSTEM VARIABLES at 64 and above, so `width` (offset 64) encoded as 64/4 = 16, wrapped the
// 4-bit field to 0, and read the script's FIRST MEMBER instead. Every 2D script on every Xtensa
// board therefore saw width, height and depth as whatever its first member happened to hold,
// usually 0: `lines.mle` drew its green row at y=0 forever, and `fractal.mle` looped zero times and
// painted a band. It reproduced on an ESP32-S3 and a classic ESP32, was correct on RISC-V and on the
// host, and no test caught it because the host JIT is arm64 and the golden bytes never pinned a
// sys-var load.
//
// The wide RRI8 forms carry an 8-bit word-scaled offset (0..1020), which covers the whole arena, so
// they are used whenever the narrow one cannot reach. The narrow form is kept for what it does fit,
// since it is 2 bytes against 3 and a member access is the common case.
static constexpr int32_t kNarrowMax32 = 60;   // 4-bit field * 4-byte words

void XtensaAssembler::load32(Reg d, Reg base, int32_t imm) {
    if (imm >= 0 && imm <= kNarrowMax32 && (imm % 4) == 0) {
        emit2(uint16_t(((uint32_t(imm) / 4) << 12) | (uint32_t(ar(base)) << 8) |
                       (uint32_t(ar(d)) << 4) | 0x8));                       // l32i.n
        return;
    }
    // l32i aD, aBase, #off (RRI8, offset in 4-byte words). The byte layout is the one spillStore
    // uses: {(t << 4) | 0x2, (op << 4) | s, imm8}, where the SECOND byte packs the opcode nibble in
    // its high half and the base register in its low half. spillStore's literal 0x61 is exactly
    // that: opcode 6 (s32i) over base a1. Writing a bare register there drops the opcode.
    if (imm < 0 || imm / 4 > 255 || (imm % 4) != 0) { overflow_ = true; return; }
    const uint8_t b[3] = {uint8_t((ar(d) << 4) | 0x2), uint8_t((0x2 << 4) | ar(base)),
                          uint8_t(imm / 4)};
    emit(b, 3);
}
void XtensaAssembler::store32(Reg base, int32_t imm, Reg val) {
    if (imm >= 0 && imm <= kNarrowMax32 && (imm % 4) == 0) {
        emit2(uint16_t(((uint32_t(imm) / 4) << 12) | (uint32_t(ar(base)) << 8) |
                       (uint32_t(ar(val)) << 4) | 0x9));                     // s32i.n
        return;
    }
    // s32i aVal, aBase, #off (RRI8): opcode nibble 6 in the second byte's high half, where l32i
    // uses 2. The first byte's low nibble stays 0x2 (the RRI8 instruction group).
    if (imm < 0 || imm / 4 > 255 || (imm % 4) != 0) { overflow_ = true; return; }
    const uint8_t b[3] = {uint8_t((ar(val) << 4) | 0x2), uint8_t((0x6 << 4) | ar(base)),
                          uint8_t(imm / 4)};
    emit(b, 3);
}
// The indexed forms compute the address into a12 first, the same dedicated scratch the byte path
// uses: it sits outside the R0..R9 vreg map, so it never clobbers a live vreg.
void XtensaAssembler::load32Idx(Reg d, Reg base, Reg off) {
    emit2(uint16_t((kAddrScratch << 12) | (uint32_t(ar(base)) << 8) |
                   (uint32_t(ar(off)) << 4) | 0xa));                      // add.n a12, base, off
    emit2(uint16_t((uint32_t(kAddrScratch) << 8) | (uint32_t(ar(d)) << 4) | 0x8));
}
void XtensaAssembler::store32Idx(Reg base, Reg off, Reg val) {
    emit2(uint16_t((kAddrScratch << 12) | (uint32_t(ar(base)) << 8) |
                   (uint32_t(ar(off)) << 4) | 0xa));                      // add.n a12, base, off
    emit2(uint16_t((uint32_t(kAddrScratch) << 8) | (uint32_t(ar(val)) << 4) | 0x9));
}
// Xtensa s8i only offsets a base by an immediate (no register-offset store), so compute the
// address into a dedicated scratch a12 — OUTSIDE the R0..R9 → a2..a11 vreg map, so it never
// clobbers a live virtual register — then s8i aVal, a12, 0.
// add.n a12, aBase, aOff : (12<<12)|(base<<8)|(off<<4)|0xa  ;  s8i aVal, a12, 0 : [(val<<4)|2, 0x40|12, 0]
void XtensaAssembler::store8(Reg base, Reg off, Reg val) {
    emit2(uint16_t((kAddrScratch << 12) | (ar(base) << 8) | (ar(off) << 4) | 0xa));   // add.n a12, base, off
    const uint8_t b[3] = {uint8_t((ar(val) << 4) | 0x2), uint8_t(0x40 | kAddrScratch), 0x00};
    emit(b, 3);                                             // s8i aVal, a12, 0
}
// l8ui aDst, aBase, #imm (0..255) : bytes [ (dst<<4)|2, base, imm ] — zero-extended byte load.
void XtensaAssembler::load8(Reg d, Reg base, int32_t imm) {
    const uint8_t b[3] = {uint8_t((ar(d) << 4) | 0x2), ar(base), uint8_t(imm & 0xff)};
    emit(b, 3);
}


// Xtensa has no register-offset load either. The computed address goes through kAddrScratch, the
// same temp store8 uses, and the RRI8 offset is 0 so the offset scaling never applies.
void XtensaAssembler::load8Idx(Reg d, Reg base, Reg off) {
    emit2(uint16_t((kAddrScratch << 12) | (ar(base) << 8) | (ar(off) << 4) | 0xa));   // add.n a12, base, off
    const uint8_t b[3] = {uint8_t((ar(d) << 4) | 0x2), kAddrScratch, 0x00};           // l8ui d, a12, 0
    emit(b, 3);
}

// branchIfZero(a, l): synthesised as `movi a13,0; bgeu a13, a, l`. Unsigned 0 >= a is true
// IFF a == 0, so this branches exactly when a is zero — using only the verified bgeu 8-bit
// branch (no separate beqz form / offset width). a13 is a scratch outside the vreg map.
void XtensaAssembler::branchIfZero(Reg a, Label l) {
    static constexpr uint8_t kZero = 13;   // a13
    const uint8_t mv[3] = {uint8_t((kZero << 4) | 0x2), 0xa0, 0x00};   // movi a13, 0
    emit(mv, 3);
    // Same relaxed form as the other branches: bltu a13, a, +3 (the inverse of bgeu) over a `j`.
    const uint8_t br[3] = {uint8_t((ar(a) << 4) | 0x7), uint8_t((0x3 << 4) | kZero), 0x02};
    emit(br, 3);
    addFixup(len_, l);
    const uint8_t j[3] = {0x06, 0x00, 0x00};
    emit(j, 3);
}
// A conditional branch to `l`, emitted as the INVERTED condition over an unconditional jump:
//
//     b<inv> aA, aB, +3      ; skip the jump when the branch is NOT taken
//     j      l               ; 18-bit displacement — reaches anywhere in a script
//
// Xtensa's conditional branches carry a single SIGNED BYTE of displacement (±127), which a loop body
// outgrows easily once spill traffic is in it — grid.mll needed 177. Truncating that silently
// retargets the branch into the middle of the program, so the choice is relax or refuse. This is the
// textbook relaxation every compiler does (GCC and LLVM emit the short form and rewrite the ones that
// do not fit); the fixed six-byte form skips the iterate-to-convergence step, which is worth a few
// bytes per branch on a cold path in exchange for not having to reason about shifting offsets.
// The 3-byte `j` keeps its own fixup, and the inverted branch's +3 is already correct as emitted.
void XtensaAssembler::branchRelaxed(uint8_t condNibble, Reg a, Reg b, Label l) {
    // The inverted condition, skipping the 3-byte `j` that follows. Xtensa branch displacements are
    // relative to PC+4 (the same rule patchBranches uses), so clearing a 3-byte instruction is +2.
    // bne(0x9) <-> beq(0x1); bgeu(0xb) <-> bltu(0x3); bge(0xa) <-> blt(0x2).
    // Every nibble this is called with is listed: an unlisted one would take the final branch and
    // emit a WRONG condition rather than failing, and a mis-inverted branch is a program that runs
    // and does the opposite thing.
    const uint8_t inv = condNibble == 0x9 ? 0x1 : condNibble == 0x1 ? 0x9
                      : condNibble == 0xb ? 0x3 : condNibble == 0x3 ? 0xb
                      : condNibble == 0xa ? 0x2 : condNibble == 0x2 ? 0xa
                      : 0xb;
    const uint8_t br[3] = {uint8_t((ar(b) << 4) | 0x7), uint8_t((inv << 4) | ar(a)), 0x02};
    emit(br, 3);
    addFixup(len_, l);
    const uint8_t j[3] = {0x06, 0x00, 0x00};      // j — the 18-bit offset is patched in
    emit(j, 3);
}
// bgeu aA, aB, l  (skip if a >= b, unsigned)
void XtensaAssembler::branchGeU(Reg a, Reg b, Label l) { branchRelaxed(0xb, a, b, l); }
// bge aA, aB, l  (skip if a >= b, SIGNED). Same relaxation, one nibble apart from bgeu.
void XtensaAssembler::branchGeS(Reg a, Reg b, Label l) { branchRelaxed(0xa, a, b, l); }
// bne aA, aB, l
void XtensaAssembler::branchNe(Reg a, Reg b, Label l) { branchRelaxed(0x9, a, b, l); }

// Windowed call to a host built-in: d = fn(a). CALL8 rotates the window by 8, so the arg goes
// in a10 and the result returns in a10. The caller's a2..a7 are preserved by the window for
// free; only a8..a11 rotate out — and those hold vreg values that may be live across the call
// (a script with two random16 calls keeps the first result live across the second). So this
// SAVES a8/a9/a11 to the entry frame around the call (a10 carries the arg, then the result),
// mirroring the host backend's full-register-save. Cold path (once per call). The 48-byte
// frame from prologue() has room at offsets 16/20/28. The 32-bit fn address is built in a8
// byte-by-byte (movi/slli/add) — no l32r literal pool.
void XtensaAssembler::call(Reg d, Reg a, Reg b, Reg c, const void* fn) {
    // Save the rotate-out scratch a8, a9, a11 (a10 will carry arg→result).
    auto s32i = [&](uint8_t r, uint8_t off4){ const uint8_t enc[3]={uint8_t((r<<4)|2),0x61,off4}; emit(enc,3); };
    auto l32i = [&](uint8_t r, uint8_t off4){ const uint8_t enc[3]={uint8_t((r<<4)|2),0x21,off4}; emit(enc,3); };
    s32i(8, 4); s32i(9, 5); s32i(11, 7);                  // [a1+16]=a8, [a1+20]=a9, [a1+28]=a11
    // a14/a15 are deliberately NOT saved here, because they are no longer vregs (kXtReg): they carry
    // this routine's own return linkage for retw.n, so writing saved copies back into them after the
    // call is what broke every scripted layout. See the Reg enum for the failure that produced.
    s32i(10, 6);                                          // [a1+24]=a10 — a vreg (R8) call8 rotates out

    // The three args into a10/a11/a12 — call8 shifts the window by 8, so the callee reads them as
    // its a2/a3/a4. Moved HIGH-first (a12, then a11, then a10) so an earlier write cannot clobber a
    // source a later one still needs.
    //
    // argA goes through a13 first. a11 is vreg R9, so argA can BE a11 — and writing argB into a11
    // would then destroy argA before the a10 move reads it. High-first ordering alone does not cover
    // that case; a13 is outside the vreg map, so staging there does.
    emit2(uint16_t((uint32_t(ar(a)) << 8) | (13 << 4) | 0xd));   // mov a13, argA  (a13 is scratch)
    emit2(uint16_t((uint32_t(ar(c)) << 8) | (12 << 4) | 0xd));   // mov a12, argC
    emit2(uint16_t((uint32_t(ar(b)) << 8) | (11 << 4) | 0xd));   // mov a11, argB
    emit2(uint16_t((13u << 8) | (10 << 4) | 0xd));               // mov a10, a13

    // The fn address is assembled a byte at a time, which needs a TEMPORARY alongside a8, and the
    // choice of temporary is load-bearing, because call8 rotates the window: caller a8..a15 become
    // callee a0..a7. So caller a9 IS THE CALLEE'S STACK POINTER (a1). Building the address through a9
    // handed the callee a fragment of a function pointer as its sp; its own `entry a1, N` then sized a
    // frame from garbage and stored through it, landing anywhere in memory, including over this
    // routine's own parked host arguments. That is the null arena pointer a script later read
    // (LoadProhibited at EXCVADDR 0x1, arena register zero), and it only bit scripts whose loop makes
    // a call AND reads a system variable, because both halves have to be present to notice.
    //
    // a13 is the safe temporary: it maps to callee a5, the fourth argument slot, which a three-argument
    // host function never reads. It is already this assembler's kTmp for exactly this reason.
    uint32_t addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fn));
    auto moviA8  = [&](uint8_t v){ const uint8_t enc[3]={0x82,0xa0,v}; emit(enc,3); };
    auto moviA13 = [&](uint8_t v){ const uint8_t enc[3]={0xd2,0xa0,v}; emit(enc,3); };
    auto slliA8  = [&]{ const uint8_t enc[3]={0x80,0x88,0x11}; emit(enc,3); };
    auto addA8A13 = [&]{ emit2(0x88dau); };   // add.n a8, a8, a13 = (8<<12)|(8<<8)|(13<<4)|0xa
    moviA8(uint8_t(addr >> 24));
    slliA8(); moviA13(uint8_t(addr >> 16)); addA8A13();
    slliA8(); moviA13(uint8_t(addr >> 8));  addA8A13();
    slliA8(); moviA13(uint8_t(addr));       addA8A13();
    emit3(kCallxOpcode | (8u << 8));                       // callx8 a8  → result in a10
    // Park the result in the FRAME, not in a register.
    //
    // It used to be stashed in a12 — but call8 rotates the window by eight, so the callee's a4 IS
    // our a12: the callee overwrites the stash with its own second argument while it runs, and the
    // "result" moved to the destination afterwards is whatever the callee happened to leave there.
    // a12/a13 are safe as scratch only BEFORE the call, never across it. Measured: A0 = 0x100 in the
    // crash dump — nLights, a script value that reached the return-address register this way.
    // Slot kResultSlot sits in the bytes call() already owns, so this costs no extra frame.
    s32i(10, kResultSlot);                                 // [a1+kResultSlot*4] = result
    l32i(8, 4); l32i(9, 5); l32i(10, 6); l32i(11, 7);
    l32i(ar(d), kResultSlot);                              // dst = the parked result
}

void XtensaAssembler::patchBranches() {
    // Nothing was emitted if the buffer never allocated, so there is nothing to patch —
    // stated rather than left to the reader to derive from fixupCount_ being 0. And an
    // OVERFLOWED compile is refused after finalize(), so patching it is pointless — and unsafe:
    // a fixup recorded just before the emit dropped its instruction points at the buffer's end,
    // and patching there writes past buf_. Same shape on every backend.
    if (!buf_ || overflow_) return;
    for (uint8_t i = 0; i < fixupCount_; i++) {
        const Fixup& f = fixups_[i];
        if (labelPos_[f.label] < 0) continue;                                  // unbound label — leave as-is (overflow_ already failed the compile)
        uint32_t enc = 0;
        if (f.kind == FixKind::Jump) {
            // `j`: the displacement is relative to the byte AFTER the instruction and occupies bits
            // 6..23: eighteen signed bits, so it reaches any script the code buffer can hold.
            // Range-checked: refusing beats silently retargeting a jump.
            const int32_t off = labelPos_[f.label] - (static_cast<int32_t>(f.at) + 4);
            if (off < -131072 || off > 131071) { overflow_ = true; return; }
            enc = 0x06u | ((static_cast<uint32_t>(off) & 0x3ffffu) << 6);
        } else {
            // `call8`: the offset counts FOUR-BYTE UNITS from the call's PC rounded DOWN to a
            // 4-byte boundary, which is why it cannot share the jump's arithmetic. The ISA states
            // it per instruction ("the target instruction address must be a 32-bit aligned ENTRY
            // instruction"), and it is what buys CALLn its wider reach than a jump.
            //
            // The target is always aligned because alignForEntry() pads before every prologue, so
            // the check below is an assertion rather than a live path. It stays because the
            // alternative to noticing is a call that lands mid-instruction: `entry` at an odd
            // offset is not merely unreachable, it is rejected by the assembler outright
            // ("unaligned entry instruction"). CALLX8 has no such rule: it encodes a full address
            // in a register, which is why the host's call INTO a block always worked while a
            // block-internal call did not.
            const int32_t base = static_cast<int32_t>(f.at & ~size_t(3));
            const int32_t byteOff = labelPos_[f.label] - (base + 4);
            if ((byteOff & 3) != 0) { overflow_ = true; return; }
            const int32_t off = byteOff >> 2;
            if (off < -131072 || off > 131071) { overflow_ = true; return; }
            enc = 0x25u | ((static_cast<uint32_t>(off) & 0x3ffffu) << 6);   // 0x25 = op0 5, n 2
        }
        buf_[f.at + 0] = static_cast<uint8_t>(enc);
        buf_[f.at + 1] = static_cast<uint8_t>(enc >> 8);
        buf_[f.at + 2] = static_cast<uint8_t>(enc >> 16);
    }
}


// The two-line binding of core's IR walk to THIS assembler. It lives here rather than in its own
// file because a template instantiation can only exist where its argument does: `lowerToBytes` for
// xtensa is not separable from XtensaAssembler.
size_t lowerToBytes(IrProgram& ir, uint8_t* out, size_t cap, const RegBudget* squeeze) {
    return lowerWith<XtensaAssembler>(ir, out, cap, squeeze, kRegCount);
}

}  // namespace mm::moonlive

#endif  // __XTENSA__
