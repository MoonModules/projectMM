// @module MicModule
// @also AudioVolumeEffect

#include "doctest.h"
#include "light/AudioLevel.h"
#include "core/MicModule.h"
#include "core/ModuleFactory.h"
#include "light/effects/AudioVolumeEffect.h"
#include "light/effects/AudioSpectrumEffect.h"

#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

// The success spec for the level path, written RED before MicModule's reader
// exists: the two I2S-mic facts that AudioLevel.h handles must hold on
// synthesized blocks — DC offset is removed (a biased-but-silent block reads 0),
// the noise floor gates quiet hiss, gain scales what survives — and the whole
// thing is crash-safe on empty/degenerate input.

namespace {

// A pure sine of `cycles` periods across `n` samples at 24-bit-ish amplitude,
// left-justified into the int32 slot (<<8) the INMP441 produces, plus an
// optional DC bias to prove the bias is stripped.
std::vector<int32_t> sine(size_t n, double cycles, double amp24, double dc24 = 0.0) {
    constexpr double kPi = std::numbers::pi_v<double>;
    std::vector<int32_t> v(n);
    for (size_t i = 0; i < n; i++) {
        const double s = amp24 * std::sin(2.0 * kPi * cycles * static_cast<double>(i) / n) + dc24;
        // <<8 on a wider signed type — left-shifting a negative int32 is UB.
        const int64_t sample = static_cast<int64_t>(s) << 8;   // 24-bit into the high bits
        v[i] = static_cast<int32_t>(sample);
    }
    return v;
}

} // namespace

TEST_CASE("AudioLevel: silence reads zero") {
    std::vector<int32_t> s(512, 0);
    mm::AudioFrame f;
    mm::computeLevel(s.data(), s.size(), /*noiseFloor*/0, /*gain*/16, f);
    CHECK(f.level == 0);
}

TEST_CASE("AudioLevel: pure DC reads zero (DC offset stripped)") {
    // A big constant bias, no AC — the DC must be stripped: RMS ~0, not huge.
    std::vector<int32_t> s(512, (1 << 22) << 8);
    mm::AudioFrame f;
    mm::computeLevel(s.data(), s.size(), 0, 16, f);
    CHECK(f.level == 0);
}

TEST_CASE("AudioLevel: a loud sine reads a higher level than a quiet one") {
    // A wide dB window (gain 40) so both land inside it, not both at 255.
    auto loud = sine(512, 8, 1 << 14);
    auto quiet = sine(512, 8, 1 << 11);
    mm::AudioFrame fl, fq;
    mm::computeLevel(loud.data(), loud.size(), 0, 40, fl);
    mm::computeLevel(quiet.data(), quiet.size(), 0, 40, fq);
    CHECK(fl.level > fq.level);          // the log-scaled level tracks amplitude
}

TEST_CASE("AudioLevel: DC bias does not change the level of a sine") {
    auto clean = sine(512, 8, 1 << 14, 0.0);
    auto biased = sine(512, 8, 1 << 14, 1 << 22);   // same AC, huge DC
    mm::AudioFrame fc, fb;
    mm::computeLevel(clean.data(), clean.size(), 0, 40, fc);
    mm::computeLevel(biased.data(), biased.size(), 0, 40, fb);
    // The DC strip makes the two read the same level (within quantisation).
    const int diff = static_cast<int>(fc.level) - static_cast<int>(fb.level);
    CHECK(std::abs(diff) <= 2);
}

TEST_CASE("AudioLevel: a high noiseFloor (dB floor) gates a modest signal to zero") {
    auto s = sine(512, 8, 1 << 14);   // a modest level
    mm::AudioFrame lo, hi;
    mm::computeLevel(s.data(), s.size(), /*noiseFloor*/0, /*gain*/40, lo);
    REQUIRE(lo.level > 0);                                  // shows with a low floor
    // Raising the dB floor above the signal's level zeroes the displayed level.
    mm::computeLevel(s.data(), s.size(), /*noiseFloor*/255, /*gain*/40, hi);
    CHECK(hi.level == 0);
}

TEST_CASE("AudioLevel: higher gain (narrower dB window) reads a higher level") {
    auto s = sine(512, 8, 1 << 14);
    mm::AudioFrame lo, hi;
    mm::computeLevel(s.data(), s.size(), 0, /*gain*/20, lo);   // wide window
    mm::computeLevel(s.data(), s.size(), 0, /*gain*/120, hi);  // narrower = hotter
    REQUIRE(lo.level > 0);
    CHECK(hi.level > lo.level);
}

TEST_CASE("AudioLevel: empty / null input is silence, never a crash") {
    mm::AudioFrame f;
    mm::computeLevel(nullptr, 512, 0, 16, f);
    CHECK(f.level == 0);
    int32_t dummy = 0;
    mm::computeLevel(&dummy, 0, 0, 16, f);
    CHECK(f.level == 0);
}

TEST_CASE("AudioLevel: isqrt64 matches floor(sqrt) on a spread of values") {
    const uint64_t xs[] = {0, 1, 2, 3, 4, 99, 100, 12345, 1ull << 40, (1ull << 46) + 7};
    for (uint64_t x : xs) {
        const uint64_t r = mm::isqrt64(x);
        CHECK(r * r <= x);
        CHECK((r + 1) * (r + 1) > x);
    }
}

// Regression: the boot wiring in main.cpp does
//   create("MicModule")->markWiredByCode()
// and create() returns nullptr for an UNREGISTERED type — so a missing
// registerType<MicModule> made the deref crash and the device boot-looped (found
// on the S3 bench). These pin that MicModule and the two audio effects are all
// registered + createable through the factory, and that latestFrame() is never
// null even with no mic (so a consumer added before the mic can't deref null).
TEST_CASE("MicModule + audio effects are registered and createable (boot-loop guard)") {
    mm::ModuleFactory::registerType<mm::MicModule>("MicModule");
    mm::ModuleFactory::registerType<mm::AudioVolumeEffect>("AudioVolumeEffect");
    mm::ModuleFactory::registerType<mm::AudioSpectrumEffect>("AudioSpectrumEffect");

    auto* mic = mm::ModuleFactory::create("MicModule");
    REQUIRE(mic != nullptr);
    CHECK(mic->role() == mm::ModuleRole::Peripheral);

    auto* vol = mm::ModuleFactory::create("AudioVolumeEffect");
    auto* spec = mm::ModuleFactory::create("AudioSpectrumEffect");
    REQUIRE(vol != nullptr);
    REQUIRE(spec != nullptr);

    delete mic;
    delete vol;
    delete spec;
}

TEST_CASE("MicModule::latestFrame is never null (silent frame with no active mic)") {
    const mm::AudioFrame* f = mm::MicModule::latestFrame();
    REQUIRE(f != nullptr);
    // With no mic having run setup(), it's the static silent frame.
    CHECK(f->level == 0);
    CHECK(f->peakHz == 0);
}
