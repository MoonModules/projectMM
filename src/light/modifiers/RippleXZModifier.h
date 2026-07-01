#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

// Collapses one axis of the logical box to a single plane, so a lower-dimensional
// effect drawn on the collapsed axis maps identically onto every slice of the
// physical box. With `shrink` on, `towardsX` flattens the box's X extent to 1 and
// folds every physical X onto x=0 (a 1D effect running along X is painted once and
// broadcast across all X); `towardsZ` does the same for the Z extent (each XY plane
// shared across depth). It is the static "collapse an axis" fold: the logical box
// loses the axis, and every coordinate on that axis folds to 0.
//
// Prior art: MoonLight's RippleXZ modifier (M_MoonLight.h) — same shrink/towardsX/
// towardsZ axis collapse (`modifySize` sets the axis to 1, `modifyPosition` sets
// the coordinate to 0). MoonLight additionally runs a per-frame buffer shift in
// loop() that propagates a ripple wave along the collapsed axis by reading and
// writing the layer's RGB pixels (setRGB/getRGB). That is a render-buffer effect,
// not a coordinate transform, and has no place in the static build-time fold this
// base class expresses — projectMM modifiers emit coordinates only, so the wave
// propagation is dropped here (the geometry — the axis collapse — is preserved
// exactly). Defaults match MoonLight: shrink=true, towardsX=true, towardsZ=false.
// Author: @Troy (WLEDMM Art-Net) — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Modifiers/M_MoonLight.h
class RippleXZModifier : public ModifierBase {
public:
    const char* tags() const override { return "💫"; }  // MoonLight origin

    // Collapse the box (shrink) and which axes to collapse. towardsX flattens X,
    // towardsZ flattens Z. Y is never collapsed.
    bool shrink = true;
    bool towardsX = true;
    bool towardsZ = false;

    void onBuildControls() override {
        controls_.addBool("shrink", shrink);
        controls_.addBool("towardsX", towardsX);
        controls_.addBool("towardsZ", towardsZ);
    }

    void modifyLogicalSize(Coord3D& size) override {
        // RECONSTRUCTED: MoonLight's modifySize() sets layer->size.x/z = 1 in place;
        // here the same collapse is applied to the running logical box `size`.
        if (shrink) {
            if (towardsX) size.x = 1;
            if (towardsZ) size.z = 1;
        }
    }

    bool modifyLogical(Coord3D& pos) const override {
        // RECONSTRUCTED: MoonLight's modifyPosition() folds the collapsed axis to 0,
        // so every physical coordinate on that axis lands on the single logical slice.
        if (shrink) {
            if (towardsX) pos.x = 0;
            if (towardsZ) pos.z = 0;
        }
        return true;  // never rejects — a collapse maps every coord onto a slice
    }
};

} // namespace mm