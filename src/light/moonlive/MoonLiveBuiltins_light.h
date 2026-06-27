#pragma once

#include "core/moonlive/MoonLiveBuiltins.h"

#include <cstdint>

// MoonLive — the LIGHT-DOMAIN built-in registration. This is the only place the LED vocabulary
// lives: the function NAMES (`setRGB`, `fill`, `random16`), their arg counts, and the meaning
// of the inline opcodes (WriteRGB = an RGB pixel write, FillRGB = fill every light). The core
// compiler sees only the neutral BuiltinTable / InlineOp tags this file hands it. A different
// host (display, sensor) would write its own registration with its own names; the core is
// unchanged. (The ESPLiveScript / ARTI bound-function model, doc §3.4.)

namespace mm::moonlive {

// random16(n) → a pseudo-random value in [0, n). A simple LCG, deterministic enough that the
// runtime Bounds guard always sees an in-range index; the same implementation on every target
// so a script behaves identically. The one host helper exposed as a Call so far.
extern "C" inline uint32_t mm_light_random16(uint32_t n) {
    static uint32_t s = 0x2545F491u;
    s = s * 1664525u + 1013904223u;
    return n ? (s >> 16) % n : 0u;
}

// The light-domain built-in table the binding injects into the compiler. setRGB and fill are
// Inline (they lower to stores — the hot-path writers, no per-call cost); random16 is a Call.
inline BuiltinTable lightBuiltins() {
    BuiltinTable t;
    // setRGB(index, r, g, b)  → write one pixel (bounds-guarded). Inline op WriteRGB.
    t.add({"setRGB", 4, /*returns*/ false, BuiltinKind::Inline, nullptr, InlineOp::WriteRGB});
    // fill(r, g, b)           → write every light. Inline op FillRGB.
    t.add({"fill", 3, false, BuiltinKind::Inline, nullptr, InlineOp::FillRGB});
    // random16(n)             → a value in [0,n). A Call to the host helper.
    t.add({"random16", 1, /*returns*/ true, BuiltinKind::Call,
           reinterpret_cast<const void*>(&mm_light_random16), {}});
    return t;
}

}  // namespace mm::moonlive
