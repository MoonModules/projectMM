#pragma once

#include <cmath>    // sinf, cosf, fmodf
#include <cstdint>
#include <numbers>  // std::numbers::pi_v — portable pi
#include "light/layouts/Layouts.h"
#include "light/light_types.h"  // lengthType, nrOfLightsType

namespace mm {

// A stylised pair of car headlights: four concentric-ring "lamps" (inner/outer
// left, inner/outer right) plus two side strips joined by 90° arcs, all scaled
// by a single `scale` control. A 2D layout emitting each LED's (x, y, 0) in
// physical wiring order.
//
// Prior art: MoonLight's CarLightsLayout (Node "Car Lights", tags 🚥;
// MoonModules/MoonLight, src light layout nodes). MoonLight builds this by
// instantiating a RingLayout object and calling its onLayout() repeatedly with
// different ringCenter / nrOfLEDs / angle settings; every ring and both strips'
// coordinates are reproduced here EXACTLY, in the same wiring order. tags 💫
// marks the MoonLight lineage.
//
// RECONSTRUCTED: projectMM's RingLayout is a standalone module — it has no
// onLayout()/addLight() and derives its ring centre INTERNALLY from nrOfLEDs, so
// it cannot be driven the way MoonLight drives its RingLayout (which takes an
// EXTERNALLY set ringCenter per headlight). The ring-emitting trig is therefore
// inlined below in emitRing(), reproducing MoonLight RingLayout::onLayout()
// verbatim (getRadius = n/2π; the PI + 2π·i/n + 2π·angleFirst/360 placement
// angle; the clockwise / counter-clockwise partial-arc inclusion filter; the
// (int)x / (int)y truncation). The external-ringCenter form is the one CarLights
// needs, which is why we don't delegate to RingLayout.h.
//
// Float trig runs on the cold build path (forEachCoord / lightCount, called from
// a rebuild), never the hot render loop, so it's allowed here. MoonLight's
// pin/wiring plumbing (nextPin / doNextPin) is dropped — a projectMM layout
// emits coordinates only; the driver owns pins.
class CarLightsLayout : public LayoutBase {
public:
    // Verbatim MoonLight default and range (the commented-out nrOfSpokes /
    // ledsPerSpoke controls in the source are inactive there too, so dropped).
    uint8_t scale = 2;  // 1..10 — spacing multiplier out from each centre

    void onBuildControls() override {
        controls_.addUint8("scale", scale, 1, 10);
    }

    const char* tags() const override { return "💫"; }

    nrOfLightsType lightCount() const override {
        // Count via the exact same emit walk so count and emit never disagree
        // (partial arcs emit fewer than nrOfLEDs lights).
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

    // Emission context threaded through the fixed build order: holds the caller's
    // callback (or a count target) and the running physical index so each ring /
    // strip appends after the previous one, exactly like MoonLight's serial
    // addLight() calls.
    struct Emit {
        CoordCallback cb;
        void* ctx;
        nrOfLightsType idx;
        // Append one light at (x, y, 0). Truncation to int matches MoonLight's
        // addLight({(int)x, (int)y, ...}).
        void add(float x, float y) {
            if (cb) cb(ctx, idx,
                       static_cast<lengthType>(static_cast<int>(x)),
                       static_cast<lengthType>(static_cast<int>(y)),
                       0);
            idx++;
        }
        void add(lengthType x, lengthType y) {
            if (cb) cb(ctx, idx, x, y, 0);
            idx++;
        }
    };

    // RECONSTRUCTED (see class comment): MoonLight RingLayout::onLayout(), inlined
    // with an externally supplied ringCenter (cx, cy) — z is always 0 here. Emits
    // the included LEDs of one ring into `e` in ring order. `scale` is the layout
    // control (MoonLight sets ringLayout.scale = scale before each ring).
    void emitRing(Emit& e, lengthType cx, lengthType cy, uint8_t nrOfLEDs,
                  uint16_t angleFirst, uint16_t rotation, bool clockwise) const {
        if (nrOfLEDs == 0) return;

        const float PI_F     = std::numbers::pi_v<float>;
        const float TWO_PI_F = 2.0f * PI_F;
        const float radius   = getRadius(nrOfLEDs);

        for (int i = 0; i < nrOfLEDs; i++) {
            float x = scale * static_cast<float>(cx);
            float y = scale * static_cast<float>(cy);

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

            // Partial-circle inclusion test (MoonLight, verbatim logic).
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
                        // No wrap: angleFirst=180, rotation=90, endAngle=270
                        includeLED = (ledAngle >= angleFirst && ledAngle <= endAngle);
                    } else {
                        // Wraps around 0: angleFirst=270, rotation=180, endAngle=90
                        includeLED = (ledAngle >= angleFirst || ledAngle <= endAngle);
                    }
                } else {
                    if (endAngle <= angleFirst) {
                        // No wrap: angleFirst=180, rotation=90, endAngle=90
                        includeLED = (ledAngle <= angleFirst && ledAngle >= endAngle);
                    } else {
                        // Wraps around 0: angleFirst=90, rotation=180, endAngle=270
                        includeLED = (ledAngle <= angleFirst || ledAngle >= endAngle);
                    }
                }
            }

