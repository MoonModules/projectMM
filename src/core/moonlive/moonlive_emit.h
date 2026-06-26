#pragma once

#include <cstdint>
#include <cstddef>

// MoonLive — per-ISA code emitter (the backend seam, §3.2 of livescripts-analysis-top-down.md).
//
// This header is the NEUTRAL declaration the engine calls; the implementation is per-ISA and
// lives behind the platform boundary (src/platform/<target>/moonlive_emit.cpp): Xtensa on
// ESP32, the host ISA (arm64 / x86-64) on desktop. The engine (src/core/moonlive/) never
// branches on ISA — it asks for bytes and runs them. That is the IR/backend seam at its
// first, smallest increment: no IR yet (one fixed routine, no language), but the boundary is
// already where it belongs.
//
// The emitted routine's C signature is FillFn: write a fixed (r,g,b) to every light —
// buf[i*cpl+0..2] = r,g,b for i in [0,nLights) — then return. The engine copies the bytes
// into an executable block (platform::allocExec + writeExec) and calls them through FillFn.

namespace mm::moonlive {

using FillFn = void (*)(uint8_t* buf, uint32_t nLights, uint8_t cpl);

// Stage 1b: the routine also takes a per-frame `t` (the host's elapsed() ms), so the host
// feeds a changing value into the SAME native code each tick and the output animates. The
// engine passes elapsed() through run().
using AnimFn = void (*)(uint8_t* buf, uint32_t nLights, uint8_t cpl, uint32_t t);

// Emit the fixed-colour fill routine's machine code into `out` (capacity `cap` bytes), for
// the ISA this translation unit was compiled for, with the colour baked in. Returns the
// number of bytes written, or 0 if `cap` is too small (the caller degrades). The emitted
// bytes ARE the function — the engine makes `out` executable and casts it to FillFn.
//
// This is the hand-written reference codegen for Stage 1a; Stage 2 replaces it with
// parser-driven emission that must reproduce the same bytes (the golden-bytes test).
size_t emitFill(uint8_t* out, size_t cap, uint8_t r, uint8_t g, uint8_t b);

// Stage 1b: emit a routine that derives its colour from the runtime arg `t` —
//   red = (t >> 3) & 0xFF, green = 0, blue = 64  for every light.
// Proves a per-frame host value flows into the emitted native code and changes the output
// (the grid's red ramps over time). Same emit/exec/call path as emitFill, one extra arg.
size_t emitAnimatedFill(uint8_t* out, size_t cap);

}  // namespace mm::moonlive
