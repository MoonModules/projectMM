// @module MicModule
// @also AudioSpectrumEffect

#include "doctest.h"
#include "light/AudioBands.h"
#include "platform/platform.h"   // platform::audioFft (desktop naive DFT)

#include <cmath>
#include <vector>

// The success spec for the frequency path, written RED before MicModule's FFT
// call exists. The whole pipeline runs host-side: synthesize a sine ->
// applyWindow -> platform::audioFft (the desktop reference DFT) ->
// magnitudesToBands, then assert the energy lands in the right band and the
// reported peak frequency tracks the tone. This is the coverage that lets the
// band-map tuning happen in CI instead of on the bench over months.

namespace {

constexpr size_t kN = 256;            // FFT size (power of two)
constexpr uint32_t kRate = 22050;     // default sample rate

// Build kN samples of a sine at `freqHz`, 24-bit amplitude, in the int32 slot.
std::vector<int32_t> tone(double freqHz, double amp24 = (1 << 21)) {
    std::vector<int32_t> v(kN);
    const double cycles = freqHz * kN / kRate;
    for (size_t i = 0; i < kN; i++) {
        const double s = amp24 * std::sin(2.0 * M_PI * cycles * static_cast<double>(i) / kN);
        v[i] = static_cast<int32_t>(s) << 8;
    }
    return v;
}

// Run the full window -> FFT -> bands pipeline; return the frame fields of
// interest. noiseFloor=80 / gain=80 set a dB window (~100 dB floor, ~40 dB span)
// that brackets the test tones' ~125 dB level, so a dominant band stands clearly
// above the others instead of every band saturating at the top of the window.
void analyse(const std::vector<int32_t>& samples, uint8_t (&bands)[16],
             uint16_t& peakHz, uint16_t& peakMag) {
    std::vector<float> windowed(kN), mag(kN / 2);
    mm::applyWindow(samples.data(), kN, windowed.data());
    mm::platform::audioFft(windowed.data(), kN, mag.data());
    mm::magnitudesToBands(mag.data(), kN / 2, kRate, /*noiseFloor*/80, /*gain*/80, bands, peakHz, peakMag);
}

// Index of the loudest band.
int dominantBand(const uint8_t (&bands)[16]) {
    int best = 0;
    for (int b = 1; b < 16; b++) if (bands[b] > bands[best]) best = b;
    return best;
}

} // namespace

TEST_CASE("AudioBands: silence yields all-zero bands and no peak") {
    std::vector<int32_t> s(kN, 0);
    uint8_t bands[16];
    uint16_t peakHz = 9999, peakMag = 9999;
    analyse(s, bands, peakHz, peakMag);
    for (int b = 0; b < 16; b++) CHECK(bands[b] == 0);
    CHECK(peakHz == 0);
    CHECK(peakMag == 0);
}

TEST_CASE("AudioBands: a low tone lands in a low band, a high tone in a high band") {
    uint8_t lo[16], hi[16];
    uint16_t hz, mag;
    analyse(tone(500.0), lo, hz, mag);     // bass
    analyse(tone(8000.0), hi, hz, mag);    // treble (under the ~11 kHz Nyquist)
    const int loBand = dominantBand(lo);
    const int hiBand = dominantBand(hi);
    CHECK(hiBand > loBand);                // higher frequency → higher band index
}

TEST_CASE("AudioBands: the reported peak frequency tracks the played tone") {
    for (double f : {1000.0, 3000.0, 6000.0}) {
        uint8_t bands[16];
        uint16_t peakHz, peakMag;
        analyse(tone(f), bands, peakHz, peakMag);
        REQUIRE(peakMag > 0);
        // Within one FFT bin (rate/N ≈ 86 Hz) of the true tone.
        const double binHz = static_cast<double>(kRate) / kN;
        CHECK(std::abs(static_cast<double>(peakHz) - f) <= binHz * 1.5);
    }
}

TEST_CASE("AudioBands: a single tone concentrates energy, not smears it everywhere") {
    uint8_t bands[16];
    uint16_t peakHz, peakMag;
    analyse(tone(2000.0), bands, peakHz, peakMag);
    const int dom = dominantBand(bands);
    // The dominant band is clearly above the average — the window/FFT isolated it.
    int sum = 0;
    for (int b = 0; b < 16; b++) sum += bands[b];
    const int avg = sum / 16;
    CHECK(bands[dom] > avg * 2);
}

TEST_CASE("AudioBands: noiseFloor gates a low idle spectrum to zero, gain scales it back") {
    // A weak tone that lands a small but nonzero band magnitude at unity gain.
    auto quiet = tone(2000.0, 1 << 12);   // small amplitude
    std::vector<float> windowed(kN), mag(kN / 2);
    mm::applyWindow(quiet.data(), kN, windowed.data());
    mm::platform::audioFft(windowed.data(), kN, mag.data());

    uint8_t open[16], gated[16];
    uint16_t hz, pm;
    mm::magnitudesToBands(mag.data(), kN / 2, kRate, /*noiseFloor*/0, 16, open, hz, pm);
    int openMax = 0;
    for (int b = 0; b < 16; b++) if (open[b] > openMax) openMax = open[b];
    REQUIRE(openMax > 0);   // something lights with the gate open

    // A noiseFloor above that level zeroes every band — the fix for idle flicker.
    mm::magnitudesToBands(mag.data(), kN / 2, kRate,
                          static_cast<uint16_t>(openMax + 50), 16, gated, hz, pm);
    for (int b = 0; b < 16; b++) CHECK(gated[b] == 0);
}

TEST_CASE("AudioBands: zero / degenerate input never crashes") {
    uint8_t bands[16];
    uint16_t peakHz, peakMag;
    mm::magnitudesToBands(nullptr, 8, kRate, 0, 16, bands, peakHz, peakMag);
    CHECK(peakMag == 0);
    float mag1 = 1.0f;
    mm::magnitudesToBands(&mag1, 0, kRate, 0, 16, bands, peakHz, peakMag);   // nMag 0
    CHECK(peakMag == 0);
    mm::magnitudesToBands(&mag1, 1, 0, 0, 16, bands, peakHz, peakMag);       // rate 0
    CHECK(peakMag == 0);

    // applyWindow on degenerate sizes is a no-op, not a crash.
    float out1 = 123.0f;
    int32_t s = 7 << 8;
    mm::applyWindow(&s, 0, &out1);
    mm::applyWindow(nullptr, 4, &out1);
    CHECK(true);
}
