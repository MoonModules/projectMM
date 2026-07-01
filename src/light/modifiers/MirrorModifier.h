#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

// Folds the far half of the logical box back onto the near half per axis, so the
// image is mirrored across the box centre — the top-left quadrant is reflected
// into the other three (2D), or the near octant into all eight (3D), depending on
// which axes are enabled. Halving the box (rounding up) then folding each far-half
// coordinate back gives a N:1 mapping where the mirrored physical lights share a
// logical source, so the fan-out is free under the mapping fold.
//
// Per axis the logical extent becomes ceil(size/2) = (size+1)/2 (an odd extent
// keeps its centre column unpaired), and a physical coordinate at or past that
// half-extent reflects to `half*2 - 1 - pos` (the far edge maps to logical 0, the
// column just past centre maps to the last logical column).
//
// Prior art: MoonLight's Mirror modifier (M_MoonLight.h) — same (size+1)/2 halving
// and `modifierSize*2 - 1 - position` reflection, per-axis mirror bools. Written
// fresh against our fold interface: modifySize()/modifyPosition() become
// modifyLogicalSize()/modifyLogical(), and the box is stashed for the const fold
// via the `modifierSize` pattern the base class documents.
class MirrorModifier : public ModifierBase {
public:
    const char* tags() const override { return "💫"; }  // MoonLight origin

    // Mirror across the box centre on this axis. Enabling an axis the layout
    // doesn't use (e.g. Z on a 2D grid, size.z == 1) is a no-op: (1+1)/2 == 1
    // leaves the extent unchanged and no coordinate is ever >= the half-extent.
    bool mirrorX = true;
    bool mirrorY = true;
    bool mirrorZ = true;

    void onBuildControls() override {
        controls_.addBool("mirrorX", mirrorX);
        controls_.addBool("mirrorY", mirrorY);
        controls_.addBool("mirrorZ", mirrorZ);
    }

    void modifyLogicalSize(Coord3D& size) override {
        // Halve each mirrored axis, rounding up so an odd extent keeps its centre
        // column. Stash the halved box for the const fold (the MoonLight
        // `modifierSize` pattern) — modifyLogical reads its own stage's box here.
        if (mirrorX) size.x = static_cast<lengthType>((size.x + 1) / 2);
        if (mirrorY) size.y = static_cast<lengthType>((size.y + 1) / 2);
        if (mirrorZ) size.z = static_cast<lengthType>((size.z + 1) / 2);
        half_ = size;
    }

    bool modifyLogical(Coord3D& pos) const override {
        // A coordinate in the far half of the box reflects back onto the near half:
        // half*2 - 1 - pos. Coordinates already in the near half pass through.
        if (mirrorX && pos.x >= half_.x) pos.x = static_cast<lengthType>(half_.x * 2 - 1 - pos.x);
        if (mirrorY && pos.y >= half_.y) pos.y = static_cast<lengthType>(half_.y * 2 - 1 - pos.y);
        if (mirrorZ && pos.z >= half_.z) pos.z = static_cast<lengthType>(half_.z * 2 - 1 - pos.z);
        return true;
    }

private:
    Coord3D half_;  // halved logical box, stashed in modifyLogicalSize for the fold
};

} // namespace mm