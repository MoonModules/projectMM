#pragma once

#include <cstdint>
#include <cstddef>
#include "core/moonlive/MoonLiveBuiltins.h"   // InlineOp (a neutral opcode tag)

// MoonLive IR — the typed intermediate representation between the front-end and the per-ISA
// assembler (§3.2 of livescripts-analysis-top-down.md). The front-end lowers an AST to a flat
// list of three-address ops over virtual registers; each per-ISA backend lowers that same IR
// to machine bytes. The IR is the seam: it knows the *operations*, never the *ISA* and never a
// domain (no LEDs). Buffer writes are not a special IR op — they are `Inline` ops carrying a
// neutral opcode tag the HOST registered (see MoonLiveBuiltins.h); the core just threads the
// tag to the backend.
//
// It is a compile-time data structure — consumed during lowering, never present at run time.
// A fixed-capacity op list (no heap in the build path); a single statement is a handful of ops.
//
// Virtual registers are plain indices v0..v(kMaxVRegs-1). A backend maps them to machine
// registers. The named host arguments arrive in fixed vregs (kArg…) so the front-end refers to
// them without knowing the ABI. They are named neutrally — kArg0..kArg3 — and a host assigns
// meaning (for the light host: buf, nLights, cpl, t).

namespace mm::moonlive {

using VReg = uint8_t;

enum : VReg { kArg0 = 0, kArg1 = 1, kArg2 = 2, kArg3 = 3, kFirstTemp = 4 };

static constexpr uint8_t kMaxVRegs = 16;     // a statement uses a handful; no allocator yet
static constexpr uint8_t kMaxIrOps = 64;     // a statement is a handful of ops; fixed, no heap

// The op set — neutral. Three-address form: dst plus up to three source operands.
enum class IrOp : uint8_t {
    Const,     // dst = imm
    Add,       // dst = a + b
    AddImm,    // dst = a + imm
    Mul,       // dst = a * b
    Loop,      // counted loop: counter=a, limit=b; body runs until LoopEnd
    LoopEnd,   // end of the innermost Loop body
    Bounds,    // guard: if a >= b, skip ops until BoundsEnd (the §4 out-of-range check)
    BoundsEnd, // end of the innermost Bounds-guarded block
    Call,      // dst = (*callFn)(a) — call a host-registered function (callFn = the C fn ptr)
    Inline,    // a host-registered inline op (inlineOp tag); operands a/b/c/d (op-specific)
};

struct IrInst {
    IrOp     op;
    VReg     dst = 0;
    VReg     a = 0, b = 0, c = 0, d = 0;   // source vregs (op-dependent)
    int32_t  imm = 0;                      // immediate (Const) / addr offset
    const void* callFn = nullptr;          // Call: the host C function pointer
    InlineOp inlineOp{};                   // Inline: the neutral opcode tag
};

// A lowered program: a fixed list of ops plus the vreg high-water mark.
struct IrProgram {
    IrInst  ops[kMaxIrOps];
    uint8_t count = 0;
    VReg    vregsUsed = kFirstTemp;

    bool push(const IrInst& i) {
        if (count >= kMaxIrOps) return false;
        ops[count++] = i;
        if (i.dst + 1 > vregsUsed) vregsUsed = static_cast<VReg>(i.dst + 1);
        return true;
    }
};

}  // namespace mm::moonlive
