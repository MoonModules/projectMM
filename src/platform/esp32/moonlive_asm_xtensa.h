#pragma once

#include "platform/platform.h"   // alloc/free — the emit buffer is heap, not stack

#include "core/moonlive/MoonLiveIr.h"   // kCodeCap — one cap for the staging buffer and every backend

#include <cstdint>
#include <cstddef>

// MoonLive Xtensa assembler (ESP32 classic/S3 backend) — the device counterpart of the host
// MacroAssembler. Same named-instruction interface (the IR lowering is written once against
// it); the encodings and the windowed ABI are Xtensa-specific. Branch displacements are
// back-patched against bound labels, so no offset is hand-computed (the crash class the
// verbatim-blob spike avoided by never composing stays avoided by back-patching).
//
// Windowed ABI: the emitted routine opens with `entry` and returns with `retw.n`. The host
// args arrive in a2..a5 (buf, nLights, cpl, t); R0..R3 map to those, R4..R9 to a6..a11.

namespace mm::moonlive {

// Ten vregs, mapping to a2..a11. NOT a14/a15: with the windowed ABI a routine that opened its frame
// with `entry` returns through `retw.n`, which reads the caller's linkage out of the TOP of the
// window — so a12..a15 are not general registers here, they are the return path. Using a14/a15 as
// vregs (and restoring saved copies into them after a callx8) corrupted that linkage, and `retw.n`
// then returned to a garbage address: `Guru Meditation (IllegalInstruction)` the moment a scripted
// LAYOUT ran, because addLight is the call that made the window rotate. a12/a13 stay scratch.
enum Reg : uint8_t { R0 = 0, R1, R2, R3, R4, R5, R6, R7, R8, R9, kRegCount };
using Label = uint8_t;
enum class Cond : uint8_t { Lo /* unsigned < */, Hs /* unsigned >= */ };

/// The vreg → machine-register map, for the device-codegen test. a2..a11 only: a12/a13 are call
/// scratch and the store8 address register, and a14/a15 carry the routine's own retw.n linkage.
const uint8_t* xtRegMap(uint8_t& count);

class XtensaAssembler {
public:
    // The register type the shared lowering (core/moonlive/moonlive_lower.h) works in.
    // Named here because each backend's Reg is its own enum, sized to its own file.
    using RegType = Reg;

    // Owns buf_ (see below). Freed here, copying deleted — an emitter that was copied
    // would double-free the buffer it emits into.
    ~XtensaAssembler() { platform::free(buf_); }
    /// `cap` is the code buffer's size, chosen per SCRIPT by the caller (codeCapFor) rather
    /// than a shared constant — the backends differ by up to 1.9x on identical source, so one
    /// number cannot fit them all. Defaults to the sanity bound for callers that emit a fixed
    /// blob (emitFill) and have no token count to size from.
    explicit XtensaAssembler(size_t cap = kCodeCap)
        : kCap(cap), buf_(static_cast<uint8_t*>(platform::alloc(cap))) {}
    XtensaAssembler(const XtensaAssembler&) = delete;
    XtensaAssembler& operator=(const XtensaAssembler&) = delete;

    void finalize() { patchBranches(); }
    // Pad to the alignment a FUNCTION ENTRY needs on this ISA, called before each prologue.
    //
    // Xtensa requires it twice over: `entry` itself must sit on a 4-byte boundary (the toolchain
    // rejects anything else with "unaligned entry instruction"), and CALLn encodes its target as a
    // count of 4-byte units from the call's own PC rounded down, so an unaligned callee is not
    // expressible at all. Instructions here are 2 or 3 bytes, so a function that follows another
    // lands on an arbitrary offset and needs the pad. This is what `.align 4` does in hand-written
    // assembly; the fill is zeros, which is never executed because the preceding function's
    // `retw.n` is the last instruction reached.
    void alignForEntry() {
        while ((len_ & 3u) != 0) { const uint8_t z = 0; emit(&z, 1); }
    }
    const uint8_t* bytes() const { return buf_; }
    size_t size() const { return len_; }
    bool overflowed() const { return overflow_; }

    // --- the call frame ---
    // The register allocator's overflow storage (core/moonlive/MoonLiveSpill.h). Xtensa already has
    // a whole-routine frame from `entry a1, N`; prologue(slots) simply widens N to carry the spill
    // slots above the bytes call() uses, so a spilling script costs one larger immediate and no
    // extra instruction. slots == 0 keeps the frame exactly as it was, so nothing changes for a
    // script that did not spill. Slots are addressed from a1, which the windowed ABI preserves
    // across callx8 — the same property a nested or recursive script function will rely on.
    void prologue(uint8_t slots = 0);    // entry a1, N  (must be the first instruction)
    void spillStore(Reg r, uint8_t slot);
    void spillLoad(Reg r, uint8_t slot);
    void slotAddr(Reg d, uint8_t slot);   // d = &frame[slot] — a call's argument block
    static constexpr uint8_t kMaxSpillSlots = kTotalSlots;   // parser/allocator range + the parked host args

    Label newLabel();
    void  bind(Label l);

