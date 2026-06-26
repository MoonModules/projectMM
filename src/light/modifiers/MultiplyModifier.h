#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

// Tiles the logical image across the physical box `multiply` times per axis,
// optionally mirroring alternate tiles. The logical box is the physical box
// divided by the per-axis multiplier. Under the fold build the fan-out is free:
// every physical light folds (`pos % logicalSize`) onto its logical cell, so the
// N physical lights of N tiles all land on the same logical light — N:1 emerges,
// no fan-out list. With multiply 2 + mirror on, an axis folds in half — the
// classic kaleidoscope mirror (this subsumes a standalone Mirror: it's just
// multiply 2 + mirror true).
//
// Prior art: MoonLight's Multiply modifier (M_MoonLight.h) — same tile+mirror
// fold (`position % modifierSize`, odd tiles reflected). We expose per-axis
// mirror bools (3) instead of MoonLight's single mirror flag, and per-axis
// multipliers, so X/Y/Z can fold and tile independently.
class MultiplyModifier : public ModifierBase {
public:
    const char* tags() const override { return "💫"; }  // MoonLight origin

    // Tiles per axis. 1 = no multiplication on that axis.
    uint8_t multiplyX = 2;
    uint8_t multiplyY = 2;
    uint8_t multiplyZ = 1;
    // Reflect alternate (odd-numbered) tiles on this axis. All default on — a
    // mirror on an axis the layout doesn't use (e.g. Z on a 2D grid) is a no-op,
    // so defaulting them true gives a kaleidoscope on whatever axes exist.
    bool mirrorX = true;
    bool mirrorY = true;
    bool mirrorZ = true;

    void onBuildControls() override {
        // 1–64 tiles per axis. More tiles than the grid has pixels just yields
        // 1-pixel tiles (the effective multiplier clamps to the axis extent).
        controls_.addUint8("multiplyX", multiplyX, 1, 64);
        controls_.addUint8("multiplyY", multiplyY, 1, 64);
        controls_.addUint8("multiplyZ", multiplyZ, 1, 64);
        controls_.addBool("mirrorX", mirrorX);
        controls_.addBool("mirrorY", mirrorY);
        controls_.addBool("mirrorZ", mirrorZ);
    }

    void modifyLogicalSize(Coord3D& size) override {
        // Logical box is the incoming box divided by the EFFECTIVE multiplier
        // (clamped to the axis extent — see eff()). On a 2D grid (size.z=1) any
        // multiplyZ clamps to 1, so depth stays 1 and Z multiplication is a no-op
        // — you can't tile an axis more times than it has pixels. When the extent
        // isn't divisible by the multiplier (e.g. 5 / 2 = 2), the tiles cover only
        // `tile * mult` pixels; the leftover strip at the high edge is unmapped
        // (covered_ records the covered extent so the fold can reject it).
        const lengthType mX = eff(multiplyX, size.x), mY = eff(multiplyY, size.y), mZ = eff(multiplyZ, size.z);
        size.x /= mX; size.y /= mY; size.z /= mZ;
        tile_ = size;
        covered_ = {static_cast<lengthType>(size.x * mX), static_cast<lengthType>(size.y * mY),
                    static_cast<lengthType>(size.z * mZ)};
    }

    bool modifyLogical(Coord3D& pos) const override {
        // A coord in the leftover edge strip (extent not divisible by the multiplier)
        // has no tile — drop it, rather than wrap it and duplicate the edge.
        if ((covered_.x > 0 && pos.x >= covered_.x) ||
            (covered_.y > 0 && pos.y >= covered_.y) ||
            (covered_.z > 0 && pos.z >= covered_.z)) return false;
        // Fold a coord into its tile: the tile index decides whether to reflect
        // (odd tile, mirror on), then wrap into the tile. Reads the stashed tile size.
        pos.x = foldAxis(pos.x, tile_.x, mirrorX);
        pos.y = foldAxis(pos.y, tile_.y, mirrorY);
        pos.z = foldAxis(pos.z, tile_.z, mirrorZ);
        return true;
    }

private:
    Coord3D tile_;      // output tile size, stashed in modifyLogicalSize for the fold
    Coord3D covered_;   // pixels the tiles actually cover (tile*mult); the leftover edge is dropped

    // Effective multiplier for an axis: the control value clamped to [1, extent].
    // ≥1 avoids divide-by-zero; ≤extent because tiling more times than the axis
    // has pixels is meaningless (and would blank the layer). So a multiplyZ on a
    // depth-1 (2D) layout clamps to 1 — no effect, as expected.
    static lengthType eff(uint8_t mult, lengthType extent) {
        lengthType m = mult ? mult : 1;
        if (extent > 0 && m > extent) m = extent;
        return m;
    }

    // Fold a physical coordinate `p` into a `logical`-sized tile, reflecting odd
    // tiles when mirroring. logical==0 (degenerate axis) passes through unchanged.
    static lengthType foldAxis(lengthType p, lengthType logical, bool mirror) {
        if (logical <= 0) return p;
        const lengthType tile   = static_cast<lengthType>(p / logical);
        const lengthType within = static_cast<lengthType>(p % logical);
        return (mirror && (tile & 1)) ? static_cast<lengthType>(logical - 1 - within) : within;
    }
};

} // namespace mm