            if (includeLED) e.add(x, y);
        }
    }

    // Single source of truth for the whole car-lights geometry: emits every ring
    // and both strips in MoonLight's exact serial order. cb/ctx receive the
    // lights; when count is non-null it also tallies the total.
    void walk(CoordCallback cb, void* ctx, nrOfLightsType* count) const {
        Emit e{cb, ctx, 0};

        const lengthType leftMargin = 9;  // MoonLight: uint8_t leftMargin = 9;

        // --- Headlights: angleFirst=90, rotation=360, clockwise (defaults) ---
        constexpr uint16_t kFirst = 90;
        constexpr uint16_t kFull  = 360;
        constexpr bool     kCW    = true;

        // inner light left, centre {leftMargin + 11, 8}
        emitRing(e, leftMargin + 11, 8,  1, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 11, 8,  8, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 11, 8, 12, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 11, 8, 16, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 11, 8, 24, kFirst, kFull, kCW);

        // outer light left, centre {leftMargin, 6}
        emitRing(e, leftMargin, 6,  1, kFirst, kFull, kCW);
        emitRing(e, leftMargin, 6,  8, kFirst, kFull, kCW);
        emitRing(e, leftMargin, 6, 12, kFirst, kFull, kCW);
        emitRing(e, leftMargin, 6, 16, kFirst, kFull, kCW);
        emitRing(e, leftMargin, 6, 24, kFirst, kFull, kCW);
        emitRing(e, leftMargin, 6, 32, kFirst, kFull, kCW);

        // (MoonLight nextPin() here — dropped)

        // inner light right, centre {leftMargin + 25, 8}
        emitRing(e, leftMargin + 25, 8,  1, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 25, 8,  8, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 25, 8, 12, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 25, 8, 16, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 25, 8, 24, kFirst, kFull, kCW);

        // outer light right, centre {leftMargin + 36, 6}
        emitRing(e, leftMargin + 36, 6,  1, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 36, 6,  8, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 36, 6, 12, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 36, 6, 16, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 36, 6, 24, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 36, 6, 32, kFirst, kFull, kCW);

        // (MoonLight nextPin() here — dropped)

        // --- left strip ---
        // for (x = (leftMargin+16)*scale; x >= leftMargin*scale; x--) addLight({x, 15*scale})
        for (int x = (leftMargin + 16) * scale; x >= leftMargin * scale; x--) {
            e.add(static_cast<lengthType>(x), static_cast<lengthType>(15 * scale));
        }
        // 52-LED arc, centre {leftMargin, 6}, angleFirst=180, rotation=90, clockwise
        emitRing(e, leftMargin, 6, 52, 180, 90, true);
        // for (y = 5; y >= 1; y--) addLight({0, y*scale})
        for (int y = 5; y >= 1; y--) {
            e.add(static_cast<lengthType>(0), static_cast<lengthType>(y * scale));
        }

        // (MoonLight nextPin() here — dropped)

        // --- right strip ---
        // for (x = (leftMargin+19)*scale; x <= (leftMargin+35)*scale; x++) addLight({x, 15*scale})
        for (int x = (leftMargin + 19) * scale; x <= (leftMargin + 35) * scale; x++) {
            e.add(static_cast<lengthType>(x), static_cast<lengthType>(15 * scale));
        }
        // 52-LED arc, centre {leftMargin + 36, 6}, angleFirst=180, rotation=90, counter-clockwise
        emitRing(e, leftMargin + 36, 6, 52, 180, 90, false);
        // for (y = 5; y >= 1; y--) addLight({(leftMargin+44)*scale, y*scale})
        for (int y = 5; y >= 1; y--) {
            e.add(static_cast<lengthType>((leftMargin + 44) * scale),
                  static_cast<lengthType>(y * scale));
        }

        // (MoonLight nextPin() here — dropped)

        if (count) *count = e.idx;
    }
};

} // namespace mm
