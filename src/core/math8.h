#pragma once

#include "core/color.h"   // scale8 (nscale8 builds on it), RGB not needed here

#include <cstdint>

// 8-bit fixed-point math for LED effects: integer trig, beat/timing, saturating
// arithmetic, and a fast PRNG. The recognisable "lib8tion" surface — same names an
// embedded/LED developer knows — written fresh against projectMM's architecture.
//
// Prior art: FastLED's lib8tion (Mark Kriegsman). We carry the ideas + names
// (sin8/beatsin8/qadd8/nscale8/random8) and the textbook algorithms; the code is ours.
//
// All integer, LUT-backed where it pays, no float, no heap — safe in the render loop.
// Time-dependent helpers (beat8/beatsin8) take the current time in ms as a parameter so
// this stays platform-agnostic (the caller passes elapsed()/platform::millis(), keeping
// the time source at the domain edge, not buried in core).

namespace mm {

// --- Integer trig (256-step circle) -----------------------------------------
// 256-entry sine LUT: sin(2*pi*i/256)*127+128, in flash. sin8 for sine, cos8 = sin8(i+64).
inline constexpr uint8_t sin8_lut[256] = {
    128,131,134,137,140,144,147,150,153,156,159,162,165,168,171,174,
    177,179,182,185,188,191,193,196,199,201,204,206,209,211,213,216,
    218,220,222,224,226,228,230,232,234,235,237,239,240,241,243,244,
    245,246,248,249,250,250,251,252,253,253,254,254,254,255,255,255,
    255,255,255,255,254,254,254,253,253,252,251,250,250,249,248,246,
    245,244,243,241,240,239,237,235,234,232,230,228,226,224,222,220,
    218,216,213,211,209,206,204,201,199,196,193,191,188,185,182,179,
    177,174,171,168,165,162,159,156,153,150,147,144,140,137,134,131,
    128,125,122,119,116,112,109,106,103,100,97,94,91,88,85,82,
    79,77,74,71,68,65,63,60,57,55,52,50,47,45,43,40,
    38,36,34,32,30,28,26,24,22,21,19,17,16,15,13,12,
    11,10,8,7,6,6,5,4,3,3,2,2,2,1,1,1,
    1,1,1,1,2,2,2,3,3,4,5,6,6,7,8,10,
    11,12,13,15,16,17,19,21,22,24,26,28,30,32,34,36,
    38,40,43,45,47,50,52,55,57,60,63,65,68,71,74,77,
    79,82,85,88,91,94,97,100,103,106,109,112,116,119,122,125
};

constexpr uint8_t sin8(uint8_t i) { return sin8_lut[i]; }
constexpr uint8_t cos8(uint8_t i) { return sin8_lut[static_cast<uint8_t>(i + 64)]; }

// Triangle wave: 0→255 over the first half of the cycle, 255→0 over the second (the
// textbook fold of a ramp). A cheaper, sharper alternative to sin8 for some effects.
constexpr uint8_t triwave8(uint8_t i) {
    return i < 128 ? static_cast<uint8_t>(i * 2)
                   : static_cast<uint8_t>((255 - i) * 2);
}

// Fast octant atan2: angle of (x, y) as 0..255 around the full circle (x, y in int16 range).
constexpr uint8_t atan2_8(int16_t y, int16_t x) {
    uint8_t r = 0;
    if (y < 0) { y = static_cast<int16_t>(-y); r = 0x80; }
    if (x < 0) { x = static_cast<int16_t>(-x); r = static_cast<uint8_t>(r | 0x40); }
    uint8_t offset = (x > y) ? 0 : 32;
    if (x < y) { int16_t t = y; y = x; x = t; }
    uint8_t b = (x == 0) ? 0 : static_cast<uint8_t>((static_cast<uint16_t>(y) * 64) / static_cast<uint16_t>(x));
    return static_cast<uint8_t>(r + offset + b);
}

// Octagonal distance approximation: |Δ| without a sqrt (max + half-min — the cheap 8-bit norm).
constexpr uint8_t dist8(int16_t dx, int16_t dy) {
    int16_t ax = dx < 0 ? static_cast<int16_t>(-dx) : dx;
    int16_t ay = dy < 0 ? static_cast<int16_t>(-dy) : dy;
    return static_cast<uint8_t>(ax > ay ? ax + (ay >> 1) : ay + (ax >> 1));
}

// --- Saturating + scaling arithmetic ----------------------------------------
// qadd8/qsub8: add/subtract clamping at the 0..255 ends instead of wrapping — so a bright
// pixel + more stays white rather than rolling over to black (the LED-blend staple).
constexpr uint8_t qadd8(uint8_t a, uint8_t b) {
    uint16_t t = static_cast<uint16_t>(a) + b;
    return t > 255 ? 255 : static_cast<uint8_t>(t);
}
constexpr uint8_t qsub8(uint8_t a, uint8_t b) {
    return a > b ? static_cast<uint8_t>(a - b) : 0;
}

// nscale8: scale a byte by a 0..255 fraction (n/256). Same as scale8 — the `nscale8` name
// is the recognisable in-place-channel-scale spelling; one definition, two names.
constexpr uint8_t nscale8(uint8_t val, uint8_t scale) { return scale8(val, scale); }

// map8: rescale a 0..255 input onto the range [rangeStart, rangeEnd] (FastLED's map8). For
// rangeEnd >= rangeStart it's rangeStart + scale8(in, rangeEnd-rangeStart). Used to turn an audio
// band (0..255) into a bar height, a line length, etc.
constexpr uint8_t map8(uint8_t in, uint8_t rangeStart, uint8_t rangeEnd) {
    const uint8_t width = static_cast<uint8_t>(rangeEnd - rangeStart);
    return static_cast<uint8_t>(rangeStart + scale8(in, width));
}

// --- Timing / beat ----------------------------------------------------------
// The time source is passed in (ms since boot, e.g. platform::millis()/elapsed()) so core stays
// platform-agnostic. `timebase` shifts the phase origin: an effect that wants its oscillation to
// start (or jump) at a chosen moment passes that moment as the timebase, so the beat is measured
// from there. This matches FastLED's beat8(bpm, timebase) / beatsin8(bpm,low,high,timebase,phase).

// beat8: a 0..255 sawtooth completing `bpm` cycles per minute, measured from `timebase`.
constexpr uint8_t beat8(uint8_t bpm, uint32_t ms, uint32_t timebase = 0) {
    if (bpm == 0) return 0;
    const uint32_t period = 60000u / bpm;
    if (period == 0) return 0;
    const uint32_t pos = (ms - timebase) % period;
    return static_cast<uint8_t>((pos * 256u) / period);
}

// beatsin8: a sine oscillating in [low,high] at `bpm`. `timebase` shifts the phase origin (in ms);
// `phase` adds a fixed 0..255 offset to the beat (a constant lead/lag). FastLED's signature
// (bpm, low, high, timebase, phase) — `ms` (the current time) is threaded as the first non-FastLED
// arg so the function stays time-source-agnostic.
constexpr uint8_t beatsin8(uint8_t bpm, uint32_t ms, uint8_t low = 0, uint8_t high = 255,
                           uint32_t timebase = 0, uint8_t phase = 0) {
    const uint8_t beat = static_cast<uint8_t>(beat8(bpm, ms, timebase) + phase);
    const uint8_t s = sin8(beat);                      // 0..255 sine
    const uint8_t range = static_cast<uint8_t>(high - low);
    return static_cast<uint8_t>(low + scale8(s, range));
}

// beatsin16: 16-bit range version, for positions across a wide grid.
constexpr uint16_t beatsin16(uint8_t bpm, uint32_t ms, uint16_t low = 0, uint16_t high = 65535,
                             uint32_t timebase = 0, uint8_t phase = 0) {
    const uint8_t beat = static_cast<uint8_t>(beat8(bpm, ms, timebase) + phase);
    const uint8_t s = sin8(beat);                      // 0..255 sine
    const uint16_t range = static_cast<uint16_t>(high - low);
    return static_cast<uint16_t>(low + ((static_cast<uint32_t>(s) * range) >> 8));
}

// --- Fast PRNG --------------------------------------------------------------
// A small seedable xorshift — hot-path-cheap and deterministic (NOT std::rand, which is
// slow, non-reentrant, and unseedable per-effect). Each effect owns a Random8 so its
// sequence is reproducible and independent. Same shape as FastLED's random8/16.
class Random8 {
public:
    constexpr explicit Random8(uint32_t seed = 0x1234ABCDu) : state_(seed ? seed : 1u) {}
    constexpr void seed(uint32_t s) { state_ = s ? s : 1u; }
    constexpr uint8_t next8() { return static_cast<uint8_t>(advance() >> 24); }
    constexpr uint16_t next16() { return static_cast<uint16_t>(advance() >> 16); }
    // 0..bound-1 (bound>0): scale the 16-bit draw to avoid modulo bias on small bounds.
    constexpr uint8_t below(uint8_t bound) {
        return bound ? static_cast<uint8_t>((static_cast<uint32_t>(next16()) * bound) >> 16) : 0;
    }
    // min..max-1 (FastLED's random8(min,max)): an offset draw over the half-open range.
    constexpr uint8_t below(uint8_t min, uint8_t max) {
        return max > min ? static_cast<uint8_t>(min + below(static_cast<uint8_t>(max - min))) : min;
    }
private:
    constexpr uint32_t advance() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }
    uint32_t state_;
};

}  // namespace mm
