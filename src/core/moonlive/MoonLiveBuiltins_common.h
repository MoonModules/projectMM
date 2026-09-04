#pragma once

#include "core/moonlive/MoonLiveBuiltins.h"
#include "core/math8.h"    // beatsin16: the shared time vocabulary
#include "core/math16.h"   // beat16 / sin16 / cos16: full-range waveforms
#include "core/noise.h"    // inoise8: the shared gradient-noise field

#include <atomic>
#include <cstdint>
#include <cstdio>

// The DOMAIN-NEUTRAL half of the MoonLive vocabulary: arithmetic, waveforms, noise, randomness and
// print. None of it is about light, so none of it belongs to the light domain.
//
// It lived in MoonLiveBuiltins_light.h because that was the only vocabulary there was. When the
// service table arrived it had to reach INTO the light header for `print` and `addControl`, which is
// core depending on a domain: the wrong direction, and the reason a service script could not call
// `sin` while an effect could, for no reason either could explain.
//
// What stays in the light table is what genuinely needs a canvas: setRGB, fill, fade, addLight,
// line, the palette and particle helpers, the audio frame, and the per-light coordinates.

namespace mm::moonlive {

// This used to fold through a 16-BIT window (`v > 32767 ? v - 65536 : v`), because a script had no
// way to hold a negative and the convention was that the top half of the 16-bit range meant one.
// That window was the inverse of uint16_t member truncation, it was written down in neither place,
// and it is what made `d = 60000` read as -5536: the script author thought in the member's range
// and the builtin thought in the window's. int16_t members hold a negative directly now, so the
// window has nothing left to undo and the value passes through.
inline int32_t signedArg(uintptr_t a) {
    return static_cast<int32_t>(uint32_t(a));
}

/// The remaining print budget. A binding resets it when it compiles, so every edit of a script gets
/// a fresh window: without that, one burst silences the debugging tool for the life of the process,
/// which is exactly when a second look at a misbehaving script is most needed.
/// ATOMIC for the same reason random16's seed is: two threads run scripts concurrently. The decrement
/// below is the one that matters: read-modify-write on a plain uint32_t lets two threads both see 1,
/// both decrement, and the budget WRAP to ~4 billion, turning the bound that keeps `print` off the
/// render tick's critical path into no bound at all.
inline std::atomic<uint32_t>& printBudget() { static std::atomic<uint32_t> n{0}; return n; }

/// Grant a fresh burst. Call from the binding's prepare(), alongside the compile.
///
/// print() writes to serial, which blocks, and an effect script runs on the render tick: so the
/// burst is what bounds the cost: a handful of writes per compile, after which the call is a compare
/// and a return. Draining through a queue would take the last of it off the tick; backlogged.
inline void resetPrintBudget() { printBudget().store(32, std::memory_order_relaxed); }

extern "C" inline uint32_t mm_ml_random16(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t n = uint32_t(args[0]);
    // ATOMIC, because two threads run scripts at once: the render task walks a layout for the frame
    // while the HTTP task asks the same layout for its light count after a control edit (the reason
    // the addLight sink is a per-thread table below). A plain `static` here is a data race, and a
    // lost update would additionally let two draws return the SAME value, which for a "random"
    // helper is a correctness bug rather than a tolerable one. compare_exchange keeps the sequence
    // exactly the LCG's, just serialized.
    static std::atomic<uint32_t> seed{0x2545F491u};
    uint32_t prev = seed.load(std::memory_order_relaxed), next;
    do {
        next = prev * 1664525u + 1013904223u;
    } while (!seed.compare_exchange_weak(prev, next, std::memory_order_relaxed));
    return n ? (next >> 16) % n : 0u;
}

// mod(a, b) → a % b, the wrap a cyclic animation needs. `t` grows without bound, so every effect
// that repeats has to fold it back into a range: `mod(t * speed, width)` is a sweep that returns to
// the start instead of running off the end once and never coming back.
//
// A Call rather than an operator because no ISA here has a cheap integer divide: Xtensa has none at
// all, and emitting a division routine inline would cost more code than the whole script. One host
// function, called like any other builtin, keeps the emitted code small and the three backends
// identical. b == 0 returns 0 rather than trapping: a script must degrade, never fault.
// SIGNED, like `%` in every language a script author already knows. A coordinate is signed now, so
// an unsigned remainder here would be a bespoke rule with nothing on the page to signpost it: the
// exact shape of the bugs this whole change set exists to remove.
//
// `t` is unsigned time and passes 2^31 after about 25 days, at which point `mod(t, n)` reads it as
// negative. That is a real edge, and it is not the reason to keep this unsigned: `t` breaks at 2^32
// regardless, so signedness moves WHEN rather than WHETHER. A wrapping clock needs its own answer,
// not a modulo that hides it. No shipped script uses mod(t, ...).
extern "C" inline uint32_t mm_ml_mod(const uintptr_t* args, uint32_t, const uint8_t*) {
    const int32_t a = static_cast<int32_t>(uint32_t(args[0]));
    const int32_t b = static_cast<int32_t>(uint32_t(args[1]));
    // INT32_MIN % -1 is UB and traps on x86-64 (the other three ISAs quietly wrap, which is why a
    // bench never shows it). Same stance as b == 0: a script degrades, never faults.
    if (b == 0 || (a == INT32_MIN && b == -1)) return 0;
    return static_cast<uint32_t>(a % b);
}

// div(a, b) → a / b, and what the '/' OPERATOR lowers to. Registered under a name for the same
// reason mod is: the parser resolves both operators through the builtin table, so core stays
// domain-neutral and a divide is one host call rather than an instruction no ISA here has.
// b == 0 SATURATES with the numerator's sign: IEEE 754's ±infinity mapped onto an int, and what
// libfixmath does on divide overflow. The value is also the visually right one: `k / dist` at
// dist == 0 is the CENTER of a ripple, where max reads as the peak the eye expects and 0 punched
// a dark hole exactly there. 0/0 stays 0 (no direction to saturate toward). mod keeps returning
// 0: there is no "infinite remainder". Either way a script degrades, never faults, and needs no
// zero-check of its own.
// SIGNED, for the reason given at mod above: `/` means what it means everywhere else. Scaling a
// coordinate is the common case and coordinates go negative, so an unsigned divide turned
// `uvX(...) * zoom / 40` on the left half of a grid into 107361151 rather than -13030.
extern "C" inline uint32_t mm_ml_div(const uintptr_t* args, uint32_t, const uint8_t*) {
    const int32_t a = static_cast<int32_t>(uint32_t(args[0]));
    const int32_t b = static_cast<int32_t>(uint32_t(args[1]));
    if (b == 0)
        return static_cast<uint32_t>(a > 0 ? INT32_MAX : a < 0 ? INT32_MIN : 0);
    // INT32_MIN / -1 overflows: UB, and a SIGFPE on x86-64. Returns the saturated value a script
    // would expect from negating INT32_MIN, rather than 0, which would read as "division broke".
    if (a == INT32_MIN && b == -1) return static_cast<uint32_t>(INT32_MAX);
    return static_cast<uint32_t>(a / b);
}

// fdiv(a, b) → the Q16.16 quotient, what the '/' OPERATOR lowers to when both sides are fixed.
// A separate host call from div because the numerator must widen: the quotient of two Q16.16
// values needs (a << 16) / b, and shifting a 32-bit fixed value left by 16 in registers wraps for
// anything past |128.0|: which is exactly what froze two shipped shaders. int64 in the host is
// exact over the whole range, and a divide is a host call on every ISA here anyway (libfixmath's
// fix16_div does the same widening for the same reason).
// b == 0 saturates with the numerator's sign, matching div; a quotient outside int32 saturates
// too, rather than wrapping into a number nobody wrote.
extern "C" inline uint32_t mm_ml_fdiv(const uintptr_t* args, uint32_t, const uint8_t*) {
    const int32_t a = static_cast<int32_t>(uint32_t(args[0]));
    const int32_t b = static_cast<int32_t>(uint32_t(args[1]));
    if (b == 0)
        return static_cast<uint32_t>(a > 0 ? INT32_MAX : a < 0 ? INT32_MIN : 0);
    const int64_t q = (static_cast<int64_t>(a) << 16) / b;
    if (q > INT32_MAX) return static_cast<uint32_t>(INT32_MAX);
    if (q < INT32_MIN) return static_cast<uint32_t>(INT32_MIN);
    return static_cast<uint32_t>(static_cast<int32_t>(q));
}

// smin(a, b, k) → the smooth minimum of two distances: two shapes FLOW into one another instead of
// merely overlapping (Quilez). `k` is the blend radius, 0 a plain min. Wraps draw::smin, so a
// script and a compiled effect melt shapes identically.
//

// beat(bpm) / beatsin(bpm, low, high) → the TIME vocabulary an animation is actually written in.
//
// An effect does not think in milliseconds, it thinks in beats: `beat` is a rising sawtooth at a
// given BPM, `beatsin` a sine oscillating between two bounds. Both wrap math8.h's beat8/beatsin16,
// the same functions the compiled effects use (GEQ3D, FreqSaws, Lines) with the same FastLED
// semantics, so a script writes what an effect writer writes.
//
// SIXTEEN bit, not eight. A script's values are 32-bit, so an 8-bit beat would throw away range for
// nothing and cap a sweep at 255: short of the 128x128 walls this drives, and short of what
// LinesEffect itself computes (a 16-bit beat scaled by the axis length). The full-scale range means
// `beat(30) * width` and a shift is the sweep position on ANY fixture size.
//
// `ms` is an explicit argument: a script writes `beat(30, t)`. Threading the clock implicitly was
// tried and is worse: a Call receives exactly the arguments the script names, so an implicit `ms`
// arrives as zero and the animation silently stands still. Explicit also matches the C++ signature
// (beat16(bpm, ms)), so a script and an effect read the same. The modulo and divide these need live
// in the host function, which is why they are Calls: no ISA here has a cheap integer divide.
extern "C" inline uint32_t mm_ml_beat(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t bpm = uint32_t(args[0]), ms = uint32_t(args[1]);
    return beat16(static_cast<uint8_t>(bpm), ms);
}

extern "C" inline uint32_t mm_ml_beatsin(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t bpm = uint32_t(args[0]), ms = uint32_t(args[1]), high = uint32_t(args[2]);
    // low is 0 and high is the caller's: a Call carries three arguments and bpm + ms take two, so
    // the common "oscillate from 0 up to N" form is the one exposed rather than a packed pair.
    return beatsin16(static_cast<uint8_t>(bpm), ms, 0, static_cast<uint16_t>(high));
}

// noise(x, y, z) → the 0..255 gradient-noise field at that point, the primitive behind fire, clouds,
// plasma and lava. Coordinates are 16.0 fixed point: the HIGH byte picks the noise cell and the low
// byte interpolates within it, so `x * 256 / scale` zooms and feeding `t` into an axis makes the
// field flow. Three arguments is exactly a Call's budget, and 2D is the same call with z held at a
// constant: one builtin rather than an arity family.
extern "C" inline uint32_t mm_ml_noise(const uintptr_t* args, uint32_t, const uint8_t*) {
    return inoise8(uint32_t(args[0]), uint32_t(args[1]), uint32_t(args[2]));
}

extern "C" inline uint32_t mm_ml_sin(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t angle = uint32_t(args[0]);
    return static_cast<uint32_t>(sin16(static_cast<angle16>(angle)) + 32768);
}

extern "C" inline uint32_t mm_ml_cos(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t angle = uint32_t(args[0]);
    return static_cast<uint32_t>(cos16(static_cast<angle16>(angle)) + 32768);
}

// turn(n) → the angle step that divides one full revolution into n parts. A full turn is 65536,
// one past the largest number a script can write: so even with a divide operator the expression
// could not be spelled. A circle therefore needs this as a builtin rather than as arithmetic.
extern "C" inline uint32_t mm_ml_turn(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t n = uint32_t(args[0]);
    return n ? 65536u / n : 0u;
}

extern "C" inline uint32_t mm_ml_scale(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t value = uint32_t(args[0]), n = uint32_t(args[1]);
    return (value * n) >> 16;
}

extern "C" inline uint32_t mm_ml_print(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t v = uint32_t(args[0]);
    // Claim one unit before printing, and only if there is one: a plain `if (left > 0) --left`
    // can underflow past zero when two threads pass the test together.
    auto& left = printBudget();
    uint32_t have = left.load(std::memory_order_relaxed);
    while (have > 0 && !left.compare_exchange_weak(have, have - 1, std::memory_order_relaxed)) {}
    if (have > 0) {
        std::printf("[script] %u\n", static_cast<unsigned>(v));
        if (have == 1) std::printf("[script] (burst spent; edit the script for a fresh one)\n");
    }
    return v;
}

/// The neutral builtins, registered into whatever table asks. Both vocabularies call this first and
/// then add their own, so a name means the same thing in an effect and in a service.
inline void addCommonBuiltins(BuiltinTable& t) {
    // mod/div: the operators a script cannot spell, since `%` and `/` are not in the grammar.
    t.add({"mod", 2, /*returns*/ true, BuiltinKind::Call, &mm_ml_mod, {}});
    t.add({"div", 2, /*returns*/ true, BuiltinKind::Call, &mm_ml_div, {}});
    // fdiv: the fixed-point divide, whose operands and result are Q16.16.
    t.add({"fdiv", 2, /*returns*/ true, BuiltinKind::Call, &mm_ml_fdiv, {},
           /*byRef*/ 0, /*byStr*/ 0, /*fixedArgs*/ 0x3, /*fixedReturn*/ true});
    // The time vocabulary: a beat, and a sine riding it.
    t.add({"beat", 2, /*returns*/ true, BuiltinKind::Call, &mm_ml_beat, {}});
    t.add({"beatsin", 3, /*returns*/ true, BuiltinKind::Call, &mm_ml_beatsin, {}});
    t.add({"noise", 3, /*returns*/ true, BuiltinKind::Call, &mm_ml_noise, {}});
    // The circle. One turn is 0..65535, so a loop over N points steps by turn(N).
    t.add({"sin", 1, /*returns*/ true, BuiltinKind::Call, &mm_ml_sin, {}});
    t.add({"cos", 1, /*returns*/ true, BuiltinKind::Call, &mm_ml_cos, {}});
    t.add({"turn", 1, /*returns*/ true, BuiltinKind::Call, &mm_ml_turn, {}});
    t.add({"scale", 2, /*returns*/ true, BuiltinKind::Call, &mm_ml_scale, {}});
    t.add({"random16", 1, /*returns*/ true, BuiltinKind::Call, &mm_ml_random16, {}});
    // print: the script author's only debugger.
    t.add({"print", 1, /*returns*/ true, BuiltinKind::Call, &mm_ml_print, {}});
}

}  // namespace mm::moonlive
