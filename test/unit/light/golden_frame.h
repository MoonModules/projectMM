#pragma once

// Golden-frame harness — pins an effect's EXACT output so a refactor that claims "renders the same"
// is proved, not asserted.
//
// Why it exists: the power-function migration rewrites effect internals (a hand-rolled phase
// accumulator becomes BeatPhase, a private imap becomes map32) with the contract "pixel-identical by
// default". A behaviour test ("writes non-zero data", "varies along x") passes just as happily when
// the arithmetic drifted by one LSB, which is exactly the regression this migration can introduce.
// A hash over the rendered bytes catches it.
//
// Determinism comes from three fixed inputs: a fixed grid, a fixed frame count, and a FIXED CLOCK.
// The clock matters most — every migrated effect is time-driven, and `Layer::tick()` reads
// `platform::millis()`. `platform::setTestNowMs` (desktop-only test seam) makes the sequence
// reproducible; ScopedTestClock restores real time afterwards so an escaped clock can't silently
// wedge unrelated tests.
//
// Deliberately a HASH, not a stored frame: repo-health tracks repo size, and checking in buffers for
// dozens of effects would grow it for no diagnostic gain — a mismatch tells you the same thing
// either way, and the effect is one `--test-case` away from being re-rendered by hand.
//
// When a migration INTENTIONALLY diverges (the bench-judged cases: an analytic float trajectory
// folded onto the particle kernel), the golden is updated in the SAME commit with the reason in the
// message. An updated golden with no reason is the smell this harness exists to make visible.
//
// WHAT A GOLDEN IS NOT: a statement that the effect looks good. It pins what the code renders TODAY,
// so a refactor that claims to change nothing can be checked. Several effects are awaiting a tuning
// pass (some were generated rather than derived, and their parameters are arbitrary); when tuning
// changes one deliberately, the golden moves with it and that is the system working, not a
// regression. The rule is only: no hash moves SILENTLY.
//
// A golden is also only as strong as the effect's visible output. Two effects here saturate their
// field to full brightness at their default settings, so their frames barely vary and their hashes
// cannot detect a phase error — verified by mutation-testing (a 7x phase-rate change moved no
// bytes). Those goldens still guard the pixel-addressing path, and nothing more; do not read a
// passing hash as "the animation is correct".

#include "doctest.h"
#include "light/layers/Layer.h"
#include "light/layers/Effects.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>

namespace mm::golden {

/// Fix the clock for a deterministic render, and restore real time on scope exit — including on a
/// failed REQUIRE, which unwinds through here.
struct ScopedTestClock {
    explicit ScopedTestClock(uint32_t startMs) { platform::setTestNowMs(startMs); }
    ~ScopedTestClock() { platform::setTestNowMs(0); }   // 0 = back to the real clock
};

/// FNV-1a over the buffer. Any stable hash works; FNV-1a is 4 lines and needs no dependency.
inline uint64_t hashBuffer(const uint8_t* data, size_t bytes) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < bytes; i++) { h ^= data[i]; h *= 1099511628211ull; }
    return h;
}

/// Render `frames` ticks of one effect on a w×h×d grid at a fixed 20 ms/frame, and hash the final
/// buffer. The effect is owned by the caller so it can set controls before rendering.
///
/// 20 ms/frame is deliberate: it is the real tick20ms cadence, so the phase accumulators under test
/// see the same dt production gives them, and a sub-millisecond desktop dt (which rounds to zero in
/// a naive accumulator) cannot mask a bug.
///
/// The frame count is 200 (4 s of animation), NOT a handful: at a typical default speed the phase
/// advances only a few units over 8 frames, which on a 16-wide grid moves nothing by a whole pixel —
/// so a short render hashes two nearly-static frames and passes even when the animation is wrong.
/// This was found by mutation-testing the harness itself (perturbing an effect's bpm and watching
/// the golden still pass). 200 frames is still a millisecond-scale test.
template <typename EffectT>
uint64_t renderHash(EffectT& effect, lengthType w, lengthType h, lengthType d, uint16_t frames = 200) {
    ScopedTestClock clock(1000);   // start away from 0 so a first-tick guard is exercised

    Layouts layouts;
    GridLayout grid;
    Layer layer;
    grid.width = w; grid.height = h; grid.depth = d;
    layouts.addChild(&grid);
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.addChild(&effect);
    layer.applyState();

    for (uint16_t f = 0; f < frames; f++) {
        platform::setTestNowMs(1000 + static_cast<uint32_t>(f) * 20);
        layer.tick();
    }
    auto& buf = layer.buffer();
    REQUIRE(buf.data() != nullptr);
    return hashBuffer(buf.data(), buf.bytes());
}

/// Check a render against its golden, and on mismatch print the value to paste back in — the
/// workflow when a divergence is intentional and reviewed.
inline void checkGolden(const char* name, uint64_t actual, uint64_t expected) {
    if (actual != expected) {
        std::printf("golden mismatch for %s:\n  expected 0x%016llxull\n  actual   0x%016llxull\n"
                    "  (if this change is intended and reviewed, update the golden in the same commit)\n",
                    name, static_cast<unsigned long long>(expected),
                    static_cast<unsigned long long>(actual));
    }
    CHECK(actual == expected);
}

}  // namespace mm::golden
