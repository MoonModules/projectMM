#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

// Swaps a pair of axes of the logical box and every coordinate folded through it,
// then optionally inverts (flips) each axis. Enable XY to swap the x and y axes
// (a matrix transpose — rows become columns), XZ or YZ likewise; the swaps apply
// in that order, so enabling several composes them. The inverse-X/Y/Z bools flip
// a coordinate within the (already-transposed) box: x → size.x - 1 - x, so the
// axis reads back-to-front. A static, build-time remap — no per-frame work.
//
// Prior art: MoonLight's Transpose modifier (M_MoonLight.h) — same pairwise axis
// swap on both the box and the coordinate, plus per-axis inverse. The inverse
// reads the box AFTER the swaps (MoonLight's modifyPosition reads the transposed
// `layer->size`), reproduced here by inverting against the stashed box. Written
// fresh against our fold interface.
class TransposeModifier : public ModifierBase {
public:
    const char* tags() const override { return "💫"; }  // MoonLight origin

    // Pairwise axis swaps. XY on by default (the common 2D transpose).
    bool transposeXY = true;
    bool transposeXZ = false;
    bool transposeYZ = false;
    // Flip each axis within the transposed box (back-to-front).
    bool inverseX = false;
    bool inverseY = false;
    bool inverseZ = false;

    void onBuildControls() override {
        controls_.addBool("XY", transposeXY);
        controls_.addBool("XZ", transposeXZ);
        controls_.addBool("YZ", transposeYZ);
        controls_.addBool("inverse X", inverseX);
        controls_.addBool("inverse Y", inverseY);
        controls_.addBool("inverse Z", inverseZ);
    }

    void modifyLogicalSize(Coord3D& size) override {
        // Swap the box axes in the same order the coordinates are swapped, so the
        // logical box matches the folded coordinates. Each swap is unconditional
        // on the flag, mirroring MoonLight exactly (enabling XY then XZ composes).
        if (transposeXY) { lengthType t = size.x; size.x = size.y; size.y = t; }
        if (transposeXZ) { lengthType t = size.x; size.x = size.z; size.z = t; }
        if (transposeYZ) { lengthType t = size.y; size.y = size.z; size.z = t; }
        // Stash the TRANSPOSED box: modifyLogical inverts against it (MoonLight's
        // modifyPosition reads the already-swapped layer->size for the inverse).
        modifierSize_ = size;
    }

    bool modifyLogical(Coord3D& pos) const override {
        if (transposeXY) { lengthType t = pos.x; pos.x = pos.y; pos.y = t; }
        if (transposeXZ) { lengthType t = pos.x; pos.x = pos.z; pos.z = t; }
        if (transposeYZ) { lengthType t = pos.y; pos.y = pos.z; pos.z = t; }

        if (inverseX) pos.x = static_cast<lengthType>(modifierSize_.x - pos.x - 1);
        if (inverseY) pos.y = static_cast<lengthType>(modifierSize_.y - pos.y - 1);
        if (inverseZ) pos.z = static_cast<lengthType>(modifierSize_.z - pos.z - 1);
        return true;  // Transpose never rejects a coordinate.
    }

private:
    Coord3D modifierSize_;  // transposed box, stashed in modifyLogicalSize for the inverse fold
};

} // namespace mm