#pragma once

#include "light/modifiers/ModifierBase.h"
#include "platform/platform.h"     // alloc / free / millis

#include <cstdint>

namespace mm {

// Randomly remaps every light to another light — a true 1:1 permutation (every light
// goes somewhere, no gaps or duplicates) — and reshuffles on a `bpm` timer. A static
// fold whose mapping changes on a beat: modifyLogical applies the permutation (the box
// is unchanged, each light maps to exactly one other), and the bpm tick (loop()) bumps
// the generation and rebuilds the Layer's mapping on a beat boundary — the same rebuild
// path a control change takes, scoped to one Layer. (Not a per-frame modifyLive: a
// permutation is a discrete reshuffle, not smooth motion, so a beat-gated rebuild is the
// right cost, not a per-frame remap.)
//
// `bpm` = reshuffles per minute (0–60, default 6 → one scramble every ~10 s; 60 → one
// per second, the cap). bpm 0 = frozen: keep the current permutation, never reshuffle.
//
// Cost: each beat re-runs the Layer's mapping rebuild on the render thread (a transient
// one-frame hitch), bounded by bpm≤60. The permutation buffer is a member sized to the
// box, (re)allocated only on a grid resize — never per frame. An alloc failure degrades
// to identity passthrough.
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

    // A remap leaves the logical box unchanged (no modifyLogicalSize override).

    // A remap leaves the box unchanged but needs it for the permutation — stash it.
    void modifyLogicalSize(Coord3D& size) override { box_ = size; }

    // Apply the permutation: fold the coord to a box index, look up its permuted index,
    // unflatten back to a coord. The first call after a resize/beat (re)builds the
    // permutation, the rest read it. const, so the permutation buffers are mutable.
    bool modifyLogical(Coord3D& pos) const override {
        const nrOfLightsType boxCount =
            static_cast<nrOfLightsType>(box_.x) * static_cast<nrOfLightsType>(box_.y) *
            static_cast<nrOfLightsType>(box_.z);
        ensurePermutation(boxCount);

        const lengthType w = box_.x > 0 ? box_.x : 1;
        const lengthType h = box_.y > 0 ? box_.y : 1;
        const nrOfLightsType idx =
            static_cast<nrOfLightsType>(pos.z) * static_cast<nrOfLightsType>(w) * static_cast<nrOfLightsType>(h) +
            static_cast<nrOfLightsType>(pos.y) * static_cast<nrOfLightsType>(w) +
            static_cast<nrOfLightsType>(pos.x);

        // Permuted index, or identity if the permutation is unavailable (OOM/empty).
        const nrOfLightsType mapped = (perm_ && idx < permCount_) ? perm_[idx] : idx;
        // Unflatten back to a coordinate in the box.
        pos.x = static_cast<lengthType>(mapped % w);
        pos.y = static_cast<lengthType>((mapped / w) % h);
        pos.z = static_cast<lengthType>(mapped / (static_cast<nrOfLightsType>(w) * h));
        return true;   // a permutation never rejects — every light maps somewhere
    }

    // Dynamic-modifier tick: accumulate the bpm timer; on each beat boundary bump the
    // generation (so the next rebuild reshuffles) and trigger the Layer's rebuild.
    // bpm 0 freezes. Layer::loop() invokes this per enabled modifier child; it sets a
    // dirty flag the Layer coalesces into one rebuild (see Layer::loop()).
    void loop() override {
        const uint32_t now = platform::millis();
        if (lastElapsed_ == 0) lastElapsed_ = now;   // first tick: no dt jump
        const uint32_t dt = now - lastElapsed_;
        lastElapsed_ = now;
        if (bpm == 0) return;                   // frozen — keep the current permutation
        // Accumulate the raw (dt * bpm) product; divide only at the read site — the same
        // integer-accumulator trick the effects use so a sub-ms dt doesn't round to zero.
        phaseNum_ += static_cast<uint64_t>(dt) * bpm;
        const uint64_t beat = phaseNum_ / 60000u;
        if (beat != lastBeat_) {
            lastBeat_ = beat;
            reshuffle();              // bump the generation; the Layer's rebuild applies it
            needsRebuild_ = true;     // Layer::loop() reads + clears this, one rebuild/frame
        }
    }

    // True iff a beat asked for a fresh permutation since the last rebuild. The Layer
    // polls this across its enabled modifiers and rebuilds once if any is set, so several
    // dynamic modifiers ticking in one frame coalesce to a single rebuild.
    bool consumeNeedsRebuild() override {
        const bool r = needsRebuild_;
        needsRebuild_ = false;
        return r;
    }

    // Bump the generation so the next rebuild reshuffles. Exposed so a test can drive a
    // reshuffle without a Layer.
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
    Coord3D box_;   // stashed in modifyLogicalSize (the box the permutation is over)
    mutable nrOfLightsType* perm_ = nullptr;
    mutable nrOfLightsType  permCount_ = 0;
    mutable uint32_t        rngState_ = 0xBADF00Du;
    mutable uint32_t        generation_ = 1;   // bumped per beat; shuffle keys off it
    mutable uint32_t        builtGen_ = 0;     // generation the current perm_ was built for

    uint64_t phaseNum_ = 0;     // dt*bpm accumulator (numerator; one beat per 60000)
    uint64_t lastBeat_ = 0;
    uint32_t lastElapsed_ = 0;
    bool     needsRebuild_ = false;   // set on a beat, consumed by Layer::loop (coalesced rebuild)
};

} // namespace mm
