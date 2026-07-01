#pragma once

#include <cstdint>
#include <limits>
#include "light/layouts/Layouts.h"
#include "light/light_types.h"  // lengthType, nrOfLightsType

namespace mm {

// Parallel vertical tubes: nrOfTubes single columns, each ledsPerTube lights
// tall, spaced tubeDistance apart along x (each tube's z stays 0). Tube t sits
// at x = t * tubeDistance; within a tube y runs 0..ledsPerTube-1, or reversed
// when `reversed` is set (the strip enters the column from the top). Wiring
// order is tube-major: all of tube 0's lights, then tube 1's, and so on —
// matching MoonLight's outer tube loop over an inner single-column emit.
//
// Prior art: MoonLight TubesLayout (github.com/MoonModules/MoonLight), which
// composes SingleColumnLayout per tube. projectMM emits coordinates only; the
// driver owns pin assignment, so MoonLight's per-column nextPin() plumbing is
// dropped.
class TubesLayout : public LayoutBase {
public:
    // Defaults verbatim from MoonLight (nrOfTubes 4, ledsPerTube 54,
    // tubeDistance 10, reversed off).
    lengthType nrOfTubes = 4;
    lengthType ledsPerTube = 54;
    lengthType tubeDistance = 10;
    bool reversed = false;   // when set, each tube is wired from its top (y descending)

    void onBuildControls() override {
        // MoonLight's counterparts are bare "slider" controls (uint8_t, 0..255).
        // These explicit ranges hold the geometry (≥1 tube of ≥1 light,
        // non-negative spacing) while keeping the box bounded.
        controls_.addInt16("nrOfTubes",    nrOfTubes,    1, 64);
        controls_.addInt16("ledsPerTube",  ledsPerTube,  1, 255);
        controls_.addInt16("tubeDistance", tubeDistance, 0, 255);
        controls_.addBool("reversed", reversed);
    }

    nrOfLightsType lightCount() const override {
        // Multiply in uint32_t to detect overflow before casting (GridLayout pattern).
        uint32_t n = static_cast<uint32_t>(nrOfTubes) * static_cast<uint32_t>(ledsPerTube);
        constexpr uint32_t kMax = std::numeric_limits<nrOfLightsType>::max();
        return static_cast<nrOfLightsType>(n > kMax ? kMax : n);
    }

    void forEachCoord(CoordCallback cb, void* ctx) const override {
        // uint32_t idx so it never wraps on a uint16_t nrOfLightsType; stop at the
        // clamped lightCount() so emitted indices stay within the buffer.
        const uint32_t limit = lightCount();
        uint32_t idx = 0;
        for (lengthType tube = 0; tube < nrOfTubes && idx < limit; tube++) {
            const lengthType x = static_cast<lengthType>(tube * tubeDistance);
            // Per-tube single column: reversed enters from the high-y end. The
            // emitted COORDINATE is the true (x, y, 0); only the index→position
            // order changes, exactly as MoonLight's SingleColumnLayout does.
            for (lengthType i = 0; i < ledsPerTube && idx < limit; i++) {
                const lengthType y = reversed
                    ? static_cast<lengthType>(ledsPerTube - 1 - i)
                    : i;
                cb(ctx, static_cast<nrOfLightsType>(idx++), x, y, 0);
            }
        }
    }
};

} // namespace mm
