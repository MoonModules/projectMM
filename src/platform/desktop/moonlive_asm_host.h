#pragma once

#include "platform/platform.h"   // alloc/free — the emit buffer is heap, not stack

#include "core/moonlive/MoonLiveIr.h"   // kCodeCap — one cap for the staging buffer and every backend

#include <cstdint>
#include <cstddef>

// MoonLive host assembler (desktop backend) — a tiny named-instruction assembler for the
// host ISA (arm64 / x86-64), the textbook MacroAssembler shape (V8 Assembler, LLVM MCInst,
// asmjit). It appends one instruction at a time to a byte buffer and back-patches label
// offsets, so the IR→bytes lowering can COMPOSE a multi-op statement without hand-computing
// branch displacements (the crash class the verbatim-blob spike avoided by never composing).
//
// This header declares the neutral surface (register handles + the instruction methods); the
// per-ISA encodings live in moonlive_asm_host.cpp behind the platform boundary. The IR
// lowering (lowerToBytes) calls these methods; it never emits raw bytes itself.

namespace mm::moonlive {

// Abstract register handle — an index the assembler maps to a real machine register. The IR's
// virtual registers map onto these; the assembler owns the machine-register assignment so the
// IR stays ISA-neutral. R0..R4 alias the host-ABI argument registers (buf, nLights, cpl, t, and
// ctrls — the control-values arena pointer, kArg4, read by load8); R5+ are caller-saved scratch.
enum Reg : uint8_t { R0 = 0, R1, R2, R3, R4, R5, R6, R7, R8, R9,
                     R10, R11, R12, R13, kRegCount };

// A label is an index into the assembler's label table; bind() fixes its position, branches to
// it are back-patched when bound.
using Label = uint8_t;

// Branch condition (only the ones the IR needs so far).
enum class Cond : uint8_t { Lo /* unsigned < */, Hs /* unsigned >= */, Ne /* != */,
                            Ge /* SIGNED >= */ };

class HostAssembler {
public:
    // The register type the shared lowering (core/moonlive/moonlive_lower.h) works in.
    // Named here because each backend's Reg is its own enum, sized to its own file.
    using RegType = Reg;

    // Owns buf_ (see below). Freed here, copying deleted — an emitter that was copied
    // would double-free the buffer it emits into.
    ~HostAssembler() { platform::free(buf_); }
    /// `cap` is the code buffer's size, chosen per SCRIPT by the caller (codeCapFor) rather
    /// than a shared constant — the backends differ by up to 1.9x on identical source, so one
    /// number cannot fit them all. Defaults to the sanity bound for callers that emit a fixed
    /// blob (emitFill) and have no token count to size from.
    explicit HostAssembler(size_t cap = kCodeCap)
        : kCap(cap), buf_(static_cast<uint8_t*>(platform::alloc(cap))) {}
    HostAssembler(const HostAssembler&) = delete;
    HostAssembler& operator=(const HostAssembler&) = delete;

    // --- buffer ---
    // Resolve all branch fixups against bound labels, then expose the finished bytes. Call
    // once after the last instruction; bytes()/size() are valid only after finalize().
    void finalize() { patchBranches(); }
    // Pad to the alignment a FUNCTION ENTRY needs, called before each prologue. A no-op on both
    // host ISAs: arm64 instructions are all four bytes, so a boundary is aligned already, and
    // x86-64 has no entry-alignment requirement at all (a call reaches any byte). It exists
    // because the shared lowering calls it, and Xtensa (2- and 3-byte forms) does need the pad.
    // Not asserted here, because the property is about EMITTED code rather than this function:
    // the per-ISA test "every function in a class starts where a call can reach it" is what would
    // fail if a compressed encoding ever made a boundary land wrong.
    void alignForEntry() {}
    const uint8_t* bytes() const { return buf_; }
    size_t size() const { return len_; }
    bool overflowed() const { return overflow_; }

    // Byte-level append primitive — public so the x86-64 backend's file-scope encoding helpers
    // (variable-length instructions marshaled into small local buffers) can call it directly.
    // Sets overflowed() and drops the write if the buffer is full; arm64 uses it too for the
    // 4-byte emit32 shortcut. Owns bounds checking and the overflow flag — no other code path
    // writes into buf_.
    void emitBytes(const uint8_t* p, size_t n);

