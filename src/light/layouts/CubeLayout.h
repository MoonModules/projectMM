#pragma once

#include <limits>
#include <cstdint>
#include "light/layouts/Layouts.h"
#include "light/light_types.h"  // lengthType, nrOfLightsType

namespace mm {

// A solid cube of lights: every integer lattice point in a width×height×depth
// box gets one LED. Unlike SphereLayout (a shell), this is a filled volume, so
// lightCount() is simply the product of the three edges.
//
// The interesting geometry is the WIRING ORDER — the sequence in which the LEDs
// are addressed. A physical cube is built from a strip that snakes through the
// volume, and which axis the strip runs along fastest (plus whether it reverses
// direction on alternate passes — boustrophedon / "snake") determines the
// index→position mapping. This layout reproduces MoonLight's Cube: a 6-way
// axis-order select, a per-axis increasing-direction flag, and a per-axis snake
// toggle. The emitted COORDINATE is always the true (x,y,z); only the ORDER of
// emission (the driver index) changes with these controls — the same principle
// as GridLayout's serpentine, generalised to three axes.
//
// Prior art: MoonLight CubeLayout (github.com/MoonModules/MoonLight). MoonLight
// drives a `Wiring` helper with per-plane pins (nextPin() per plane); projectMM
// layouts emit coordinates only — the driver owns pins — so that plumbing is
// dropped and only the geometry is kept.
class CubeLayout : public LayoutBase {
public:
    // Cube edges. Defaults 10×10×10, range 1..128 — MoonLight's exact defaults.
    lengthType width = 10;
    lengthType height = 10;
    lengthType depth = 10;

    // Wiring order: which axis the strip runs along fastest. Index into the
    // axisOrders table below. Default 3 → {Y,X,Z} (Y outer, X middle, Z inner),
    // matching MoonLight's `panels` default.
    uint8_t wiringOrder = 3;

    // Per-axis increasing direction. When set, that axis is scanned from 0 up;
    // when clear, from high down. All true by default (MoonLight `inc`).
    bool incX = true;
    bool incY = true;
    bool incZ = true;

    // Per-axis snake (boustrophedon): reverse this axis's scan direction on
    // alternate passes of the enclosing loop. Defaults {false, true, false}
    // (snake on Y only) — MoonLight `snake`.
    bool snakeX = false;
    bool snakeY = true;
    bool snakeZ = false;

    void onBuildControls() override {
        controls_.addInt16("width",  width,  1, 128);
        controls_.addInt16("height", height, 1, 128);
        controls_.addInt16("depth",  depth,  1, 128);
        controls_.addSelect("wiringOrder", wiringOrder, kWiringOrderOptions, kWiringOrderCount);
        controls_.addBool("X++", incX);
        controls_.addBool("Y++", incY);
        controls_.addBool("Z++", incZ);
        controls_.addBool("snakeX", snakeX);
        controls_.addBool("snakeY", snakeY);
        controls_.addBool("snakeZ", snakeZ);
    }

    const char* tags() const override { return "💫"; }  // MoonLight origin

    nrOfLightsType lightCount() const override {
        // Solid volume: product of the three edges. Multiply in uint32_t and
        // clamp to nrOfLightsType's max before casting, as GridLayout does.
        uint32_t n = static_cast<uint32_t>(width) * height * depth;
        constexpr uint32_t kMax = std::numeric_limits<nrOfLightsType>::max();
        return static_cast<nrOfLightsType>(n > kMax ? kMax : n);
    }

