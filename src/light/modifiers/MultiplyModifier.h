#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

// Tiles the logical image across the physical box `multiply` times per axis,
// optionally mirroring alternate tiles. The logical box is the physical box
// divided by the per-axis multiplier; each logical light fans out to one
// physical position per tile. With a multiplier of 2 and mirror on, an axis
// folds in half — the classic kaleidoscope mirror (this subsumes the old
// MirrorModifier: mirror = multiply 2 + mirror true).
//
// Prior art: MoonLight's Multiply modifier (M_MoonLight.h) — same tile+mirror
// shape (`position % modifierSize`, odd tiles reflected). We expose per-axis
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

    // Upper bound on fan-out = product of the raw control multipliers. Computed
    // without the grid extents (not known here), so it's an over-estimate when a
    // multiplier exceeds its axis (the effective multiplier clamps to the extent
    // in mapToPhysical). The Layer sizes its per-light scratch buffer to this, so
    // over-estimating is safe (a few unused slots); under-estimating would
    // truncate. No fixed cap — limited only by memory. e.g. 8×8 = 64.
    nrOfLightsType maxMultiplier() const override {
        const lengthType cx = multiplyX ? multiplyX : 1;
        const lengthType cy = multiplyY ? multiplyY : 1;
        const lengthType cz = multiplyZ ? multiplyZ : 1;
        return static_cast<nrOfLightsType>(cx) *
               static_cast<nrOfLightsType>(cy) *
               static_cast<nrOfLightsType>(cz);
    }

    void onBuildControls() override {
        // 1–64 tiles per axis. The fan-out (product) sizes the LUT scratch buffer
        // dynamically, so the cap is generous, not a buffer constraint — more
        // tiles than the grid has pixels just yields 1-pixel tiles.
        controls_.addUint8("multiplyX", multiplyX, 1, 64);
        controls_.addUint8("multiplyY", multiplyY, 1, 64);
        controls_.addUint8("multiplyZ", multiplyZ, 1, 64);
        controls_.addBool("mirrorX", mirrorX);
        controls_.addBool("mirrorY", mirrorY);
        controls_.addBool("mirrorZ", mirrorZ);
    }

    void logicalDimensions(lengthType physW, lengthType physH, lengthType physD,
                           lengthType& logW, lengthType& logH, lengthType& logD) const override {
        // Logical box is the physical box divided by the EFFECTIVE multiplier
        // (clamped to the axis extent — see eff()). On a 2D grid (physD=1) any
        // multiplyZ clamps to 1, so logD stays 1 and Z multiplication is a no-op
        // — you can't tile an axis more times than it has pixels.
        logW = physW / eff(multiplyX, physW);
        logH = physH / eff(multiplyY, physH);
        logD = physD / eff(multiplyZ, physD);
    }

    void mapToPhysical(lengthType lx, lengthType ly, lengthType lz,
                       lengthType physW, lengthType physH, lengthType physD,
                       nrOfLightsType* outPhysicals, nrOfLightsType& outCount,
                       nrOfLightsType maxOut) const override {
        outCount = 0;
        const lengthType mX = eff(multiplyX, physW);
        const lengthType mY = eff(multiplyY, physH);
        const lengthType mZ = eff(multiplyZ, physD);
        const lengthType tileW = physW / mX;
        const lengthType tileH = physH / mY;
        const lengthType tileD = physD / mZ;

        // One physical position per tile on each axis. For a mirror-enabled axis,
        // odd tiles reflect within their tile (MoonLight's `size-1-pos`).
        for (lengthType tz = 0; tz < mZ; tz++) {
            const lengthType pz = tileOrigin(tz, tileD) + axisOffset(lz, tileD, tz, mirrorZ);
            for (lengthType ty = 0; ty < mY; ty++) {
                const lengthType py = tileOrigin(ty, tileH) + axisOffset(ly, tileH, ty, mirrorY);
                for (lengthType tx = 0; tx < mX; tx++) {
                    if (outCount >= maxOut) return;
                    const lengthType px = tileOrigin(tx, tileW) + axisOffset(lx, tileW, tx, mirrorX);
                    outPhysicals[outCount++] =
                        static_cast<nrOfLightsType>(pz) * static_cast<nrOfLightsType>(physW) * static_cast<nrOfLightsType>(physH) +
                        static_cast<nrOfLightsType>(py) * static_cast<nrOfLightsType>(physW) +
                        static_cast<nrOfLightsType>(px);
                }
            }
        }
    }

private:
    // Effective multiplier for an axis: the control value clamped to [1, extent].
    // ≥1 avoids divide-by-zero; ≤extent because tiling more times than the axis
    // has pixels is meaningless (and would blank the layer via logD=0). So a
    // multiplyZ on a depth-1 (2D) layout clamps to 1 — no effect, as expected.
    static lengthType eff(uint8_t mult, lengthType extent) {
        lengthType m = mult ? mult : 1;
        if (extent > 0 && m > extent) m = extent;
        return m;
    }

    static lengthType tileOrigin(uint8_t tile, lengthType tileSize) {
        return static_cast<lengthType>(tile) * tileSize;
    }
    // Offset within a tile: identity, or reflected for odd tiles when mirroring.
    static lengthType axisOffset(lengthType l, lengthType tileSize, uint8_t tile, bool mirror) {
        return (mirror && (tile & 1)) ? static_cast<lengthType>(tileSize - 1 - l) : l;
    }
};

} // namespace mm
