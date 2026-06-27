#pragma once

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
// IR stays ISA-neutral. R0..R3 alias the host-ABI argument registers (buf, nLights, cpl, t);
// R4+ are caller-saved scratch.
enum Reg : uint8_t { R0 = 0, R1, R2, R3, R4, R5, R6, R7, R8, R9,
                     R10, R11, R12, R13, kRegCount };

// A label is an index into the assembler's label table; bind() fixes its position, branches to
// it are back-patched when bound.
using Label = uint8_t;

// Branch condition (only the ones the IR needs so far).
enum class Cond : uint8_t { Lo /* unsigned < */, Hs /* unsigned >= */ };

class HostAssembler {
public:
    // --- buffer ---
    // Resolve all branch fixups against bound labels, then expose the finished bytes. Call
    // once after the last instruction; bytes()/size() are valid only after finalize().
    void finalize() { patchBranches(); }
    const uint8_t* bytes() const { return buf_; }
    size_t size() const { return len_; }
    bool overflowed() const { return overflow_; }

    // --- labels ---
    Label newLabel();
    void  bind(Label l);                 // mark l's position = current offset

    // --- instructions (named, register/immediate operands) ---
    void movImm(Reg d, int32_t imm);     // d = imm
    void addImm(Reg d, Reg a, int32_t imm);   // d = a + imm
    void addReg(Reg d, Reg a, Reg b);    // d = a + b
    void mulImm(Reg d, Reg a, int32_t imm);   // d = a * imm  (index scaling by a constant)
    void mulReg(Reg d, Reg a, Reg b);    // d = a * b   (index scaling by a runtime cpl)
    void store8(Reg base, Reg off, Reg val);  // byte store: base[off] = val (low 8 bits)
    void cmp(Reg a, Reg b);              // flags = a - b
    void branchIfZero(Reg a, Label l);   // if a == 0 goto l
    void branchIf(Cond c, Label l);      // if flags satisfy c goto l (after cmp)
    // Call a host built-in: d = fn(a). Preserves the host-arg registers (R0/R1/R2 = buf,
    // nLights, cpl) across the call by saving them on the stack, so they stay live for the
    // statement after the call — the live-vreg-across-Call contract. `fn` is an absolute
    // function pointer (materialised into a scratch register). Caller-saved vregs other than
    // R0..R2 must not be live across a call (the front-end orders ops so none are).
    void call(Reg d, Reg a, const void* fn);
    void ret();

private:
    static constexpr size_t kCap = 768;
    static constexpr uint8_t kMaxLabels = 16;
    static constexpr uint8_t kMaxFixups = 32;

    void emit32(uint32_t w);             // append one 32-bit instruction (arm64) — or byte run (x64)
    void emitBytes(const uint8_t* p, size_t n);

    uint8_t  buf_[kCap] = {};
    size_t   len_ = 0;
    bool     overflow_ = false;

    // Label positions (-1 = unbound) and pending branch fixups.
    int32_t  labelPos_[kMaxLabels];
    uint8_t  labelCount_ = 0;
    struct Fixup { size_t at; Label label; uint8_t kind; };  // kind: 0=cbz,1=b.cond
    Fixup    fixups_[kMaxFixups];
    uint8_t  fixupCount_ = 0;

    void patchBranches();                // resolve all fixups against bound labels
};

}  // namespace mm::moonlive
