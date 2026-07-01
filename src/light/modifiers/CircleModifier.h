#pragma once

#include "light/modifiers/ModifierBase.h"

#include <cmath> // std::sqrt

namespace mm {

// Maps a 1D strip onto concentric circular rings: every physical light folds to
// its Euclidean distance from the box centre, so a linear index becomes a radius.
// The logical box collapses to a single column of `radius+1` cells (x=1, z=1),
// which an effect then paints as rings. A static (build-time) remap — the fold is
// baked into the Layer's mapping LUT once per rebuild, nothing runs per frame.
//
// Geometry (reproduced exactly from MoonLight):
//   dx = pos.x - box.x/2,  dy = pos.y - box.y/2,  dz = pos.z - box.z/2   (integer)
//   distance = sqrt(dx*dx + dy*dy + dz*dz)                               (float)
//   pos -> (0, distance, 0)
// The logical size is the incoming box run through that same fold (turning the
// far corner into its distance) and then grown by one on every axis.
//
// Prior art: MoonLight's Circle modifier (M_MoonLight.h) — same centre-offset +
// Euclidean-distance fold and the same +1-per-axis size bump. MoonLight tags it
// 💎; the 💫 here marks the MoonLight origin per projectMM convention. Written
// fresh against our ModifierBase fold interface (modifyLogicalSize / modifyLogical)
// rather than MoonLight's modifySize / modifyPosition Node API.
// Author: MoonLight — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Modifiers/M_MoonLight.h
class CircleModifier : public ModifierBase {
public:
    const char* tags() const override { return "💫"; } // MoonLight origin

    // 1D -> 2D remap: the modifier only ever produces (0, distance, 0), so it
    // meaningfully transforms in the x/y plane. Advisory UI chip only. This maps
    // MoonLight's dim()==_2D onto projectMM's Dim enum. // RECONSTRUCTED
    Dim dimensions() const override { return Dim::D2; }

    // No controls: MoonLight's Circle exposes none — the centre is always the box
    // middle and the distance is fixed Euclidean. onBuildControls() stays default.

    void modifyLogicalSize(Coord3D& size) override {
        // Stash the incoming box so the per-light fold (const, no box arg) can read
        // the centre from it — the MoonLight `modifierSize` pattern.
        modifierSize_ = size;

        // Reshape the logical box exactly as MoonLight does: run the box's own far
        // corner through the SAME distance fold (yielding (0, cornerDistance, 0)),
        // then grow every axis by one. modifyLogical is const, so fold a local copy
        // and copy the result back into `size` — same net transform as MoonLight
        // calling modifyPosition(layer->size). // RECONSTRUCTED (fold-order equivalence)
        Coord3D corner = size;
        fold(corner);
        size = corner;

        // Change the size to be one bigger in each dimension.
        size.x++;
        size.y++;
        size.z++;
    }

    bool modifyLogical(Coord3D& pos) const override {
        fold(pos);
        return true; // Circle never rejects a coordinate.
    }

private:
    Coord3D modifierSize_; // the box seen at build time, stashed for the fold

    // Fold a coordinate to (0, distance-from-centre, 0). Integer centre offsets,
    // float Euclidean distance truncated back to the coordinate type — MoonLight's
    // exact math (int dx/dy/dz, sqrt, assign to position.y). Runs on the cold build
    // path, so float sqrt is fine here.
    void fold(Coord3D& position) const {
        // Calculate the offset from the centre (integer division on the box size,
        // matching MoonLight's modifierSize.{x,y,z} / 2).
        const int dx = position.x - modifierSize_.x / 2;
        const int dy = position.y - modifierSize_.y / 2;
        const int dz = position.z - modifierSize_.z / 2;

        // Euclidean distance from the centre.
        const float distance = std::sqrt(static_cast<float>(dx * dx + dy * dy + dz * dz));

        position.x = 0;
        position.y = static_cast<lengthType>(distance);
        position.z = 0;
    }
};

} // namespace mm