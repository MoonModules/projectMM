#pragma once

#include <cstdio>

#include "core/moonlive/MoonLiveBuiltins.h"

#include <cstdint>

// MoonLive — the LIGHT-DOMAIN built-in registration. This is the only place the LED vocabulary
// lives: the function NAMES (`setRGB`, `fill`, `random16`), their arg counts, and the meaning
// of the inline opcodes (StoreElem = an RGB pixel write, FillElems = fill every light). The core
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

// print(v) → write one value to the serial log, and return it so `print` can be dropped into an
// expression without changing what it computes (`setXYZ(0, print(x), y, z)` still stores x).
//
// This is the only way to see INSIDE a running script. A script that compiles cleanly and produces
// a black fixture gives no other clue: every part reports success and the result is simply wrong.
// That case cost a long debugging session before this existed.
//
// **Rate-limited, because the call sites are per-light.** A modifier's script runs once per light
// per mapping rebuild — 16,384 times on a 128x128 wall. Printing all of them would flood the serial
// line, stall the render (a UART write blocks) and bury the first values, which are the useful
// ones. So a burst is capped and the rest are counted, not printed: the tail of a flood tells you
// nothing the head did not.
extern "C" inline uint32_t mm_light_print(uint32_t v) {
    static uint32_t shown = 0;
    constexpr uint32_t kBurst = 32;
    if (shown < kBurst) {
        std::printf("[script] %u\n", static_cast<unsigned>(v));
        if (++shown == kBurst) std::printf("[script] (further prints suppressed)\n");
    }
    return v;
}

// The light-domain built-in table the binding injects into the compiler. setRGB and fill are
// Inline (they lower to stores — the hot-path writers, no per-call cost); random16 is a Call.
inline BuiltinTable lightBuiltins() {
    BuiltinTable t;
    // setRGB(index, r, g, b)  → write one pixel (bounds-guarded). Inline op StoreElem.
    t.add({"setRGB", 4, /*returns*/ false, BuiltinKind::Inline, nullptr, InlineOp::StoreElem});
    // setXYZ(index, x, y, z)  → write one POSITION (bounds-guarded). The same StoreElem as
    // setRGB: three values at index * stride. What differs is the destination the binding hands
    // run() — a colour buffer for an effect, a coordinate for a modifier — so one op serves both
    // and the engine stays free of any notion of what the three bytes mean.
    t.add({"setXYZ", 4, /*returns*/ false, BuiltinKind::Inline, nullptr, InlineOp::StoreElem});
    // fill(r, g, b)           → write every light. Inline op FillElems.
    t.add({"fill", 3, false, BuiltinKind::Inline, nullptr, InlineOp::FillElems});
    // random16(n)             → a value in [0,n). A Call to the host helper (typed fn pointer).
    t.add({"random16", 1, /*returns*/ true, BuiltinKind::Call, &mm_light_random16, {}});
    // print(v)                → log v and return it. The script-level debugger.
    t.add({"print", 1, /*returns*/ true, BuiltinKind::Call, &mm_light_print, {}});
    return t;
}

}  // namespace mm::moonlive
