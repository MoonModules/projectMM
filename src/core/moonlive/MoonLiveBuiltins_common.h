#pragma once

#include "core/moonlive/MoonLiveBuiltins.h"
#include "core/util/math8.h"    // beatsin16: the shared time vocabulary
#include "core/util/math16.h"   // beat16 / sin16 / cos16: full-range waveforms
#include "core/util/noise.h"    // inoise8: the shared gradient-noise field

#include <atomic>
#include <cstdint>
#include <cstdio>

/// @defgroup moonlive_builtins_common MoonLive neutral builtins
/// @{
/// The domain-neutral half of the script vocabulary: arithmetic, waveforms, noise, randomness and print.
///
/// None of it is about light, so none of it belongs to the light domain.
///
/// @moreinfo
///
/// ## Why it left the light header
///
/// It lived there because that was the only vocabulary there was.
/// When the service table arrived it had to reach into the light header for `print` and `addControl`, which is core depending on a domain.
/// That is the wrong direction, and the reason a service script could not call `sin` while an effect could, for no reason either could explain.
///
/// ## What stays on the light side
///
/// Whatever genuinely needs a canvas: the pixel writes, the palette and particle helpers, the audio frame, and the per-light coordinates.

namespace mm::moonlive {

/// A script argument read as signed, which is what every builtin taking a coordinate wants.
inline int32_t signedArg(uintptr_t a) {
    return static_cast<int32_t>(uint32_t(a));
}

/// The remaining print budget, reset by a binding when it compiles.
inline std::atomic<uint32_t>& printBudget() { static std::atomic<uint32_t> n{0}; return n; }

/// Grant a fresh print burst, called from a binding's prepare alongside the compile.
inline void resetPrintBudget() { printBudget().store(32, std::memory_order_relaxed); }

extern "C" inline uint32_t mm_ml_random16(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t n = uint32_t(args[0]);
    // Atomic: two threads run scripts at once, and a lost update repeats a "random" value.
    static std::atomic<uint32_t> seed{0x2545F491u};
    uint32_t prev = seed.load(std::memory_order_relaxed), next;
    do {
        next = prev * 1664525u + 1013904223u;
    } while (!seed.compare_exchange_weak(prev, next, std::memory_order_relaxed));
    return n ? (next >> 16) % n : 0u;
}

/// The signed remainder, which is the wrap a cyclic animation needs; zero divisor answers 0.
extern "C" inline uint32_t mm_ml_mod(const uintptr_t* args, uint32_t, const uint8_t*) {
    const int32_t a = static_cast<int32_t>(uint32_t(args[0]));
    const int32_t b = static_cast<int32_t>(uint32_t(args[1]));
    // INT32_MIN % -1 traps on x86-64 where the other ISAs wrap, so a bench never shows it.
    if (b == 0 || (a == INT32_MIN && b == -1)) return 0;
    return static_cast<uint32_t>(a % b);
}

// A zero divisor saturates toward the numerator's sign, so a ripple's center reads as its peak.
/// The signed quotient, which the `/` operator lowers to.
extern "C" inline uint32_t mm_ml_div(const uintptr_t* args, uint32_t, const uint8_t*) {
    const int32_t a = static_cast<int32_t>(uint32_t(args[0]));
    const int32_t b = static_cast<int32_t>(uint32_t(args[1]));
    if (b == 0)
        return static_cast<uint32_t>(a > 0 ? INT32_MAX : a < 0 ? INT32_MIN : 0);
    // INT32_MIN / -1 is a SIGFPE on x86-64, and saturating reads better than "division broke".
    if (a == INT32_MIN && b == -1) return static_cast<uint32_t>(INT32_MAX);
    return static_cast<uint32_t>(a / b);
}

// The numerator widens in int64, since a register shift wraps past |128.0| and froze two shaders.
/// The Q16.16 quotient, which `/` lowers to when both sides are fixed.
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


/// A rising sawtooth at a given BPM, full scale, so scaling it sweeps any fixture size.
extern "C" inline uint32_t mm_ml_beat(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t bpm = uint32_t(args[0]), ms = uint32_t(args[1]);
    return beat16(static_cast<uint8_t>(bpm), ms);
}

extern "C" inline uint32_t mm_ml_beatsin(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t bpm = uint32_t(args[0]), ms = uint32_t(args[1]), high = uint32_t(args[2]);
    // A Call carries three arguments, so the low bound is 0 rather than a packed pair.
    return beatsin16(static_cast<uint8_t>(bpm), ms, 0, static_cast<uint16_t>(high));
}

/// The gradient-noise field at a point, whose high byte picks the cell and low byte interpolates.
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

// A builtin because a full turn is 65536, one past the largest number a script can write.
/// The angle step dividing one revolution into n parts.
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
    // Claimed before printing, since `if (left > 0) --left` underflows when two threads race.
    auto& left = printBudget();
    uint32_t have = left.load(std::memory_order_relaxed);
    while (have > 0 && !left.compare_exchange_weak(have, have - 1, std::memory_order_relaxed)) {}
    if (have > 0) {
        std::printf("[script] %u\n", static_cast<unsigned>(v));
        if (have == 1) std::printf("[script] (burst spent; edit the script for a fresh one)\n");
    }
    return v;
}

// Both vocabularies call this first and then add their own, so a name means one thing everywhere.
/// Register the neutral builtins into whatever table asks.
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

/// @}

}  // namespace mm::moonlive
