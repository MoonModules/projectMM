#pragma once

#include <cstdint>
#include "light/layouts/Layouts.h"
#include "light/light_types.h"  // lengthType, nrOfLightsType

namespace mm {

// A hollow sphere: lights sit on the surface only (a one-light-thick shell),
// not the interior. Lattice layout — every light is at an integer (x,y,z) in a
// (2r+1)^3 bounding box, centred at (r,r,r). A lattice point is on the shell
// when its distance from the centre rounds to `radius`, i.e. it falls in the
// half-open band [radius-0.5, radius+0.5). The same band predicate drives both
// lightCount() (count) and forEachCoord() (emit), so they never disagree.
//
// Distances are compared squared (integer math, no sqrt/float per light) — the
// hot-path discipline (integer math, no float per light) applies here even
// though layout iteration is a cold path, because the same pattern reads
// uniformly across the codebase.
class SphereLayout : public LayoutBase {
public:
    // Surface radius in light-units. Min 1 (a radius-1 shell is the 6 axis
    // neighbours of the centre — the smallest recognisable hollow sphere).
    // Max 64 keeps the (2*64+1)^3 bounding-box scan bounded.
    lengthType radius = 4;

    void onBuildControls() override {
        controls_.addInt16("radius", radius, 1, 64);
    }

    nrOfLightsType lightCount() const override {
        // Count the shell points. Cheap relative to rendering, recomputed only
        // on a radius change (controlChangeTriggersBuildState → rebuild).
        nrOfLightsType n = 0;
        forEachShellPoint([](void*, nrOfLightsType, lengthType, lengthType, lengthType) {}, nullptr, &n);
        return n;
    }

    void forEachCoord(CoordCallback cb, void* ctx) const override {
        forEachShellPoint(cb, ctx, nullptr);
    }

private:
    // Walk the (2r+1)^3 lattice, invoking cb for each shell point with a
    // sequential index. When `count` is non-null, also tally the points (so
    // lightCount() reuses the exact same predicate as the iterator). Either cb
    // or count (or both) may be active; cb is a no-op lambda in the count path.
    void forEachShellPoint(CoordCallback cb, void* ctx, nrOfLightsType* count) const {
        const int32_t r = radius;
        // Half-open band [r-0.5, r+0.5) compared in squared integer space:
        //   lo = (2r-1)^2 / 4 ... but keep it integer by comparing 4*d^2 to
        //   (2r-1)^2 and (2r+1)^2 — multiply the whole inequality by 4.
        const int32_t lo = (2 * r - 1) * (2 * r - 1);   // 4*(r-0.5)^2
        const int32_t hi = (2 * r + 1) * (2 * r + 1);   // 4*(r+0.5)^2
        nrOfLightsType idx = 0;
        for (int32_t z = 0; z <= 2 * r; z++) {
            const int32_t dz = z - r;
            for (int32_t y = 0; y <= 2 * r; y++) {
                const int32_t dy = y - r;
                for (int32_t x = 0; x <= 2 * r; x++) {
                    const int32_t dx = x - r;
                    const int32_t d4 = 4 * (dx * dx + dy * dy + dz * dz);
                    if (d4 < lo || d4 >= hi) continue;
                    cb(ctx, idx,
                       static_cast<lengthType>(x),
                       static_cast<lengthType>(y),
                       static_cast<lengthType>(z));
                    idx++;
                }
            }
        }
        if (count) *count = idx;
    }
};

} // namespace mm
