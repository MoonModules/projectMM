#pragma once

#include "core/AudioFrame.h"
#include "core/AudioLevel.h"   // magToByte — the shared log/dB mapping

#include <cmath>     // cosf/powf — band math is inherently float (so is the
                     // audioFft seam it feeds); the recognisable DSP choice.
#include <cstddef>
#include <cstdint>

namespace mm {

// Frequency analysis for one block of mic samples — the FFT *post-processing*,
// pure domain math with no platform header (the FFT kernel itself is the one
// seam, platform.h audioFft). Host-tested: feed a synthesized sine through
// applyWindow -> audioFft (the desktop naive DFT) -> magnitudesToBands and assert
// the dominant band + peak frequency, all in CI without an ESP32.
//
// Textbook real-signal spectrum analysis, nothing exotic:
//   - applyWindow: a Hann window — the standard general-purpose DSP window.
//     Tapering the block's edges to zero stops spectral leakage (a tone smearing
//     across many bins because the block isn't a whole number of cycles). Also
//     DC-strips the 24-in-32 samples to floats for the FFT.
//   - magnitudesToBands: groups the n/2 FFT magnitude bins into 16 log-spaced
//     bands (pitch is logarithmic — bass gets few bins, treble many) with a plain
//     geometric (equal-ratio) bin split, normalises to 0..255, and picks the
//     single loudest bin as the dominant peak.

// Hann window coefficient at sample `i` of `n`: w(i) = 0.5 - 0.5*cos(2πi/(n-1)).
inline float hannWindow(size_t i, size_t n) {
    if (n <= 1) return 1.0f;
    const float x = 6.28318530718f * static_cast<float>(i) / static_cast<float>(n - 1);
    return 0.5f - 0.5f * std::cos(x);
}

// Window `n` samples (24-in-32, DC stripped) into `out` floats ready for the FFT.
// DC removal here mirrors AudioLevel's — a windowed-but-DC-biased block dumps all
// its energy into bin 0 and swamps the real peak.
inline void applyWindow(const int32_t* samples, size_t n, float* out) {
    if (!samples || !out || n == 0) return;
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += (samples[i] >> 8);
    const float mean = static_cast<float>(sum) / static_cast<float>(n);
    for (size_t i = 0; i < n; i++) {
        const float s = static_cast<float>(samples[i] >> 8) - mean;
        out[i] = s * hannWindow(i, n);
    }
}

// Group `nMag` FFT magnitudes (covering DC..Nyquist over `sampleRate`) into 16
// log-spaced bands (0..255 each) and report the dominant peak (`peakHz` = its
// frequency, `peakMag` = its 0..255 magnitude). Robust to nMag==0 (all zero).
//
// `noiseFloor` and `gain` condition the bands exactly like the level path
// (AudioLevel.h): each band's scaled magnitude has `noiseFloor` subtracted (so a
// quiet idle spectrum — the mic's own noise — gates to 0 instead of flickering
// the LEDs) and is then multiplied by `gain`/16 (16 = unity) for live brightness
// control. Same knobs, same meaning, both the level and the spectrum.
inline void magnitudesToBands(const float* mag, size_t nMag, uint32_t sampleRate,
                              uint16_t noiseFloor, uint16_t gain,
                              uint8_t bands[16], uint16_t& peakHz, uint16_t& peakMag) {
    for (uint8_t b = 0; b < 16; b++) bands[b] = 0;
    peakHz = 0;
    peakMag = 0;
    if (!mag || nMag == 0 || sampleRate == 0) return;

    // Hz per bin = sampleRate / (2 * nMag).
    const float binHz = static_cast<float>(sampleRate) / (2.0f * static_cast<float>(nMag));

    // 17 log-spaced bin-index edges: edge[e] = nMag^(e/16), so edge[0]=1 (skip
    // DC), edge[16]=nMag, each band spanning the same frequency *ratio* — a plain
    // geometric split, the standard way to map linear FFT bins onto pitch.
    size_t edge[17];
    for (uint8_t e = 0; e <= 16; e++) {
        const float frac = static_cast<float>(e) / 16.0f;
        size_t ix = static_cast<size_t>(std::pow(static_cast<float>(nMag), frac));
        if (ix < 1) ix = 1;
        if (ix > nMag) ix = nMag;
        edge[e] = ix;
    }

    // Magnitude → 0..255 on the shared LOGARITHMIC (dB) scale (magToByte, in
    // AudioLevel.h) — the same mapping the level/VU path uses, so noiseFloor/gain
    // mean one thing across both. These are the generic per-display knobs (not
    // per-band) — the recognisable "range + sensitivity" pair an analyser exposes.
    auto toByte = [noiseFloor, gain](float m) -> uint8_t {
        return magToByte(m, noiseFloor, gain);
    };

    float peakVal = 0.0f;
    size_t peakBin = 0;
    for (size_t i = 1; i < nMag; i++)              // single peak scan (skip DC)
        if (mag[i] > peakVal) { peakVal = mag[i]; peakBin = i; }

    for (uint8_t b = 0; b < 16; b++) {
        size_t lo = edge[b], hi = edge[b + 1];
        if (hi <= lo) hi = lo + 1;
        if (hi > nMag) hi = nMag;
        // Peak (not average) magnitude in the band: a narrow tone shouldn't be
        // diluted by the empty bins of a wide treble band — this is what makes a
        // single tone light ONE band instead of smearing across many.
        float best = 0.0f;
        for (size_t i = lo; i < hi; i++) if (mag[i] > best) best = mag[i];
        bands[b] = toByte(best);
    }

    if (peakVal > 0.0f) {
        peakHz = static_cast<uint16_t>(static_cast<float>(peakBin) * binHz);
        peakMag = toByte(peakVal);
    }
}

} // namespace mm
