#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

// Carves the layer down to a sub-region of the physical bounding box: the effect
// renders only into the region, everything outside is dark. The region is given
// as PERCENTAGES of the physical extent on each axis (start 0 / end 100 = the
// full box, an identity carve), so it survives a physical resize — a 0..50 region
// stays the left half whether the panel is 64 or 128 wide.
//
// It is a region *crop*, the textbook crop/region node of any compositor: it
// resizes the logical box to the region (logicalDimensions) and maps each
// region-local cell 1:1 to its box cell at the region's start offset
// (mapToPhysical). Because the logical box is already the region size, every
// region cell is in-bounds — the "drop outside" is achieved by the smaller box,
// exactly as a Mirror shrinks the box to the half it folds. Never fans out
// (maxMultiplier == 1), same 1:1 family as CheckerboardModifier.
//
// Rounding rule (spec: docs/moonmodules/light/RegionModifier.md): HALF-OPEN
// [startPixel, endPixel). start% floors to the lower pixel; end% ceils to an
// EXCLUSIVE pixel. This makes abutting regions tile exactly — a 0..50 and a
// 50..100 layer split a 128-wide axis into 0..63 and 64..127 with no overlap and
// no gap. Clamped so the region is always ≥1 pixel and never runs off the box.
// (start 33 / end 66 on a 4-wide axis → floor(1.32)=1 .. ceil(2.64)=3 → pixels
// 1..2, width 2; end 100 on a W-wide axis → ceil(W)=W → full width.) Negative /
// >100 percentages are legal on the wire (Int16) but clamp to the box here.
//
// Fast path: the cheapest carve is *no modifier* — then Layer::rebuildLUT takes
// its identity/memcpy path with zero carving cost. Adding a full-region (0/100)
// RegionModifier is correct but not free; the default is to not add one.
class RegionModifier : public ModifierBase {
public:
    lengthType startX = 0,   startY = 0,   startZ = 0;
    lengthType endX   = 100, endY   = 100, endZ   = 100;

    nrOfLightsType maxMultiplier() const override { return 1; }  // 1:1 inside, never fans out

    void onBuildControls() override {
        // Int16 so negative / >100 percentages round-trip; the carve math clamps.
        controls_.addInt16("startX", startX);
        controls_.addInt16("startY", startY);
        controls_.addInt16("startZ", startZ);
        controls_.addInt16("endX",   endX);
        controls_.addInt16("endY",   endY);
        controls_.addInt16("endZ",   endZ);
    }

    void logicalDimensions(lengthType physW, lengthType physH, lengthType physD,
                           lengthType& logW, lengthType& logH, lengthType& logD) const override {
        logW = axisCount(startX, endX, physW);
        logH = axisCount(startY, endY, physH);
        logD = axisCount(startZ, endZ, physD);
    }

    void mapToPhysical(lengthType lx, lengthType ly, lengthType lz,
                       lengthType physW, lengthType physH, lengthType physD,
                       nrOfLightsType* outPhysicals, nrOfLightsType& outCount,
                       nrOfLightsType maxOut) const override {
        outCount = 0;
        if (maxOut == 0) return;
        // Region-local → box coordinate: add each axis's start-pixel offset. lx/ly/lz
        // are already bounded by the region size logicalDimensions reported, so the
        // result is always in-box; no per-cell drop needed.
        const lengthType bx = lx + axisStart(startX, physW);
        const lengthType by = ly + axisStart(startY, physH);
        const lengthType bz = lz + axisStart(startZ, physD);
        outPhysicals[0] = static_cast<nrOfLightsType>(bz) * static_cast<nrOfLightsType>(physW) * static_cast<nrOfLightsType>(physH) +
                          static_cast<nrOfLightsType>(by) * static_cast<nrOfLightsType>(physW) +
                          static_cast<nrOfLightsType>(bx);
        outCount = 1;
    }

private:
    // First pixel of the region on an axis: floor(start% · extent), clamped to
    // [0, extent-1]. Shared by logicalDimensions (via axisCount) and mapToPhysical
    // so the two can't drift.
    static lengthType axisStart(lengthType startPct, lengthType extent) {
        if (extent <= 0) return 0;
        long p = (static_cast<long>(startPct) * extent) / 100;   // floor for non-negative; clamps below anyway
        if (p < 0) p = 0;
        if (p > extent - 1) p = extent - 1;
        return static_cast<lengthType>(p);
    }

    // Region size on an axis (half-open): count = endPixel - startPixel, where
    // endPixel is ceil(end% · extent) treated as EXCLUSIVE, clamped to
    // [startPixel+1, extent] so the region is ≥1 pixel and stays in the box.
    // Spec example: start 33 / end 66 on extent 4 → s=floor(1.32)=1,
    // endExcl=ceil(2.64)=3 → count = 3-1 = 2 (pixels 1,2). Default end 100 on
    // extent W → ceil(W)=W → count = W (full width). 0..50 then 50..100 on 128 →
    // 0..64 and 64..128 exclusive → 64 + 64, tiling exactly.
    static lengthType axisCount(lengthType startPct, lengthType endPct, lengthType extent) {
        if (extent <= 0) return 0;
        const lengthType s = axisStart(startPct, extent);
        // Ceiling division of (endPct * extent) / 100, for non-negative endPct.
        long num = static_cast<long>(endPct) * extent;
        long e = num <= 0 ? 0 : (num + 99) / 100;   // ceil, EXCLUSIVE end pixel
        if (e < s + 1) e = s + 1;                    // ≥1-pixel region
        if (e > extent) e = extent;                  // never past the box
        return static_cast<lengthType>(e - s);
    }
};

} // namespace mm
