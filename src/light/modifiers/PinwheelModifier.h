#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include "light/modifiers/ModifierBase.h"

namespace mm {

// Polar remap: folds the logical box into `petals` angular wedges radiating from
// the box centre — a pinwheel. Each physical light's angle about the centre picks
// a petal (x), and (in 2D+) its radius becomes the along-petal coordinate (y), so
// a 1D effect scrolls outward along every spoke and a 2D effect paints across the
// wheel. `swirl` shears the angle by radius (a spiral), `symmetry` divides the
// full 360° into a repeating sub-arc (a factor of 360), `zTwist` rotates the wheel
// per z-slice, and `reverse` flips petal order.
//
// Prior art: MoonLight's Pinwheel modifier (M_MoonLight.h) — same atan2 petal
// binning, hypot swirl shear, FACTORS[]-of-360 symmetry table, and per-slice
// zTwist. Written fresh against our fold interface: MoonLight's `layerDimension`/
// `effectDimension` (the effect's own dimensionality, which a projectMM modifier
// can't see) is adapted to "is the incoming box 2D+?" — i.e. modifierSize_.y > 1 —
// per the Stage-4 modifier conventions. Pins/wiring plumbing is dropped; this
// emits coordinates only. Float math (atan2/hypot/sqrt) runs on the build path
// (modifyLogical is called at rebuild, not in the hot render loop).
// Author: MoonLight — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Modifiers/M_MoonLight.h
class PinwheelModifier : public ModifierBase {
public:
    const char* tags() const override { return "💫"; }  // MoonLight origin

    uint8_t petals = 60;
    // Signed: negative values reverse the swirl direction. MoonLight's slider
    // range is -127..127. int16_t because the control system's signed type is
    // addInt16 (there is no addInt8); the value stays within ±127.
    int16_t swirl = 30;
    bool reverse = false;
    uint8_t symmetry = 1;
    uint8_t zTwist = 0;

    void onBuildControls() override {
        controls_.addUint8("petals", petals);
        controls_.addInt16("swirl", swirl, -127, 127);
        controls_.addBool("reverse", reverse);
        controls_.addUint8("symmetry", symmetry);
        controls_.addUint8("zTwist", zTwist);
    }

    void modifyLogicalSize(Coord3D& size) override {
        // Stash the ORIGINAL box and its centre BEFORE reshaping — modifyLogical
        // (const, no box arg) reads the pre-resize centre, exactly as MoonLight's
        // modifyPosition reads layer->middle (the original box's middle) while the
        // resized box lives in modifierSize.
        modifierSize_ = size;
        middle_ = {static_cast<lengthType>(size.x / 2),
                   static_cast<lengthType>(size.y / 2),
                   static_cast<lengthType>(size.z / 2)};

        if (petals < 1) petals = 1;  // Ensure at least one petal (guards value %= petals)

        // 2D+ branch: MoonLight gates on (layerDimension>_1D && effectDimension>_1D).
        // A projectMM modifier can't see the effect's dimension, so we adapt to
        // "the incoming box is 2D+" (its y extent > 1) per the Stage-4 conventions.
        if (modifierSize_.y > 1) {
            // Adjust y before x (MoonLight order): furthest radius from centre + 1.
            const int rx = std::max(size.x - middle_.x, static_cast<int>(middle_.x));
            const int ry = std::max(size.y - middle_.y, static_cast<int>(middle_.y));
            size.y = static_cast<lengthType>(std::sqrt(static_cast<float>(rx * rx + ry * ry)) + 1);
            size.x = static_cast<lengthType>(petals);
            size.z = 1;
        } else {
            size.x = 1;
            size.y = static_cast<lengthType>(petals);
            size.z = 1;
        }

        // Petal width in degrees: 360 (or a factor of it, chosen by `symmetry`)
        // divided across the petals. FACTORS[] are the divisors of 360.
        const int FACTORS[23] = {360, 180, 120, 90, 72, 60, 45, 40, 36, 30, 24, 20,
                                 18, 15, 12, 10, 9, 8, 6, 5, 4, 3, 2};
        int factor;
        if (symmetry > 23)
            factor = 2;  // Default to 2 if symmetry is greater than 23
        else if (symmetry > 0)
            factor = FACTORS[symmetry - 1];  // Convert symmetry to a factor of 360
        else
            factor = 360;  // Default to 360 if symmetry is <= 0
        petalWidth_ = factor / static_cast<float>(petals);
    }

    bool modifyLogical(Coord3D& pos) const override {
        // Polar coordinates relative to the (original box) centre.
        const int dx = pos.x - middle_.x;
        const int dy = pos.y - middle_.y;

        // Swirl shears the angle by radius (only computed when swirl != 0).
        const int swirlFactor =
            swirl == 0 ? 0 : static_cast<int>(std::hypot(static_cast<float>(dy), static_cast<float>(dx)) * std::abs(swirl));

        // Angle 0..360 (atan2 returns -180..180, degrees() then +180 shifts it).
        int angle = static_cast<int>(degrees_(std::atan2(static_cast<float>(dy), static_cast<float>(dx)))) + 180;

        if (swirl < 0) angle = 360 - angle;  // Reverse swirl

        int value = angle + swirlFactor + (zTwist * pos.z);
        value = static_cast<int>(value / petalWidth_);
        value %= petals;

        if (reverse) value = petals - value - 1;  // Reverse movement

        pos.x = static_cast<lengthType>(value);
        pos.y = 0;
        // 2D+ branch (see modifyLogicalSize): the radius becomes the along-petal y.
        if (modifierSize_.y > 1) {
            pos.y = static_cast<lengthType>(std::sqrt(static_cast<float>(dx * dx + dy * dy)));
        }
        pos.z = 0;
        return true;
    }

private:
    Coord3D modifierSize_;   // original (pre-resize) box, stashed for the 2D+ test
    Coord3D middle_;         // centre of the original box (MoonLight's layer->middle)
    float petalWidth_ = 6.0f;  // degrees per petal, from symmetry factor / petals

    // Radians → degrees. MoonLight uses Arduino's degrees() macro; reproduced here
    // as a plain helper so the header carries no Arduino dependency.
    static float degrees_(float rad) { return rad * (180.0f / 3.14159265358979323846f); }
};

} // namespace mm