    // --- labels ---
    Label newLabel();
    void  bind(Label l);                 // mark l's position = current offset

    // --- the call frame ---
    // The register allocator's overflow storage (MoonLiveSpill.h). prologue() opens a frame with
    // room for `slots` spilled values and parks a frame pointer at its base; spillStore/spillLoad
    // address a slot as an offset from THAT pointer, never from sp — so a call() that moves sp
    // underneath them, and the nested/recursive calls MoonLive is gaining next, leave slot
    // addressing untouched. On arm64 slots == 0 emits nothing at all, so a script that never
    // spilled pays zero; x86-64 always opens a frame, because it has nonvolatile registers in its
    // vreg map to save and (on Win64) shadow space its callees are owed.
    void prologue(uint8_t slots);
    void epilogue();                     // tear the frame down, then ret
    /// Park `a` where the ABI returns a value, so the host reads it after the call. The move
    /// happens BEFORE the epilogue's teardown: on a windowed or frame-pointer ABI the
    /// teardown is what makes the register the caller sees.
    void retValue(Reg a);
    void spillStore(Reg r, uint8_t slot);
    void spillLoad(Reg r, uint8_t slot);
    void slotAddr(Reg d, uint8_t slot);   // d = &frame[slot] — a call's argument block
    static constexpr uint8_t kMaxSpillSlots = kTotalSlots;   // parser/allocator range + the parked host args   // what the frame below can address

    // --- instructions (named, register/immediate operands) ---
    void movPtr(Reg d, const void* p);   // a full-width address into a register (ConstPtr)
    void movImm(Reg d, int32_t imm);     // d = imm
    void addImm(Reg d, Reg a, int32_t imm);   // d = a + imm
    void addReg(Reg d, Reg a, Reg b);    // d = a + b
    void mulImm(Reg d, Reg a, int32_t imm);   // d = a * imm  (index scaling by a constant)
    void mulReg(Reg d, Reg a, Reg b);    // d = a * b   (index scaling by a runtime cpl)
    void mulhi(Reg d, Reg a, Reg b);     // d = the SIGNED high 32 bits of a * b (Q16.16 multiply)
    void shlImm(Reg d, Reg a, uint8_t n);// d = a << n
    void sarImm(Reg d, Reg a, uint8_t n);// d = a >> n, ARITHMETIC (sign-filling)
    void shrImm(Reg d, Reg a, uint8_t n);// d = a >> n, LOGICAL (zero-filling)
    void store8(Reg base, Reg off, Reg val);  // byte store: base[off] = val (low 8 bits)
    void load8(Reg d, Reg base, int32_t imm); // d = base[imm] (zero-extended byte) — control read
    void load32(Reg d, Reg base, int32_t imm); // d = base[imm..imm+3] — a whole 4-byte slot
    void store32(Reg base, int32_t imm, Reg val);// base[imm..imm+3] = val (offset IMMEDIATE)
    void load32Idx(Reg d, Reg base, Reg off);  // d = base[off..off+3], index in a REG
    void store32Idx(Reg base, Reg off, Reg val);// base[off..off+3] = val, index in a REG
    void load8Idx(Reg d, Reg base, Reg off);  // d = base[off] (zero-extended byte), index in a REG
    void movReg(Reg d, Reg a);           // d = a
    void branchIfZero(Reg a, Label l);   // if a == 0 goto l
    // The FUSED compare-and-branch forms, which is how the shared lowering spells a conditional.
    // arm64 has no fused branch, so these emit cmp + b.cond; RISC-V and Xtensa have the single
    // instruction. Naming the operation rather than the flags is what lets one lowering serve all
    // three: a backend that needs two instructions hides that here, where the encoding already is.
    void branchGeU(Reg a, Reg b, Label l);    // if (unsigned)a >= b goto l
    void branchGeS(Reg a, Reg b, Label l);    // if (signed)a >= b goto l
    void branchNe(Reg a, Reg b, Label l);     // if a != b goto l
    // Call a host built-in: d = fn(a, b, c). Preserves the host-arg registers (R0/R1/R2 = buf,
    // nLights, cpl) across the call by saving them on the stack, so they stay live for the
    // statement after the call — the live-vreg-across-Call contract. `fn` is an absolute
    // function pointer (materialised into a scratch register). The implementation saves the WHOLE
    // vreg pool, not just R0..R2, so any value may be live across a call — a loop counter and its
    // limit are, whenever the body calls anything, which is most real effects.
    void call(Reg d, Reg a, Reg b, Reg c, const void* fn);
    /// Call a function in THIS block, by label: the script-to-script call. The return address is
    /// linked into x30 (arm64 `bl`) or pushed on the stack (x86-64 `call rel32`); either way the
    /// callee's prologue preserves it, which is what lets the call nest.
    /// Every caller vreg is preserved across it exactly as call() does for a builtin, and the
    /// return value is delivered into `d` when `take` is set.
    void callLabel(Label l, Reg d = R0, bool take = false);
    void ret();

private:
    // The flags pair the fused branches above are built from. arm64-only, so private: a lowering
    // that reached for these could not be shared with a backend that has no flags register.
    void cmp(Reg a, Reg b);              // flags = a - b
    void branchIf(Cond c, Label l);      // if flags satisfy c goto l (after cmp)
    // The emitted-code buffer's size, fixed for this object's life but chosen per script.
    const size_t kCap;
    // Sized in core (kAsmLabels/kAsmFixups) so the three backends cannot drift apart.
    static constexpr uint8_t kMaxLabels = kAsmLabels;
    static constexpr uint8_t kMaxFixups = kAsmFixups;

