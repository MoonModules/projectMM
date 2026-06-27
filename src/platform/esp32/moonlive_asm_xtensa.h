#pragma once

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

enum Reg : uint8_t { R0 = 0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, kRegCount };
using Label = uint8_t;
enum class Cond : uint8_t { Lo /* unsigned < */, Hs /* unsigned >= */ };

class XtensaAssembler {
public:
    void finalize() { patchBranches(); }
    const uint8_t* bytes() const { return buf_; }
    size_t size() const { return len_; }
    bool overflowed() const { return overflow_; }

    void prologue();                     // entry a1, 32  (must be the first instruction)
    Label newLabel();
    void  bind(Label l);

    void movImm(Reg d, int32_t imm);     // movi aD, #imm (0..255)
    void movReg(Reg d, Reg a);           // mov.n aD, aA
    void addImm(Reg d, Reg a, int32_t imm);   // addi.n aD, aA, #imm (1..15)
    void addReg(Reg d, Reg a, Reg b);    // add.n aD, aA, aB
    void mulReg(Reg d, Reg a, Reg b);    // mull aD, aA, aB
    void store8(Reg base, Reg off, Reg val);  // s8i via computed address (add then s8i,0)
    void branchIfZero(Reg a, Label l);   // beqz aA, l  (nLights==0 guard)
    void branchGeU(Reg a, Reg b, Label l);    // bgeu aA, aB, l  (Bounds: skip if a>=b)
    void branchNe(Reg a, Reg b, Label l);     // bne aA, aB, l   (loop test)
    void call(Reg d, Reg a, const void* fn);  // windowed call8 to a host built-in
    void epilogue();                     // retw.n

private:
    static constexpr size_t kCap = 768;
    static constexpr uint8_t kMaxLabels = 16;
    static constexpr uint8_t kMaxFixups = 32;

    void emit(const uint8_t* p, size_t n);
    void emit2(uint16_t w);              // narrow (16-bit) instruction
    void emit3(uint32_t w);              // wide (24-bit) instruction

    uint8_t  buf_[kCap] = {};
    size_t   len_ = 0;
    bool     overflow_ = false;

    int32_t  labelPos_[kMaxLabels];
    uint8_t  labelCount_ = 0;
    struct Fixup { size_t at; Label label; };   // all our branches use the 8-bit offset at byte+2
    Fixup    fixups_[kMaxFixups];
    uint8_t  fixupCount_ = 0;

    void patchBranches();
};

}  // namespace mm::moonlive
