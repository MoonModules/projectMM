#pragma once

#include <cmath>    // sinf, cosf
#include <cstdint>
#include <limits>   // std::numeric_limits (lightCount clamp, matches GridLayout)
#include "light/layouts/Layouts.h"
#include "light/light_types.h"  // lengthType, nrOfLightsType

namespace mm {

// A tapering conical spiral: `ledCount` lights wound up a cone whose radius
// tapers linearly from `bottomRadius` at the base to 0 at the top, over a total
// vertical rise of `height`. The winding rate is fixed at the bottom's LED
// density — ledsPerRound = 2π·bottomRadius (one light per unit of bottom
// circumference) — so the angle simply accumulates i·2π/ledsPerRound while the
// radius shrinks, giving a spiral that packs tightly near the base and opens out
// toward the tip. The whole shape is offset so its base sits at (bottomRadius,
// 0, bottomRadius), keeping every coordinate non-negative.
//
// Prior art: MoonLight SpiralLayout (MoonModules/MoonLight, src light nodes).
// Geometry reproduced exactly — the float trig runs on the cold build path
// (forEachCoord is called from a rebuild, not the render loop), so MoonLight's
// sinf/cosf and the float→integer truncation on each coordinate are kept as-is.
// MoonLight's per-strip pin plumbing (nextPin) is dropped: a projectMM layout
// emits coordinates only; the driver owns wiring.
class SpiralLayout : public LayoutBase {
public:
    lengthType ledCount = 640;      // total lights along the spiral
    lengthType bottomRadius = 10;   // radius at the base, in light-units
    lengthType height = 25;         // vertical rise from base to tip

    void onBuildControls() override {
        controls_.addInt16("ledCount",     ledCount,     1, 2048);
        controls_.addInt16("bottomRadius", bottomRadius, 1, 100);
        controls_.addInt16("height",       height,       1, 200);
    }

    nrOfLightsType lightCount() const override {
        // One light per iteration of the spiral loop below. ledCount's control
        // range (1..2048) sits well under nrOfLightsType's max on every target,
        // but clamp anyway to mirror GridLayout and stay robust to any value.
        constexpr uint32_t kMax = std::numeric_limits<nrOfLightsType>::max();
        const uint32_t n = ledCount > 0 ? static_cast<uint32_t>(ledCount) : 0;
        return static_cast<nrOfLightsType>(n > kMax ? kMax : n);
    }

    void forEachCoord(CoordCallback cb, void* ctx) const override {
        const uint32_t limit = lightCount();
        if (limit == 0) return;

        // π as a float literal — the codebase convention (see platform_desktop.cpp);
        // M_PI is not defined portably under MSVC's <cmath> without _USE_MATH_DEFINES.
        constexpr float pi = 3.14159265358979323846f;

        // Base sits at (bottomRadius, 0, bottomRadius) so all coords are >= 0.
        const float middleX = static_cast<float>(bottomRadius);
        const float middleZ = static_cast<float>(bottomRadius);

        // Winding rate: one light per unit of bottom circumference (1cm spacing
        // in MoonLight's units). ledsPerRound = 2π·bottomRadius / 1.0.
        const float bottomCircumference = 2.0f * pi * static_cast<float>(bottomRadius);
        const float ledsPerRound = bottomCircumference / 1.0f;

        for (uint32_t i = 0; i < limit; i++) {
            // progress runs 0..1 across the spiral. MoonLight divides by
            // (ledCount-1); ledCount==1 (a reachable control value) would make
            // that 0/0, so pin progress to 0 for the single-light case — the one
            // light lands at the base, full radius, angle 0. // RECONSTRUCTED
            const float progress = (ledCount > 1)
                ? static_cast<float>(i) / static_cast<float>(ledCount - 1)
                : 0.0f;

            const float currentRadius = static_cast<float>(bottomRadius) * (1.0f - progress);
            const float currentHeight = static_cast<float>(height) * progress;
            const float radians = static_cast<float>(i) * 2.0f * pi / ledsPerRound;

            const float x = currentRadius * sinf(radians);
            const float y = currentHeight;
            const float z = currentRadius * cosf(radians);

            cb(ctx, static_cast<nrOfLightsType>(i),
               static_cast<lengthType>(x + middleX),
               static_cast<lengthType>(y),
               static_cast<lengthType>(z + middleZ));
        }
    }
};

} // namespace mm