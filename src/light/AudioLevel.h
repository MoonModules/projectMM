#pragma once

#include "light/AudioFrame.h"

#include <cmath>     // log10 for the shared dB mapping
#include <cstddef>
#include <cstdint>

namespace mm {

// Shared magnitude → 0..255 mapping on a LOGARITHMIC (decibel) scale — used by
// BOTH the level path (this file) and the spectrum path (AudioBands.h) so a VU
// meter and a spectrum bar share one consistent scaling. dB = 20·log10(m) is
// mapped through a window [floorDb, floorDb+spanDb] → [0,255]:
//   - `noiseFloor` sets the window bottom: floorDb = 60 + noiseFloor/2 dB.
//   - `gain` is sensitivity, the INTUITIVE direction — HIGHER gain = NARROWER
//     window = a sound fills more of the range: spanDb = (255-gain)/4 + 4.
// Human hearing is logarithmic and FFT/RMS magnitudes span a huge range, so a
// linear map crushes the quiet or saturates the loud; this is the standard fix.
inline uint8_t magToByte(float m, uint16_t noiseFloor, uint16_t gain) {
    if (m <= 1.0f) return 0;
    const float floorDb = 60.0f + static_cast<float>(noiseFloor) * 0.5f;
    const float spanDb = static_cast<float>(255 - gain) * 0.25f + 4.0f;
    const float t = (20.0f * std::log10(m) - floorDb) / spanDb;
    if (t <= 0.0f) return 0;
    if (t >= 1.0f) return 255;
    return static_cast<uint8_t>(t * 255.0f);
}

// DC-blocker: the standard one-pole/one-zero high-pass that removes the constant
// (DC) offset and sub-bass rumble from the sample stream before any analysis —
// y[n] = x[n] - x[n-1] + R·y[n-1]. R near 1 sets the cutoff: R = 0.99 ≈ 40 Hz at
// 22 kHz. State (the two delay registers) persists across blocks, so the filter
// is continuous frame to frame. Hot-path-trivial: one subtract + one multiply-add
// per sample, two floats of state, no allocation. Host-tested.
struct DcBlocker {
    float xPrev = 0.0f;   // x[n-1]
    float yPrev = 0.0f;   // y[n-1]

    void reset() { xPrev = 0.0f; yPrev = 0.0f; }

    // Filter `n` samples in place. R is the pole (0..1); higher = lower cutoff.
    void process(int32_t* samples, size_t n, float r = 0.99f) {
        if (!samples) return;
        for (size_t i = 0; i < n; i++) {
            const float x = static_cast<float>(samples[i]);
            const float y = x - xPrev + r * yPrev;
            xPrev = x;
            yPrev = y;
            // Clamp before narrowing: casting a float outside the int32 range to
            // int32_t is undefined behaviour. A settling transient (or a degenerate
            // r) can briefly push y past the bounds, so saturate first.
            const float clamped = y < -2147483648.0f ? -2147483648.0f
                                : y >  2147483647.0f ?  2147483647.0f : y;
            samples[i] = static_cast<int32_t>(clamped);
        }
    }
};

// Sound-level (loudness) analysis for one block of I2S microphone samples — pure
// domain math, no platform header, so it is host-tested without an ESP32 (the
// platform owns only the I2S read that produces these samples; see platform.h
// audioMic*). The same host-testable shape as RmtSymbol.h / LcdSlots.h.
//
// Two facts about an I2S MEMS microphone drive the math here, both straight from
// how the part behaves (e.g. the INMP441 datasheet), not from any tuning recipe:
//   - It carries a DC bias. The 24-bit sample stream sits on a large constant
//     offset, so a plain RMS is dominated by the bias, not the sound — a silent
//     room would read "loud". Subtract the block mean first.
//   - Its quietest output is hiss, not zero. A `noiseFloor` threshold treats any
//     level below it as silence so idle hiss doesn't twitch the LEDs; `gain` then
//     scales what's left.
//
// INMP441 sample format: 24-bit signed data left-justified in a 32-bit slot, so
// the magnitude lives in the top bits. We arithmetic-shift right by 8 to land the
// 24-bit value in an int32, then accumulate in 64-bit so a full block can't
// overflow.

// 64-bit integer square root (Newton's method, converges in a handful of steps
// for our range). Free function so AudioBands.h can reuse it; the level path
// stays free of <cmath> (it is otherwise all integer), so this is the one root.
inline uint64_t isqrt64(uint64_t x) {
    if (x == 0) return 0;
    uint64_t r = x, last;
    do {
        last = r;
        r = (r + x / r) >> 1;
    } while (r < last);
    return last;
}

// Analyse `n` samples into `frame.level` — the overall RMS loudness mapped
// through the same log/dB window the bands use (magToByte), so the VU meter and
// the spectrum share one scaling and the noiseFloor/gain knobs mean the same
// thing for both. Empty/null input yields zero (silence), never a crash.
inline void computeLevel(const int32_t* samples, size_t n,
                         uint16_t noiseFloor, uint16_t gain, AudioFrame& frame) {
    if (!samples || n == 0) {
        frame.level = 0;
        return;
    }

    // DC mean of the block. 64-bit sum: n * 2^23 fits easily.
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += (samples[i] >> 8);
    const int64_t mean = sum / static_cast<int64_t>(n);

    // RMS of the DC-removed signal.
    uint64_t sqSum = 0;
    for (size_t i = 0; i < n; i++) {
        const int64_t v = (samples[i] >> 8) - mean;
        sqSum += static_cast<uint64_t>(v * v);
    }
    const uint64_t meanSq = sqSum / static_cast<uint64_t>(n);
    const uint64_t rms = isqrt64(meanSq);

    frame.level = magToByte(static_cast<float>(rms), noiseFloor, gain);
}

} // namespace mm
