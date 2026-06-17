#pragma once

#include "light/layers/Layer.h"   // ModifierBase + Layer (we call layer->onBuildState() on a beat)
#include "platform/platform.h"     // alloc / free / millis

#include <cstdint>

namespace mm {

// Randomly remaps every light to another light — a true 1:1 permutation (every light
// goes somewhere, no gaps or duplicates) — and reshuffles on a `bpm` timer. The first
// DYNAMIC modifier: a static modifier shapes the LUT once, this one re-shapes it on a
// beat. The permutation rides the existing LUT (mapToPhysical emits one destination per
// light, the same outCount=1 shape CheckerboardModifier uses); the only new machinery
// is the bpm tick (loop()) that, on a beat boundary, reshuffles and asks the Layer to
// rebuild its LUT — the same rebuild path a control change takes, scoped to one Layer.
//
// `bpm` = reshuffles per minute (0–60, default 6 → one scramble every ~10 s; 60 → one
// per second, the cap — faster would be a strobe and a per-frame rebuild is too costly).
// bpm 0 = frozen: keep the current permutation, never reshuffle (a fixed random remap).
//
// Cost: each beat re-runs the Layer's LUT rebuild on the render thread (a transient
// one-frame hitch, like a device scan), bounded by bpm≤60. The permutation buffer is a
// member sized to the light count, (re)allocated only on a grid resize — never per
// frame. An alloc failure degrades to identity passthrough (no remap), like the LUT.
//
// Sparse layouts: the permutation is over box indices, so a real light can map to a
// non-light cell (dropped → dark). Acceptable for v1.
class RandomMapModifier : public ModifierBase {
public:
    uint8_t bpm = 6;   // reshuffles per minute (0–60); 6 ≈ every 10 s; 0 = frozen

    ~RandomMapModifier() override { releasePerm(); }

    void onBuildControls() override {
        controls_.addUint8("bpm", bpm, 0, 60);
    }

    // A remap doesn't resize the logical box — logical dims == physical dims (identity),
    // like CheckerboardModifier. Each logical light maps to exactly one physical light.
    void logicalDimensions(lengthType physW, lengthType physH, lengthType physD,
                           lengthType& logW, lengthType& logH, lengthType& logD) const override {
        logW = physW;
        logH = physH;
        logD = physD;
    }

    nrOfLightsType maxMultiplier() const override { return 1; }   // 1:1, never fans out

    // Emit the permuted destination for this light. mapToPhysical is called once per box
    // during the Layer's LUT rebuild; the first call after a resize or a beat (generation
    // bump) (re)builds the permutation, the rest just read it. const, so the permutation
    // buffers are mutable.
    void mapToPhysical(lengthType lx, lengthType ly, lengthType lz,
                       lengthType physW, lengthType physH, lengthType physD,
                       nrOfLightsType* outPhysicals, nrOfLightsType& outCount,
                       nrOfLightsType maxOut) const override {
        outCount = 0;
        if (maxOut == 0) return;

        const nrOfLightsType boxCount =
            static_cast<nrOfLightsType>(physW) * static_cast<nrOfLightsType>(physH) *
            static_cast<nrOfLightsType>(physD);
        ensurePermutation(boxCount);

        const nrOfLightsType idx =
            static_cast<nrOfLightsType>(lz) * static_cast<nrOfLightsType>(physW) * static_cast<nrOfLightsType>(physH) +
            static_cast<nrOfLightsType>(ly) * static_cast<nrOfLightsType>(physW) +
            static_cast<nrOfLightsType>(lx);

        // If the permutation isn't available (alloc failed or empty grid), pass through
        // unchanged — identity remap, never a crash or a dropped frame.
        outPhysicals[0] = (perm_ && idx < permCount_) ? perm_[idx] : idx;
        outCount = 1;
    }

