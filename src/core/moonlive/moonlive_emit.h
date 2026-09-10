#pragma once

#include <cstdint>
#include <cstddef>

// MM_MOONLIVE_HAS_HOST_JIT is defined in platform_config.h (per-platform), so the arch check stays
// behind the platform boundary and this header stays neutral.
//
// It exists for the TEST HARNESS and nothing else — no shipping code branches on it. A build with
// no backend degrades at RUN time instead: lowerToBytes returns 0 (moonlive_asm_noarch.cpp),
// compile() reports the failure, and a scripted module renders dark. But a test that calls
// render() cannot even be COMPILED there, so ~40 TEST_CASEs are gated on `#if
// MM_MOONLIVE_HAS_HOST_JIT`. That is why it is a macro rather than a constexpr, and why the
// noarch path does not make it redundant: one answers the program, the other answers the build.
#include "platform/platform.h"

// MoonLive — per-ISA code emitter (the backend seam, §3.2 of livescripts-analysis-top-down.md).
//
// This header is the NEUTRAL declaration the engine calls; the implementation is per-ISA and
// lives behind the platform boundary (src/platform/<target>/moonlive_emit.cpp): Xtensa on the
// classic/S3, RISC-V on the P4, the host ISA (arm64 / x86-64) on desktop. The engine
// (src/core/moonlive/) never branches on ISA — it asks for bytes and runs them. Adding an ISA
// is a new branch in the emitter; the engine and front-end are unchanged.
//
// The emitted routine's C signature is FillFn: write a fixed (r,g,b) to every light —
// buf[i*cpl+0..2] = r,g,b for i in [0,nLights) — then return. The engine copies the bytes
// into an executable block (platform::allocExec + writeExec) and calls them through FillFn.

namespace mm::moonlive {

using FillFn = void (*)(uint8_t* buf, uint32_t nLights, uint8_t cpl);

// The animated routine also takes a per-frame `t` (the host's elapsed() ms), so the host
// feeds a changing value into the same native code each tick and the output animates. The
// engine passes elapsed() through run().
using AnimFn = void (*)(uint8_t* buf, uint32_t nLights, uint8_t cpl, uint32_t t);

// The front-end-compiled routine takes a 5th arg: a pointer to the control-values arena (one
// byte per declared control, kArg4). A script reads a control with LoadCtrl; the host updates an
// arena byte when a slider moves and the next call reads it — live, no recompile. Code that
// declares no control simply never reads the pointer (it may be nullptr then). This is the
// signature compileSource()'d code is called through.
using CtrlFn = void (*)(uint8_t* buf, uint32_t nLights, uint8_t cpl, uint32_t t, const uint8_t* ctrls);

/// The SAME emitted function, called for its answer rather than its effect: identical parameters,
/// identical frame, only the host's view of the return register differs. A script function that
/// ends in `return <expr>` parks its value there, which is what lets `dimensions()` and `tags()`
/// report to the host without a second calling convention.
///
/// Calling a function that returns nothing through this alias reads whatever the register held, so
/// the binding calls it only for functions the script actually declared: `hasEntry(name)` first.
using ValueFn = uintptr_t (*)(uint8_t* buf, uint32_t nLights, uint8_t cpl, uint32_t t, const uint8_t* ctrls);

// Emit the fixed-color fill routine's machine code into `out` (capacity `cap` bytes), for
// the ISA this translation unit was compiled for, with the color baked in. Returns the
// number of bytes written, or 0 if `cap` is too small (the caller degrades). The emitted
// bytes ARE the function — the engine makes `out` executable and casts it to FillFn. The
// parser-driven codegen (MoonLiveCompiler) reproduces these exact bytes (the golden-bytes test).
size_t emitFill(uint8_t* out, size_t cap, uint8_t r, uint8_t g, uint8_t b);

// Emit a routine that derives its color from the runtime arg `t` —
//   red = (t >> 3) & 0xFF, green = 0, blue = 64  for every light.
// Proves a per-frame host value flows into the emitted native code and changes the output
// (the grid's red ramps over time). Same emit/exec/call path as emitFill, one extra arg.
size_t emitAnimatedFill(uint8_t* out, size_t cap);

struct IrProgram;   // src/core/moonlive/MoonLiveIr.h

// What one target's register file offers the allocator — the ONLY thing core's spill pass needs to
// know about an ISA, and the reason the allocator is written once instead of three times. Each
// backend fills this in from its own map and hands it to spillToBudget (MoonLiveSpill.h); nothing
// ISA-specific crosses in the other direction.
// WHY the last lowering returned 0. A lowering has four distinct ways to refuse and they used to
// share one return value, so a failure on the device read "codegen failed (unsupported on this
// target, or too large)" whether the register allocator gave up, the assembler overflowed, or the
// code outgrew its buffer. Compiling the same script on the host through the same emitter succeeded,
// which left only the device able to say which, and it could not. Static rather than threaded
// through LowerFn: the seam has three backends and a test double, and the compile is single-threaded
// per call, so one byte read straight after the call is the whole contract.
enum class LowerRefusal : uint8_t { None, Spill, NullCall, AsmOverflow, OverCap };
inline LowerRefusal& lowerRefusal() { thread_local LowerRefusal r = LowerRefusal::None; return r; }
// The register allocator's own refusal detail: which of its guards fired, and the budget it saw.
// Written by spillToBudget, read by compileSource into the error string, so a device can say
// "avail 7, temps 3, guard 5" instead of one message for six different causes.
struct SpillDetail { uint8_t guard = 0, avail = 0, temps = 0, vregs = 0, slots = 0, spilled = 0; };
inline SpillDetail& spillDetail() { thread_local SpillDetail d; return d; }

struct RegBudget {
    uint8_t regs = 0;        // machine registers the vreg map exposes (kRegCount)
    uint8_t reserved = 0;    // registers the backend keeps for the inline ops this program contains
    uint8_t slots = 0;       // spill slots the backend's frame can address (0 = cannot spill at all)

    /// Registers left for the allocator once the backend's inline scratch is taken out. Saturating,
    /// because a program whose scratch demand exceeds the whole file must report "no registers"
    /// rather than wrap to a huge count and allocate against a register that does not exist.
    uint8_t allocatable() const { return regs > reserved ? uint8_t(regs - reserved) : uint8_t(0); }
};

// Lower a typed IR program to machine code for this TU's ISA, via the per-ISA assembler.
// This is the general codegen path the front-end uses; emitFill/emitAnimatedFill are the
// hand-encoded references the assembler-built output is behaviorally checked against. Returns
// the byte count, or 0 on overflow / cap too small (the caller degrades).
//
// `ir` is taken by NON-CONST reference because the backend runs core's register allocator over it
// first (MoonLiveSpill.h), which rewrites a program that names more live values than this target has
// registers. Taking a copy instead would double the compile's peak memory on the smallest device for
// no benefit — the IR is compile-time scratch the caller drops immediately afterwards.
///
/// `squeeze`, when non-null, REPLACES the register budget this backend would compute for itself.
/// It exists because the spiller is the hardest logic in the compiler and only one backend is ever
/// executed by tests: with a budget deliberately smaller than the host's, the allocator runs on the
/// arm64 path a test can actually call and RUN, and the same script at the full and the squeezed
/// budget must render identical pixels. Without this seam the pass would be verifiable only on
/// hardware. Production callers pass nullptr and get the target's real budget.
size_t lowerToBytes(IrProgram& ir, uint8_t* out, size_t cap, const RegBudget* squeeze = nullptr);

}  // namespace mm::moonlive
