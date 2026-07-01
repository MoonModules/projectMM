#pragma once

#include "light/modifiers/ModifierBase.h"
#include "core/math8.h"           // sin8, cos8 — integer trig

#include <cstdint>

namespace mm {

// Rotates the 2D image around its centre, turning continuously over time. The one
// DYNAMIC modifier in the set: it overrides modifyLive(), so the Layer re-applies it
// every frame (a smooth turn, not a stepped LUT rebuild). A static-only chain pays
// nothing — the per-frame pass runs only because this modifier reports hasModifyLive().
//
// modifyLive is the **backward-mapping** seam: for each DESTINATION logical cell it
// computes the SOURCE cell to gather from, so no destination is ever left unfilled (the
// textbook reason image warping samples backward — Forward-and-Backward Mapping, the
// classic CV result). The source is the inverse rotation R(-θ) of the destination.
//
// This modifier is also the codebase's **transform-matrix reference**. Rotation is the
// canonical affine transform, so unlike the % / mask folds (Multiply, Checkerboard,
// Region — non-affine, expressed as direct coordinate folds), it's written as an explicit
// 2×2 rotation matrix R(-θ) = [[c, s], [-s, c]] applied to the centred coordinate. The
// matrix entries are integer fixed-point (cos8/sin8 → 0..255 centred at 128, so c=cos8-128
// is the signed unit component scaled by 128; the >>7 divides back out). A future affine
// "Transform" modifier (translate+scale+rotate+shear in one) would compose its matrix the
// same way and apply it here — the fold interface hosts a matrix-backed modifier with no
// change. Non-affine modifiers can't use a matrix (a mask is a predicate, a tile is modulo
// — neither is a linear map), which is why only this one is matrix-shaped.
//
// Prior art: MoonLight M_MoonLight.h Rotate (modifyXYZ per-frame transform). Same per-frame
// coordinate remap; we name the hook modifyLive and carry an explicit matrix.
// Author: WildCats08 / @Brandon502 (MoonLight) — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Modifiers/M_MoonLight.h
class RotateModifier : public ModifierBase {
public:
    Dim dimensions() const override { return Dim::D2; }   // 2D rotation (advisory chip)
    bool hasModifyLive() const override { return true; }  // animates every frame

    uint8_t speed = 1;   // rotation speed, 1..255 (turns faster as it rises)

    void onBuildControls() override {
        controls_.addUint8("speed", speed, 1, 255);
    }

    // `speed` only changes how fast the angle advances; rotation is applied live in
    // modifyLive, not baked into the mapping, so a speed edit needs no rebuild.
    bool controlChangeTriggersBuildState(const char* /*controlName*/) const override { return false; }

    // Per-frame backward map: a destination logical cell `pos` is replaced by the
    // SOURCE cell it samples — the inverse rotation R(-θ) about the box centre.
    // `logical` is the box. Out-of-box sources stay out-of-box, so the Layer's live
    // pass leaves that destination dark (nothing to gather) — a clean edge, no wrap.
    void modifyLive(Coord3D& pos, const Coord3D& logical) const override {
        // Centre in half-units (×2) so an even-width box rotates about its true centre.
        const int32_t cx2 = logical.x - 1;                       // 2·centreX
        const int32_t cy2 = logical.y - 1;                       // 2·centreY
        const int32_t dx2 = 2 * static_cast<int32_t>(pos.x) - cx2;   // 2·(x − centre)
        const int32_t dy2 = 2 * static_cast<int32_t>(pos.y) - cy2;

        // R(-θ) = [[ c,  s],
        //          [-s,  c]]   with c = cos θ, s = sin θ in signed fixed-point /128.
        // source = R(-θ) · dest. cos8/sin8 are 0..255 centred at 128.
        const int32_t c = static_cast<int32_t>(cos8(angle_)) - 128;
        const int32_t s = static_cast<int32_t>(sin8(angle_)) - 128;
        const int32_t sx2 = ( dx2 * c + dy2 * s) >> 7;           // row 0 of the matrix · dest
        const int32_t sy2 = (-dx2 * s + dy2 * c) >> 7;           // row 1 of the matrix · dest

        // Undo the ×2 and centre shift, rounding to nearest.
        pos.x = static_cast<lengthType>((sx2 + cx2 + 1) >> 1);
        pos.y = static_cast<lengthType>((sy2 + cy2 + 1) >> 1);
        // z passes through (2D rotation).
    }

    // Dynamic tick: advance the angle on the timer. No rebuild — modifyLive applies
    // the new angle on the next frame. The angle is uint8 turn units (256 = a turn);
    // dt·speed accumulates so a sub-ms frame isn't lost (the integer-accumulator idiom
    // the effects use). Layer::loop() invokes this per enabled modifier child.
    void loop() override {
        const uint32_t now = platform::millis();
        if (lastElapsed_ == 0) lastElapsed_ = now;
        const uint32_t dt = now - lastElapsed_;
        lastElapsed_ = now;
        phaseNum_ += static_cast<uint64_t>(dt) * speed;
        angle_ = static_cast<uint8_t>(phaseNum_ >> 6);   // one turn unit per 64 accumulator units
    }

private:
    uint8_t  angle_ = 0;        // current rotation angle, uint8 turn units (256 = full turn)
    uint64_t phaseNum_ = 0;     // dt·speed accumulator
    uint32_t lastElapsed_ = 0;
};

} // namespace mm
