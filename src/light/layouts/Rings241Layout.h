#pragma once

#include <cstdint>
#include <cmath>  // std::sin, std::cos on float — cold build-path ring trig
#include "light/layouts/Layouts.h"
#include "light/light_types.h"  // lengthType, nrOfLightsType

namespace mm {

// The classic 241-LED concentric-ring disc: nine full circles sharing one
// centre, with ring LED counts 1, 8, 12, 16, 24, 32, 40, 48, 60 (sum 241).
//
// Prior art: MoonLight's Rings241Layout, which composes MoonLight's RingLayout
// once per ring. RingLayout places `n` LEDs evenly on a circle of radius
// n / (2π), starting at the bottom (angleRad = π at i=0) and stepping by 2π/n.
// This port reproduces that exact per-LED math but emits coordinates only —
// MoonLight's pin/wiring plumbing (doNextPin/nextPin, and RingLayout's
// angleFirst/rotation/clockwise/nrOfLEDs UI controls) has no place here, since
// a projectMM layout hands positions to the driver and the driver owns pins.
// The one geometry control that survives is `scale`, RingLayout's spacing
// multiplier. Every ring is a full circle (MoonLight's rotation = 360), so every
// LED is emitted; that makes lightCount() the fixed constant 241.
//
// Precision is reproduced statement-for-statement, because the disc's integer
// coordinates depend on it: MoonLight forms `angleRad` from the double macros
// PI / TWO_PI, stores it in a *float*, then takes float sinf/cosf and multiplies
// a float radius. Doing the angle in all-float (a float π constant) or keeping
// it all-double both shift ring points that land exactly on an integer axis
// (cos/sin = 0) to the wrong side of the (int) truncation, differing from
// MoonLight by one unit. The faithful path is MoonLight's own: form the angle in
// double → narrow to float → float trig → float radius.
// Author: MoonLight — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h
class Rings241Layout : public LayoutBase {
public:
    // Spacing multiplier — scales both the ring radii and the shared centre.
    // MoonLight default 2, range 1..10.
    uint8_t scale = 2;

    void onBuildControls() override {
        controls_.addUint8("scale", scale, 1, 10);
    }

    nrOfLightsType lightCount() const override {
        // Fixed by construction: the nine ring sizes always sum to 241, and every
        // ring is a full circle so no LED is culled. Kept in lockstep with
        // forEachCoord below (same kRingSizes sum).
        nrOfLightsType total = 0;
        for (uint8_t n : kRingSizes) total += n;
        return total;  // 1+8+12+16+24+32+40+48+60 = 241
    }

    void forEachCoord(CoordCallback cb, void* ctx) const override {
        // Shared centre — MoonLight: leftMargin = 1.1 * getRadius(60), assigned to
        // a uint8_t (implicit truncation), stored as ringCenter's integer x/y, then
        // scaled per LED: x = scale * ringCenter.x.
        const uint8_t leftMargin = static_cast<uint8_t>(1.1 * getRadius(60));

        nrOfLightsType idx = 0;
        // Rings emitted smallest-to-largest, the same order MoonLight calls
        // RingLayout::onLayout (nrOfLEDs = 1, 8, 12, … 60).
        for (uint8_t n : kRingSizes) {
            const float radius = getRadius(n);
            for (uint8_t i = 0; i < n; i++) {
                float x = static_cast<float>(scale * leftMargin);
                float y = static_cast<float>(scale * leftMargin);
                if (n != 1) {
                    // angleFirst = 0, so RingLayout's angleRad = π + 2π·i / n.
                    // Formed in double (MoonLight's PI/TWO_PI are double macros),
                    // then narrowed to float for the float sinf/cosf below.
                    const float angleRad = static_cast<float>(kPi + (kTwoPi * static_cast<double>(i)) / static_cast<double>(n));
                    x -= scale * std::sin(angleRad) * radius;
                    y += scale * std::cos(angleRad) * radius;
                }
                // MoonLight truncates each axis with a (int) cast (toward zero);
                // z is scale * ringCenter.z = scale * 0 = 0. Preserve exactly.
                cb(ctx, idx++,
                   static_cast<lengthType>(static_cast<int>(x)),
                   static_cast<lengthType>(static_cast<int>(y)),
                   0);
            }
        }
    }

private:
    // RingLayout::getRadius(n) = n / (2π) — the radius that spaces n LEDs one
    // unit apart around the circle. Returns float, as MoonLight does (the
    // division is in double then narrowed, matching n / TWO_PI on the double macro).
    static float getRadius(uint8_t n) { return static_cast<float>(n / kTwoPi); }

    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kTwoPi = 2.0 * kPi;

    // The nine ring sizes of the 241-LED disc, inner to outer.
    static constexpr uint8_t kRingSizes[9] = {1, 8, 12, 16, 24, 32, 40, 48, 60};
};

} // namespace mm