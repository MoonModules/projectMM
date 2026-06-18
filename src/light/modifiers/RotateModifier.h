#pragma once

#include "light/layers/Layer.h"   // ModifierBase + Layer (we call layer->onBuildState() on a step)
#include "core/color.h"           // sin8, cos8 — integer trig
#include "platform/platform.h"

#include <cstdint>

namespace mm {

// Rotates the 2D image around its centre, turning continuously over time. A DYNAMIC
// modifier (like RandomMapModifier): the rotation is a coordinate remap baked into the
// Layer's LUT, and the angle advances on a `speed` timer. The LUT is rebuilt only when
// the angle crosses to a new integer step (256 steps per turn), NOT every frame — so a
// slow rotation rebuilds rarely and a fast one more often, always bounded, never a
// per-frame alloc. Integer-only: the inverse rotation uses the sin8/cos8 LUT, nearest-
// neighbour sampling (no float, no bilinear).
//
// Each destination light at (dx,dy) from centre samples the SOURCE at the inverse
// rotation: sx = dx·cosθ + dy·sinθ, sy = -dx·sinθ + dy·cosθ. cos8/sin8 return 0..255
// centred at 128, so (val-128)/128 is the unit component; the >>7 divides by 128. A
// source outside the grid is dropped (outCount=0 → that light goes dark at this angle).
//
// Prior art: MoonLight M_MoonLight.h Rotate/PinWheel (modifyXYZ per-light transform).
// Our version carries the transform in the LUT instead, reusing the dynamic-modifier
// loop() hook + the existing rebuild path — no Layer::render coupling, no dynamic_cast.
class RotateModifier : public ModifierBase {
public:
    Dim dimensions() const override { return Dim::D2; }   // 2D rotation (advisory chip)

    uint8_t speed = 1;   // rotation speed, 1..255 (turns faster as it rises); affects how
                         // many angle-steps pass per second (and so how often the LUT rebuilds)

    void onBuildControls() override {
        controls_.addUint8("speed", speed, 1, 255);
    }

    nrOfLightsType maxMultiplier() const override { return 1; }   // 1:1 or 1:0, never fans out

    // Identity dimensions — rotation doesn't resize the box, it remaps within it.
    void logicalDimensions(lengthType physW, lengthType physH, lengthType physD,
                           lengthType& logW, lengthType& logH, lengthType& logD) const override {
        logW = physW;
        logH = physH;
        logD = physD;
    }

    // Map a destination light to its rotated SOURCE light. Inverse rotation by the
    // current angle (angle_), integer sin8/cos8. 2D: z passes through unchanged.
    void mapToPhysical(lengthType lx, lengthType ly, lengthType lz,
                       lengthType physW, lengthType physH, lengthType /*physD*/,
                       nrOfLightsType* outPhysicals, nrOfLightsType& outCount,
                       nrOfLightsType maxOut) const override {
        outCount = 0;
        if (maxOut == 0) return;

        // Centre of the grid (in half-units to handle even widths: use 2× coords).
        const int32_t cx2 = physW - 1;   // 2*centreX
        const int32_t cy2 = physH - 1;   // 2*centreY
        const int32_t dx2 = 2 * static_cast<int32_t>(lx) - cx2;   // 2*(x-centre)
        const int32_t dy2 = 2 * static_cast<int32_t>(ly) - cy2;

        // Signed cos/sin in [-128,127]. Inverse rotation: source = R(-θ)·dest.
        const int32_t c = static_cast<int32_t>(cos8(angle_)) - 128;
        const int32_t s = static_cast<int32_t>(sin8(angle_)) - 128;
        const int32_t sx2 = (dx2 * c + dy2 * s) >> 7;   // ÷128, back to 2×-unit space
        const int32_t sy2 = (-dx2 * s + dy2 * c) >> 7;

        // Back to grid coordinates (undo the 2× and the centre shift), rounding to nearest.
        const int32_t sx = (sx2 + cx2 + 1) >> 1;
        const int32_t sy = (sy2 + cy2 + 1) >> 1;

        if (sx < 0 || sx >= physW || sy < 0 || sy >= physH) return;   // off-grid → dropped
        outPhysicals[0] =
            static_cast<nrOfLightsType>(lz) * static_cast<nrOfLightsType>(physW) * static_cast<nrOfLightsType>(physH) +
            static_cast<nrOfLightsType>(sy) * static_cast<nrOfLightsType>(physW) +
            static_cast<nrOfLightsType>(sx);
        outCount = 1;
    }

    // Dynamic tick: advance the angle on the timer; when it crosses to a new integer
    // step (1/256 of a turn), rebuild the LUT so the image rotates. Stepped, not
    // per-frame — the rebuild only fires on a step change, bounded by speed.
    // Overrides MoonModule::loop(); Layer::loop() invokes it per enabled modifier child.
    void loop() override {
        Layer* lyr = static_cast<Layer*>(parent());
        if (!lyr) return;
        const uint32_t now = lyr->elapsed();
        if (lastElapsed_ == 0) lastElapsed_ = now;
        const uint32_t dt = now - lastElapsed_;
        lastElapsed_ = now;
        // Accumulate dt*speed; the step is phaseNum_/64 (mod 256), so one step takes
        // 64/speed ms and a full turn (256 steps = 16384 units) takes 16384/speed ms.
        // Read the high bits as the angle step — the same integer accumulator idiom the
        // effects use so a sub-ms dt isn't lost.
        phaseNum_ += static_cast<uint64_t>(dt) * speed;
        const uint8_t step = static_cast<uint8_t>(phaseNum_ >> 6);   // one step per 64 units
        if (step != angle_) {
            angle_ = step;
           // Rebuild the LUT at the new angle (re-runs mapToPhysical). Like
            // RandomMapModifier this is a step-gated rebuild from loop(), an accepted
            // bounded cost (runs after the effect pass, not per-tick) — but the bound
            // here is the angle-step rate, not a bpm cap: one step per 64 accumulator
            // units → speed/64 rebuilds/sec, i.e. up to ~4/sec at speed 255 (vs
            // RandomMap's ≤1/sec at bpm 60). Each rebuild does the alloc/free
            // rebuildLUT() does; that ~4/sec ceiling on the render task is the cost of
            // smooth rotation. Lower `speed` for fewer rebuilds.
            lyr->onBuildState();
        }
    }

private:
    uint8_t  angle_ = 0;        // current rotation angle, uint8 turn units (256 = full turn)
    uint64_t phaseNum_ = 0;     // dt*speed accumulator
    uint32_t lastElapsed_ = 0;
};

} // namespace mm