    void movPtr(Reg d, const void* p);   // a full-width address into a register (ConstPtr)
    void movImm(Reg d, int32_t imm);     // movi aD, #imm (0..255)
    void movReg(Reg d, Reg a);           // mov.n aD, aA
    void addImm(Reg d, Reg a, int32_t imm);   // addi.n aD, aA, #imm (1..15)
    void addReg(Reg d, Reg a, Reg b);    // add.n aD, aA, aB
    void mulReg(Reg d, Reg a, Reg b);    // mull aD, aA, aB
    void mulhi(Reg d, Reg a, Reg b);     // mulsh aD, aA, aB — the SIGNED high 32 bits
    void shlImm(Reg d, Reg a, uint8_t n);// slli aD, aA, #n (1..31)
    void sarImm(Reg d, Reg a, uint8_t n);// srai aD, aA, #n (0..31), arithmetic
    void shrImm(Reg d, Reg a, uint8_t n);// LOGICAL right shift (srli / extui)
    void store8(Reg base, Reg off, Reg val);  // s8i via computed address (add then s8i,0)
    void load8(Reg d, Reg base, int32_t imm); // l8ui aDst, aBase, #imm — a control read
    void load32(Reg d, Reg base, int32_t imm); // l32i.n aDst, aBase, #imm — a whole 4-byte slot
    void store32(Reg base, int32_t imm, Reg val);// s32i.n aVal, aBase, #imm (offset IMMEDIATE)
    void load32Idx(Reg d, Reg base, Reg off);  // add.n tmp,base,off ; l32i.n d,tmp,0
    void store32Idx(Reg base, Reg off, Reg val);// add.n tmp,base,off ; s32i.n val,tmp,0
    void load8Idx(Reg d, Reg base, Reg off);  // add.n tmp,base,off ; l8ui d,tmp,0
    void branchIfZero(Reg a, Label l);   // beqz aA, l  (nLights==0 guard)
    void branchGeU(Reg a, Reg b, Label l);    // bgeu aA, aB, l  (Bounds: skip if a>=b)
    void branchGeS(Reg a, Reg b, Label l);    // bge  aA, aB, l  (a script's own comparison)
    void branchNe(Reg a, Reg b, Label l);     // bne aA, aB, l   (loop test)
    void call(Reg d, Reg a, Reg b, Reg c, const void* fn);  // windowed call8 to a host built-in
    /// Call a function in THIS block, by label: the script-to-script call.
    ///
    /// Much smaller than the host `call()` above: the arguments are already in frame slots, so
    /// there is no staging, and the target is a label, so there is no address to build.
    ///
    /// CALL8, not call0. `call0` was the first choice (cheaper, no window rotation) and is WRONG
    /// here: entry/retw and call0 are two different ABIs and do not mix. esp-idf's own abi_entry
    /// shows the split: the windowed path emits `entry sp, locsz`, the call0 path emits
    /// `addi sp, sp, -N` plus an explicit `s32i a0` to save the return address. This assembler
    /// emits entry/retw.n, so its callees are windowed routines and reaching one with call0 would
    /// hand it a frame it never allocated.
    ///
    /// The callee therefore owes the 32-byte window-save reserve like any other call8 frame, which
    /// per-function prologues already give it.
    ///
    /// Every caller vreg is preserved across the call exactly as call() does for a builtin, and the
    /// callee's return value is delivered into `d` when `take` is set. The window rotation is why
    /// that value arrives in the CALLER's a10: call8 rotates by 8, so the callee's a2 is this
    /// frame's a10.
    void callLabel(Label l, Reg d = R0, bool take = false);
    void epilogue();                     // retw.n
    /// Park `a` where the ABI returns a value, so the host reads it after the call. The move
    /// happens BEFORE the epilogue's teardown: on a windowed or frame-pointer ABI the
    /// teardown is what makes the register the caller sees.
    void retValue(Reg a);

private:
    // The emitted-code buffer's size, fixed for this object's life but chosen per script.
    const size_t kCap;
    // Sized in core (kAsmLabels/kAsmFixups) so the three backends cannot drift apart.
    static constexpr uint8_t kMaxLabels = kAsmLabels;
    static constexpr uint8_t kMaxFixups = kAsmFixups;

    void emit(const uint8_t* p, size_t n);
    void emit2(uint16_t w);              // narrow (16-bit) instruction
    void emit3(uint32_t w);              // wide (24-bit) instruction
    // A pending reference to a label. `kind` is needed now that not every fixup patches a `j`:
    // a script-to-script call is a `call8`, whose displacement is SCALED (four-byte units) and
    // sits in different bits. Patching one as the other retargets it silently, which is the
    // failure this discriminator exists to make impossible.
    enum class FixKind : uint8_t { Jump, Call };
    struct Fixup { size_t at; Label label; FixKind kind = FixKind::Jump; };
    void addFixup(size_t at, Label label, FixKind kind = FixKind::Jump);   // enqueue a fixup (bounds-checked)

    // HEAP, not a member array: the assembler is a stack local in lowerToBytes, so a kCap-sized
    // member put 2 KB on the compile chain's stack — on top of the staging buffer and the parser
    // frames. On a classic ESP32 that overflowed the task and faulted inside _xt_context_save
    // (the plan named this: "buf_[kCap] inside the assembler, itself a stack local"). The buffer is
    // scratch that ends in a memcpy to the caller's output, so nothing outlives the object.
    uint8_t* buf_;
    size_t   len_ = 0;
    bool     overflow_ = false;

    int32_t  labelPos_[kMaxLabels];
    uint8_t  labelCount_ = 0;
    Fixup    fixups_[kMaxFixups];
    uint8_t  fixupCount_ = 0;

    // A conditional branch emitted as inverted-condition-over-`j`, so its reach is the jump's
    // 18 bits rather than the branch's signed byte. See the .cpp for why that is not optional.
    void branchRelaxed(uint8_t condNibble, Reg a, Reg b, Label l);
    void patchBranches();
};

}  // namespace mm::moonlive
