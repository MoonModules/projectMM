#pragma once

#include <cstdlib> // std::abs
#include <algorithm> // std::max

#include "light/modifiers/ModifierBase.h"

namespace mm {

// Remaps a 1D effect onto the CHEBYSHEV-distance field of a 2D box: each physical
// light folds to its block distance from the box centre — max(|dx|,|dy|) — so the
// effect draws as concentric SQUARE rings expanding from the middle. It is the
// square-ring sibling of the Circle modifier (which uses the Euclidean sqrt and
// draws round rings); Block swaps that for the Chebyshev max, which is why the
// rings are axis-aligned squares. Z is not part of the distance (this is a 2D
// remap, exactly as MoonLight defines it).
//
// The logical box the fold produces is one row wide and (max block distance + 1)
// tall: a 1D effect painted along y lights the rings from centre outward.
//
// Prior art: MoonLight's BlockModifier (M_MoonLight.h) — same centre formula
// ((n+1)/2 - 1, a floor-biased middle), same dx/dy abs deltas, same distance =
// max(dx, dy), same {0, distance, 0} output, and the same box transform (fold the
// box through the position map, then grow each axis by one). Written fresh against
// our fold interface: modifySize() -> modifyLogicalSize (stashing the incoming box),
// modifyPosition() -> the const modifyLogical fold that reads the stash.
// Author: MoonLight — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Modifiers/M_MoonLight.h
class BlockModifier : public ModifierBase {
public:
    const char* tags() const override { return "💫"; }  // MoonLight origin

    // 1D -> 2D remap: it reads x/y, writes into y. The z axis is untouched, so the
    // advisory chip is 2D (MoonLight's dim() == _2D).
    Dim dimensions() const override { return Dim::D2; }

    void modifyLogicalSize(Coord3D& size) override {
        // Stash the incoming (physical) box so the const per-light fold can read the
        // centre from it. This is MoonLight's `modifierSize = layer->size`.
        modifierSize_ = size;

        // RECONSTRUCTED: MoonLight calls `modifyPosition(layer->size)` here to fold
        // the box itself into block-distance space, then does `size.x++/y++/z++`.
        // We inline that same computation over `size` (our modifyLogical is const and
        // reads the stash we are still writing, so we can't call it here). The math is
        // identical: the box becomes {0, max(|dx|,|dy|), 0}, then each axis grows by 1.
        const int centerX = (modifierSize_.x + 1) / 2 - 1;
        const int centerY = (modifierSize_.y + 1) / 2 - 1;
        const int dx = std::abs(static_cast<int>(size.x) - centerX);
        const int dy = std::abs(static_cast<int>(size.y) - centerY);
        const int distance = std::max(dx, dy);

        size.x = 0;
        size.y = static_cast<lengthType>(distance);
        size.z = 0;

        // Grow each axis by one so the top ring index is inclusive (MoonLight's
        // size.x++/y++/z++). Result: {1, distance + 1, 1}.
        size.x++;
        size.y++;
        size.z++;
    }

    bool modifyLogical(Coord3D& pos) const override {
        // Chebyshev (block) distance from the floor-biased box centre. Reads the
        // stashed physical box. z plays no part — square rings live in the x/y plane.
        const int centerX = (modifierSize_.x + 1) / 2 - 1;
        const int centerY = (modifierSize_.y + 1) / 2 - 1;

        const int dx = std::abs(static_cast<int>(pos.x) - centerX);
        const int dy = std::abs(static_cast<int>(pos.y) - centerY);

        // Block distance is the maximum of the two deltas (creates square rings).
        const int distance = std::max(dx, dy);

        pos.x = 0;
        pos.y = static_cast<lengthType>(distance);
        pos.z = 0;
        return true; // never rejects a coord
    }

private:
    // The physical box, stashed at modifyLogicalSize time so the const fold can
    // read the centre (the MoonLight `modifierSize` pattern).
    Coord3D modifierSize_;
};

} // namespace mm