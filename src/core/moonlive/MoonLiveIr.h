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
// them without knowing the ABI. They are named neutrally — kArg0..kArg4 — and a host assigns
// meaning (for the light host: buf, nLights, cpl, t, and a controls-values pointer).

namespace mm::moonlive {

using VReg = uint8_t;

// kArg4 is a per-instance data pointer the host passes at run time (for the light host: the
// control-values arena). LoadCtrl reads a byte from it — a script control whose value the binding
// updates live, without a recompile (the kArg3/t pattern, one slot over).
enum : VReg { kArg0 = 0, kArg1 = 1, kArg2 = 2, kArg3 = 3, kArg4 = 4, kFirstTemp = 5 };

static constexpr uint8_t kMaxVRegs = 16;     // a statement uses a handful; no allocator yet
static constexpr uint8_t kMaxIrOps = 64;     // a statement is a handful of ops; fixed, no heap

// The op set — neutral. Three-address form: dst plus up to three source operands. (Counted
// loops and bounds guards are not IR ops: the StoreElem/FillElems inline ops carry their own
// loop + bounds-guard in the per-ISA lowering. They'll arrive here when a script statement needs
// a general loop — added then, not speculatively now.)
enum class IrOp : uint8_t {
    Const,     // dst = imm
    Add,       // dst = a + b
    AddImm,    // dst = a + imm
    Mul,       // dst = a * b
    Call,      // dst = (*callFn)(a) — call a host-registered function (callFn = the C fn ptr)
    Inline,    // a host-registered inline op (inlineOp tag); operands a/b/c/d (op-specific)
    LoadCtrl,  // dst = ((const uint8_t*)kArg4)[imm] — read a control value byte at offset imm
};

struct IrInst {
    IrOp     op;
    VReg     dst = 0;
    VReg     a = 0, b = 0, c = 0, d = 0;   // source vregs (op-dependent)
    int32_t  imm = 0;                      // immediate (Const) / addr offset
    HostCallFn callFn = nullptr;           // Call: the host C function pointer (typed alias)
    InlineOp inlineOp{};                   // Inline: the neutral opcode tag
};

// A control a script declared (`uint8_t speed = 50; // @control 0..99`). Neutral: the core
// knows {name, a neutral type, range, default, and the byte offset into the run-time controls
// arena it lives at}. The light-domain binding turns this into a real MoonModule control bound to
// the arena slot. `type` is a neutral kind — Uint8 only in Stage 1 — NOT a projectMM ControlType.
enum class CtrlType : uint8_t { Uint8 };

struct DeclaredControl {
    const char* name = nullptr;        // script-declared name (points into the source buffer)
    uint8_t     nameLen = 0;           // length (the source is not NUL-terminated per token)
    CtrlType    type = CtrlType::Uint8;
    int32_t     min = 0, max = 255, def = 0;
    uint8_t     offset = 0;            // byte offset into the controls arena (declaration order)
};

static constexpr uint8_t kMaxCtrls = 8;   // a script declares a handful of controls; fixed, no heap

// A lowered program: a fixed list of ops plus the vreg high-water mark.
struct IrProgram {
    IrInst  ops[kMaxIrOps];
    uint8_t count = 0;
    VReg    vregsUsed = kFirstTemp;

    bool push(const IrInst& i) {
        if (count >= kMaxIrOps) return false;
        // Reject any op that names a vreg outside the fixed register budget — an invalid program
        // is dropped at the seam rather than reaching a backend that would index past its map.
        if (i.dst >= kMaxVRegs || i.a >= kMaxVRegs || i.b >= kMaxVRegs ||
            i.c >= kMaxVRegs || i.d >= kMaxVRegs) return false;
        ops[count++] = i;
        if (i.dst + 1 > vregsUsed) vregsUsed = static_cast<VReg>(i.dst + 1);
        return true;
    }
};

}  // namespace mm::moonlive
