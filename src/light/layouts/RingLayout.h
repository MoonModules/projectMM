#pragma once

#include <cmath>    // sinf, cosf, fmodf
#include <cstdint>
#include <numbers>  // std::numbers::pi_v — portable pi (M_PI is a non-standard <cmath> extension)
#include "light/layouts/Layouts.h"
#include "light/light_types.h"  // lengthType, nrOfLightsType

namespace mm {

// A ring of lights: nrOfLEDs placed evenly around a circle, optionally a partial
// arc (rotation < 360, clockwise or counter-clockwise from angleFirst) and scaled
// out from the centre. Every light sits at an integer (x, y, 0); the circle centre
// is placed at ~1.1× the ring radius on both axes so the whole ring lands in the
// positive quadrant.
//
// Prior art: MoonLight's RingLayout (MoonModules/MoonLight, src light layout nodes).
// Geometry (getRadius = n / 2π, the PI + 2πi/n + 2π·angleFirst/360 placement angle,
// the 1.1× centre offset, the clockwise/counter-clockwise partial-arc filter, and
// the integer truncation of every coordinate) is reproduced exactly. MoonLight's
// pin/wiring plumbing (doNextPin/nextPin) is dropped — a projectMM layout emits
// coordinates only; the driver owns pins.
//
// Float trig runs on the cold build path (forEachCoord / lightCount, called from a
// rebuild), never the hot render loop, so it's allowed here.
class RingLayout : public LayoutBase {
public:
    // MoonLight defaults and ranges, preserved verbatim.
    uint8_t  nrOfLEDs   = 24;    // 1..255
    uint16_t angleFirst = 0;     // 0..359 — angle of the first LED (0 = top)
    uint16_t rotation   = 360;   // 0..360 — arc span; <360 emits a partial ring
    bool     clockwise  = true;
    uint8_t  scale      = 1;     // 1..10 — spacing multiplier out from the centre

    void onBuildControls() override {
        controls_.addUint8("nrOfLEDs",    nrOfLEDs,   1, 255);
        controls_.addUint16("angleFirst", angleFirst, 0, 359);
        controls_.addUint16("rotation",   rotation,   0, 360);
        controls_.addBool("clockwise",    clockwise);
        controls_.addUint8("scale",       scale,      1, 10);
    }

    nrOfLightsType lightCount() const override {
        // Reuse the exact inclusion predicate as forEachCoord so count and emit
        // never disagree (a partial arc emits fewer than nrOfLEDs lights).
        nrOfLightsType n = 0;
        walk([](void*, nrOfLightsType, lengthType, lengthType, lengthType) {}, nullptr, &n);
        return n;
    }

    void forEachCoord(CoordCallback cb, void* ctx) const override {
        walk(cb, ctx, nullptr);
    }

private:
    // MoonLight: getRadius(n) = n / TWO_PI.
    static float getRadius(uint8_t n) {
        return static_cast<float>(n) / (2.0f * std::numbers::pi_v<float>);
    }

    // Single source of truth for the ring geometry: walks the nrOfLEDs candidate
    // positions, applies MoonLight's partial-arc filter, and for each INCLUDED LED
    // invokes cb with a sequential index and/or tallies it into *count.
    void walk(CoordCallback cb, void* ctx, nrOfLightsType* count) const {
        const float PI_F     = std::numbers::pi_v<float>;
        const float TWO_PI_F = 2.0f * PI_F;

        // nrOfLEDs is a uint8_t control (1..255) but can be 0 if set directly; the
        // loop below never runs then and every division by nrOfLEDs is guarded by
        // it, so a degenerate ring emits zero lights without dividing by zero.
        if (nrOfLEDs == 0) {
            if (count) *count = 0;
            return;
        }

        // RECONSTRUCTED: MoonLight computes ringCenter in onUpdate() (on the
        // nrOfLEDs control change), storing it as an integer Coord3D — the float
        // 1.1 * getRadius(nrOfLEDs) is truncated to int on assignment. projectMM
        // rebuilds fresh each build, so ringCenter is derived inline here from
        // nrOfLEDs. Behaviourally identical; the int truncation is preserved so
        // the emitted coordinates match MoonLight exactly. z is always 0.
        const lengthType ringCenterX = static_cast<lengthType>(1.1f * getRadius(nrOfLEDs));
        const lengthType ringCenterY = static_cast<lengthType>(1.1f * getRadius(nrOfLEDs));

        const float radius = getRadius(nrOfLEDs);

        nrOfLightsType idx = 0;
        for (int i = 0; i < nrOfLEDs; i++) {
            float x = scale * ringCenterX;
            float y = scale * ringCenterY;

            // Angle of this LED (for the partial-arc inclusion test).
            const float ledAngle =
                fmodf(angleFirst + (static_cast<float>(i) / nrOfLEDs) * 360.0f, 360.0f);

            // Placement angle for the actual position.
            const float angleRad =
                PI_F + (TWO_PI_F * i) / nrOfLEDs + TWO_PI_F * angleFirst / 360.0f;

            if (nrOfLEDs != 1) {
                x -= scale * sinf(angleRad) * radius;
                y += scale * cosf(angleRad) * radius;
            }

            // Partial-circle inclusion test (MoonLight, verbatim logic). rotation
            // is a uint16_t, so the full-circle test is integer, not float.
            bool includeLED = false;
            if (rotation < 1 || rotation >= 360) {
                includeLED = true;  // full circle
            } else {
                float endAngle;
                if (clockwise) {
                    endAngle = fmodf(angleFirst + rotation, 360.0f);
                } else {
                    endAngle = fmodf(angleFirst - rotation + 360.0f, 360.0f);
                }

                if (clockwise) {
                    if (endAngle >= angleFirst) {
                        // No wrap: e.g. angleFirst=180, rotation=90, endAngle=270
                        includeLED = (ledAngle >= angleFirst && ledAngle <= endAngle);
                    } else {
                        // Wraps around 0: e.g. angleFirst=270, rotation=180, endAngle=90
                        includeLED = (ledAngle >= angleFirst || ledAngle <= endAngle);
                    }
                } else {
                    if (endAngle <= angleFirst) {
                        // No wrap: e.g. angleFirst=180, rotation=90, endAngle=90
                        includeLED = (ledAngle <= angleFirst && ledAngle >= endAngle);
                    } else {
                        // Wraps around 0: e.g. angleFirst=90, rotation=180, endAngle=270
                        includeLED = (ledAngle <= angleFirst || ledAngle >= endAngle);
                    }
                }
            }

            if (includeLED) {
                if (cb) {
                    cb(ctx, idx,
                       static_cast<lengthType>(static_cast<int>(x)),
                       static_cast<lengthType>(static_cast<int>(y)),
                       0);  // scale * ringCenter.z, and ringCenter.z is always 0
                }
                idx++;
            }
        }
        if (count) *count = idx;
    }
};

} // namespace mm