    // Dynamic-modifier tick: accumulate the bpm timer; on each beat boundary bump the
    // generation (so the next rebuild reshuffles) and trigger the Layer's LUT rebuild.
    // bpm 0 freezes — no accumulation, no reshuffle (the permutation stays put).
    // Overrides MoonModule::loop(); Layer::loop() invokes it per enabled modifier child.
    void loop() override {
        Layer* lyr = static_cast<Layer*>(parent());
        if (!lyr) return;
        const uint32_t now = lyr->elapsed();   // same clock the effects use this frame
        if (lastElapsed_ == 0) lastElapsed_ = now;   // first tick: no dt jump
        const uint32_t dt = now - lastElapsed_;
        lastElapsed_ = now;
        if (bpm == 0) return;                   // frozen — keep the current permutation
        // Accumulate the raw (dt * bpm) product; divide only at the read site — the same
        // integer-accumulator trick the effects use so a sub-ms dt doesn't round to zero.
        // One beat = 60000/bpm ms, i.e. phaseNum_ crossing a multiple of 60000.
        phaseNum_ += static_cast<uint64_t>(dt) * bpm;
        const uint64_t beat = phaseNum_ / 60000u;
        if (beat != lastBeat_) {
            lastBeat_ = beat;
            reshuffle();          // ask the next rebuild to produce a fresh permutation
            lyr->onBuildState();  // rebuild the LUT now (re-runs mapToPhysical → reshuffle)
        }
    }

    // Bump the generation so the next mapToPhysical pass reshuffles to a new permutation.
    // loop() calls this on a beat; exposed so a test can drive a reshuffle without a Layer.
    void reshuffle() { generation_++; }

private:
    // (Re)build the permutation when the box count changed or a beat bumped the
    // generation. Sized to the full box count; (re)allocated only on a size change.
    void ensurePermutation(nrOfLightsType boxCount) const {
        if (boxCount == 0) { releasePerm(); return; }
        if (boxCount != permCount_ || !perm_) {
            releasePerm();
            perm_ = static_cast<nrOfLightsType*>(
                platform::alloc(static_cast<size_t>(boxCount) * sizeof(nrOfLightsType)));
            if (!perm_) { permCount_ = 0; return; }   // OOM → mapToPhysical passes through identity
            permCount_ = boxCount;
            builtGen_ = generation_ - 1;              // force a shuffle below
        }
        if (builtGen_ != generation_) {
            shuffle();
            builtGen_ = generation_;
        }
    }

    // Fisher–Yates in place over [0, permCount_). Initialises to identity then shuffles,
    // so the result is always a true bijection. Integer-only LCG (no std::rand, no
    // float), seeded from the generation so each beat yields a different permutation and
    // a fixed generation is reproducible (testable).
    void shuffle() const {
        for (nrOfLightsType i = 0; i < permCount_; i++) perm_[i] = i;
        rngState_ = 0xBADF00Du ^ (generation_ * 2654435761u);
        // i from high to low; swap perm_[i-1] with perm_[j], j in [0, i).
        for (nrOfLightsType i = permCount_; i > 1; i--) {
            const nrOfLightsType j = static_cast<nrOfLightsType>(rand() % i);
            const nrOfLightsType tmp = perm_[i - 1];
            perm_[i - 1] = perm_[j];
            perm_[j] = tmp;
        }
    }

    // LCG (glibc constants), same as ParticlesEffect — fast, integer, deterministic.
    uint32_t rand() const {
        rngState_ = rngState_ * 1103515245u + 12345u;
        return rngState_ >> 8;   // drop the low bits (poor randomness in an LCG)
    }

    void releasePerm() const {
        if (perm_) { platform::free(perm_); perm_ = nullptr; }
        permCount_ = 0;
    }

    // Mutable: mapToPhysical is const but lazily (re)builds the permutation.
    mutable nrOfLightsType* perm_ = nullptr;
    mutable nrOfLightsType  permCount_ = 0;
    mutable uint32_t        rngState_ = 0xBADF00Du;
    mutable uint32_t        generation_ = 1;   // bumped per beat; shuffle keys off it
    mutable uint32_t        builtGen_ = 0;     // generation the current perm_ was built for

    uint64_t phaseNum_ = 0;     // dt*bpm accumulator (numerator; one beat per 60000)
    uint64_t lastBeat_ = 0;
    uint32_t lastElapsed_ = 0;
};

} // namespace mm
