#pragma once

#include "platform/platform.h"   // alloc/free — the emit buffer is heap, not stack

#include "core/moonlive/MoonLiveIr.h"   // kCodeCap — one cap for the staging buffer and every backend

#include <cstdint>
#include <cstddef>

// MoonLive RISC-V assembler (ESP32-P4 backend) — the device counterpart of the host/Xtensa
// MacroAssemblers, same named-instruction interface. RV32: fixed 4-byte instructions, a
// standard (non-windowed) call ABI — simpler than Xtensa. Branch displacements are back-patched
// against bound labels.
//
// Register map: R0..R3 → a0..a3 (the host args buf/nLights/cpl/t); R4.. → caller-saved temps
// (t0..t6, a4..a7). All in the caller-saved set, so call() saves the live pool explicitly.

namespace mm::moonlive {

// Twelve was the count every backend started with; RISC-V has room for more, and a nested loop
// needs it — two loop levels hold four values live, and a three-argument call needs three temps on
// top. Fourteen is what the CALLER-SAVED registers alone provide, and that is the whole map.
//
// It briefly reached eighteen by also mapping x18..x21 (s2..s5) on the reasoning that "the emitted
// routine is a leaf that saves what it uses". It does not: prologue() is empty, so the routine has
// no entry/exit save at all and would have returned to its caller with four callee-saved registers
// clobbered. Giving the routine a prologue would cost every script a save/restore it almost never
// needs; dropping the four costs nothing, since fourteen still exceeds Xtensa's twelve and no
// script measured here uses more than eleven.
enum Reg : uint8_t { R0 = 0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11,
                     R12, R13, kRegCount };
using Label = uint8_t;
enum class Cond : uint8_t { Lo /* unsigned < */, Hs /* unsigned >= */ };

class RiscvAssembler {
public:
    // The register type the shared lowering (core/moonlive/moonlive_lower.h) works in.
    // Named here because each backend's Reg is its own enum, sized to its own file.
    using RegType = Reg;

    // Owns buf_ (see below). Freed here, copying deleted — an emitter that was copied
    // would double-free the buffer it emits into.
    ~RiscvAssembler() { platform::free(buf_); }
    /// `cap` is the code buffer's size, chosen per SCRIPT by the caller (codeCapFor) rather
    /// than a shared constant — the backends differ by up to 1.9x on identical source, so one
    /// number cannot fit them all. Defaults to the sanity bound for callers that emit a fixed
    /// blob (emitFill) and have no token count to size from.
    explicit RiscvAssembler(size_t cap = kCodeCap)
        : kCap(cap), buf_(static_cast<uint8_t*>(platform::alloc(cap))) {}
    RiscvAssembler(const RiscvAssembler&) = delete;
    RiscvAssembler& operator=(const RiscvAssembler&) = delete;

    void finalize() { patchBranches(); }
    // Pad to the alignment a FUNCTION ENTRY needs, called before each prologue. Every instruction
    // on this ISA is four bytes, so a function boundary is always aligned already and this is a
    // no-op; it exists because the shared lowering calls it, and Xtensa (2- and 3-byte forms) does
    // need the pad. Not asserted here, because the property is about EMITTED code rather than this
    // function: the per-ISA test "every function in a class starts where a call can reach it" is
    // what would fail if the compressed (C) extension ever made a boundary land off four bytes.
    void alignForEntry() {}
    const uint8_t* bytes() const { return buf_; }
    size_t size() const { return len_; }
    bool overflowed() const { return overflow_; }

    // --- the call frame ---
    // The register allocator's overflow storage (core/moonlive/MoonLiveSpill.h). RV32 had no frame
    // at all outside call(), so a spilling program is the first thing here that needs one: prologue
    // opens it and parks s0 (the standard frame pointer) at its top, epilogue tears it down. Slots
    // are addressed from s0, NOT sp, because call() moves sp by 80 bytes around every host call —
    // sp-relative offsets would be wrong for its duration, and reading a spilled value after a
    // random16() is the ordinary case. It is also the layout a nested or recursive script function
    // needs: one s0 per activation.
    // slots == 0 emits nothing, so a non-spilling script keeps today's zero-instruction entry.
    void prologue(uint8_t slots = 0);
    void spillStore(Reg r, uint8_t slot);
    void spillLoad(Reg r, uint8_t slot);
    void slotAddr(Reg d, uint8_t slot);   // d = &frame[slot] — a call's argument block
    static constexpr uint8_t kMaxSpillSlots = kTotalSlots;   // parser/allocator range + the parked host args

    Label newLabel();
    void  bind(Label l);