    void emit32(uint32_t w);             // append one 32-bit instruction (arm64 only; x64 encoders
                                         // are variable-length and call emitBytes directly)
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(MM_MOONLIVE_FORCE_NO_HOST_JIT)
    // The four indexed `[base + index]` memory ops share one encoder; see the definition for the
    // two SDM rules it centralizes (the rbp/r13 base case, and the byte store's mandatory REX).
    // Declared only where it is defined: on arm64 these ops are single fixed-width instructions
    // and this member would be dead weight in the class.
    void emitIndexed(const uint8_t* opcode, size_t opLen, bool prefix66, bool forceRex,
                     uint8_t reg, uint8_t base, uint8_t index);
#endif
    // A pending reference to a label. The kind is needed because a call's displacement is a
    // different field from a branch's: `bl` carries imm26 at bits 0..25, the conditional branches
    // imm19 at bits 5..23. Patching one as the other retargets it in silence, which is the failure
    // this discriminator exists to make impossible. Named rather than numbered, matching the other
    // two backends: Branch covers cbz and b.cond, which share the imm19 field.
    enum class FixKind : uint8_t { Branch, Call };
    struct Fixup { size_t at; Label label; FixKind kind = FixKind::Branch; };
    void addFixup(size_t at, Label label, FixKind kind = FixKind::Branch);   // bounds-checked

    // HEAP, not a member array: the assembler is a stack local in lowerToBytes, so a kCap-sized
    // member put 2 KB on the compile chain's stack — on top of the staging buffer and the parser
    // frames. On a classic ESP32 that overflowed the task and faulted inside _xt_context_save
    // (the plan named this: "buf_[kCap] inside the assembler, itself a stack local"). The buffer is
    // scratch that ends in a memcpy to the caller's output, so nothing outlives the object.
    uint8_t* buf_;
    size_t   len_ = 0;
    bool     overflow_ = false;
    // Frame size in bytes, 0 when no prologue was emitted. arm64's epilogue reads it, so its
    // teardown can never disagree with the setup about how far sp moved: the class of bug that
    // returns to a corrupted stack and is indistinguishable from a miscompile. x86-64 unwinds via
    // `lea rsp, [rbp - kNonvolSaveBytes]` instead, which needs no size; it keeps the field current
    // because spillStore/spillLoad read it to refuse a slot access with no frame behind it.
    uint16_t frameBytes_ = 0;

    // Label positions (-1 = unbound) and pending branch fixups.
    int32_t  labelPos_[kMaxLabels];
    uint8_t  labelCount_ = 0;
    Fixup    fixups_[kMaxFixups];
    uint8_t  fixupCount_ = 0;

    void patchBranches();                // resolve all fixups against bound labels
};

}  // namespace mm::moonlive