    void forEachCoord(CoordCallback cb, void* ctx) const override {
        // Choose the axis order (which loop slot drives which axis). axes[0] is
        // the OUTERMOST loop's axis, axes[2] the innermost (fastest). Verbatim
        // from MoonLight: index 0=X,1=Y,2=Z. Guard an out-of-range select.
        const uint8_t* axes = kAxisOrders[wiringOrder < kWiringOrderCount ? wiringOrder : 0];

        const lengthType size[3]  = { width, height, depth };
        const bool        inc[3]  = { incX, incY, incZ };
        const bool        snake[3]= { snakeX, snakeY, snakeZ };

        const uint32_t limit = lightCount();
        uint32_t idx = 0;

        // Three nested serpentine passes. `i`, `j`, `k` are the loop counters for
        // the outer/middle/inner slots; the direction of each pass is resolved by
        // axisValue() from that axis's inc/snake plus the enclosing counter.
        //
        // RECONSTRUCTED: MoonLight expresses this via a `Wiring::iterate(slot,
        // prev, cb)` helper that is NOT in the provided sources. Reconstructed
        // here as the standard boustrophedon walk implied by its usage and
        // defaults: inc[axis] picks the base scan direction (true = ascending,
        // matching the all-true default), and snake[axis] reverses that direction
        // on odd passes of the enclosing loop — the same snaking GridLayout
        // applies on one axis, generalised to three. The snake toggle keys on the
        // enclosing loop *counter* (physical pass number), the physically-correct
        // boustrophedon; for the all-inc default that counter equals the emitted
        // coordinate, so this reproduces MoonLight's default exactly.
        const uint8_t a0 = axes[0], a1 = axes[1], a2 = axes[2];
        const lengthType n0 = size[a0], n1 = size[a1], n2 = size[a2];

        for (lengthType i = 0; i < n0 && idx < limit; i++) {
            const lengthType v0 = axisValue(i, n0, inc[a0], snake[a0], 0);
            for (lengthType j = 0; j < n1 && idx < limit; j++) {
                const lengthType v1 = axisValue(j, n1, inc[a1], snake[a1], i);
                for (lengthType k = 0; k < n2 && idx < limit; k++) {
                    const lengthType v2 = axisValue(k, n2, inc[a2], snake[a2], j);

                    // Scatter the three loop values onto their real axes:
                    // coords[axes[slot]] = value, then emit (x,y,z). Verbatim
                    // from MoonLight's coords[] fill.
                    lengthType coords[3] = {0, 0, 0};
                    coords[a0] = v0;
                    coords[a1] = v1;
                    coords[a2] = v2;
                    cb(ctx, static_cast<nrOfLightsType>(idx++),
                       coords[0], coords[1], coords[2]);
                }
            }
        }
    }

private:
    // MoonLight's axisOrders[6][3], verbatim. Rows are {outer, middle, inner}
    // axis indices (0=X,1=Y,2=Z). The wiringOrder select maps 1:1 onto these,
    // in the same order MoonLight lists its addControlValue() labels.
    static constexpr uint8_t kAxisOrders[6][3] = {
        {2, 1, 0},  // XYZ label — Z outer, Y middle, X inner (X fastest)
        {2, 0, 1},  // YXZ       — Z, X, Y
        {1, 2, 0},  // XZY       — Y, Z, X
        {1, 0, 2},  // YZX       — Y, X, Z   (default, wiringOrder = 3)
        {0, 2, 1},  // ZXY       — X, Z, Y
        {0, 1, 2},  // ZYX       — X, Y, Z
    };
    static constexpr const char* kWiringOrderOptions[6] = {
        "XYZ", "YXZ", "XZY", "YZX", "ZXY", "ZYX"
    };
    static constexpr uint8_t kWiringOrderCount = 6;

    // Resolve one serpentine pass's value for step `step` (0..count-1). Base
    // direction from `inc`; `snake` flips it when the enclosing counter `prev`
    // is odd — the boustrophedon toggle. RECONSTRUCTED (see forEachCoord).
    static lengthType axisValue(lengthType step, lengthType count,
                                bool inc, bool snake, lengthType prev) {
        bool ascending = inc;
        if (snake && (prev & 1)) ascending = !ascending;
        return ascending ? step : static_cast<lengthType>(count - 1 - step);
    }
};

} // namespace mm