    void movPtr(Reg d, const void* p);   // a full-width address into a register (ConstPtr)
    void movImm(Reg d, int32_t imm);     // li rd, imm  (addi rd, x0, imm)
    void movReg(Reg d, Reg a);           // mv rd, ra   (addi rd, ra, 0)
    void addImm(Reg d, Reg a, int32_t imm);   // addi rd, ra, imm
    void addReg(Reg d, Reg a, Reg b);    // add rd, ra, rb
    void mulReg(Reg d, Reg a, Reg b);    // mul rd, ra, rb
    void mulhi(Reg d, Reg a, Reg b);     // mulh rd, ra, rb — the SIGNED high 32 bits
    void shlImm(Reg d, Reg a, uint8_t n);// slli rd, ra, #n
    void sarImm(Reg d, Reg a, uint8_t n);// srai rd, ra, #n — arithmetic, sign-filling
    void shrImm(Reg d, Reg a, uint8_t n);// srli rd, ra, #n — logical, zero-filling
    void store8(Reg base, Reg off, Reg val);  // add tmp,base,off ; sb val,0(tmp)
    void load8(Reg d, Reg base, int32_t imm); // lbu rDst, imm(rBase) — a control read
    void load32(Reg d, Reg base, int32_t imm); // lw rDst, imm(rBase) — a whole 4-byte slot
    void store32(Reg base, int32_t imm, Reg val);// sw rVal, imm(rBase) (offset IMMEDIATE)
    void load32Idx(Reg d, Reg base, Reg off);  // add tmp,base,off ; lw d,0(tmp)
    void store32Idx(Reg base, Reg off, Reg val);// add tmp,base,off ; sw val,0(tmp)
    void load8Idx(Reg d, Reg base, Reg off);  // add tmp,base,off ; lbu d,0(tmp)
    void branchIfZero(Reg a, Label l);   // beqz a, l  (bge x0, a... use bgeu against x0)
    void branchGeU(Reg a, Reg b, Label l);    // bgeu a, b, l
    void branchGeS(Reg a, Reg b, Label l);    // bge  a, b, l
    void branchNe(Reg a, Reg b, Label l);     // bne a, b, l
    void call(Reg d, Reg a, Reg b, Reg c, const void* fn);  // standard call to a host built-in
    /// Call a function in THIS block, by label: the script-to-script call. `jal ra, off` links the
    /// return address in x1 and jumps; the callee's own prologue saves ra, so recursion works.
    /// Every caller vreg is preserved across it exactly as call() does for a builtin, and the
    /// callee's a0 is delivered into `d` when `take` is set.
    void callLabel(Label l, Reg d = R0, bool take = false);
    void epilogue();                     // undo prologue's frame (if any), then ret
    /// Park `a` where the ABI returns a value, so the host reads it after the call. The move
    /// happens BEFORE the epilogue's teardown: on a windowed or frame-pointer ABI the
    /// teardown is what makes the register the caller sees.
    void retValue(Reg a);
    void ret();

private:
    // The emitted-code buffer's size, fixed for this object's life but chosen per script.
    const size_t kCap;
    // Sized in core (kAsmLabels/kAsmFixups) so the three backends cannot drift apart.
    static constexpr uint8_t kMaxLabels = kAsmLabels;
    static constexpr uint8_t kMaxFixups = kAsmFixups;

    void emit32(uint32_t w);
    /// One conditional branch, as an inverted short branch over a `jal` (see the definition for
    /// why every conditional branch takes the two-word form).
    void branchRelaxed(uint8_t rs1, uint8_t rs2, uint8_t f3, Label l);
    // A pending reference to a label. `kind` distinguishes the B-type conditional branches from a
    // J-type `jal`: the two scatter their immediate into different bit fields, so patching one as
    // the other silently retargets it.
    enum class FixKind : uint8_t { Branch, Jal };
    void addFixup(size_t at, Label label, FixKind kind = FixKind::Branch);

    // HEAP, not a member array: the assembler is a stack local in lowerToBytes, so a kCap-sized
    // member put 2 KB on the compile chain's stack — on top of the staging buffer and the parser
    // frames. On a classic ESP32 that overflowed the task and faulted inside _xt_context_save
    // (the plan named this: "buf_[kCap] inside the assembler, itself a stack local"). The buffer is
    // scratch that ends in a memcpy to the caller's output, so nothing outlives the object.
    uint8_t* buf_;
    size_t   len_ = 0;
    bool     overflow_ = false;
    // Frame size in bytes, 0 when no prologue was emitted. epilogue() reads it, so the teardown can
    // never disagree with the setup about how far sp moved — a mismatch there returns to a corrupted
    // stack, which looks like anything except the compiler bug it is.
    uint16_t frameBytes_ = 0;

    int32_t  labelPos_[kMaxLabels];
    uint8_t  labelCount_ = 0;
    struct Fixup { size_t at; Label label; FixKind kind = FixKind::Branch; };   // 4 bytes at `at`
    Fixup    fixups_[kMaxFixups];
    uint8_t  fixupCount_ = 0;

    void patchBranches();
};

}  // namespace mm::moonlive
