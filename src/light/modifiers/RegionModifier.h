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
// shrinks the logical box to the region (modifyLogicalSize) and folds each
// physical light into region-local space by subtracting the start offset, then
// REJECTS any physical light that lands outside the region (modifyLogical returns
// false) — the "drop outside" that makes everything beyond the region dark. A 1:1
// fold, same family as CheckerboardModifier.
//
// Rounding rule (spec: docs/moonmodules/light/RegionModifier.md): HALF-OPEN
// [startPixel, endPixel). start% floors to the lower pixel; end% ceils to an
// EXCLUSIVE pixel. This makes abutting regions tile exactly — a 0..50 and a
// 50..100 layer split a 128-wide axis into 0..63 and 64..127 with no overlap and
// no gap. (start 33 / end 66 on a 4-wide axis → floor(1.32)=1 .. ceil(2.64)=3 →
// pixels 1..2, width 2; end 100 on a W-wide axis → ceil(W)=W → full width.)
//
// Off-screen windows. start/end percentages may go **negative or past 100** (Int16
// on the wire, default UI range −100..200) to slide the window partly or fully out
// of the visible box. The logical box is the FULL window span (endPixel−startPixel),
// so the effect renders at a consistent scale; physical lights outside the window
// are dropped, and window cells with no physical light under them (the off-screen
// part) are simply dark. A window entirely off-box renders nothing — the layer goes
// dark, which is how you move an effect completely out of view.
//
// Fast path: the cheapest carve is *no modifier* — then Layer::rebuildLUT takes
// its identity/memcpy path with zero carving cost. Adding a full-region (0/100)
// RegionModifier is correct but not free; the default is to not add one.
class RegionModifier : public ModifierBase {
public:
    lengthType startX = 0,   startY = 0,   startZ = 0;
    lengthType endX   = 100, endY   = 100, endZ   = 100;

    void onBuildControls() override {
        // Int16 so negative / >100 percentages round-trip; the carve math clamps.
        controls_.addInt16("startX", startX);
        controls_.addInt16("startY", startY);
        controls_.addInt16("startZ", startZ);
        controls_.addInt16("endX",   endX);
        controls_.addInt16("endY",   endY);
        controls_.addInt16("endZ",   endZ);
    }

    void modifyLogicalSize(Coord3D& size) override {
        // Window edges in pixels — NOT clamped to the box, so the window can sit
        // partly or fully outside it. The logical box is the full window span (the
        // effect renders at a fixed scale regardless of how far off-screen it sits —
        // moving start+end together slides it without resizing, like an OS window).
        start_ = {floorPx(startX, size.x), floorPx(startY, size.y), floorPx(startZ, size.z)};
        const Coord3D end{ceilPx(endX, size.x), ceilPx(endY, size.y), ceilPx(endZ, size.z)};
        // Window span = end − start, floored to ≥1 on a non-empty axis so the effect
        // always has a box to render into (a fully off-screen window still renders; it
        // just maps no lights to the screen). A genuinely 0-extent axis stays 0.
        region_ = {span(start_.x, end.x, size.x), span(start_.y, end.y, size.y),
                   span(start_.z, end.z, size.z)};
        size = region_;
    }

    bool modifyLogical(Coord3D& pos) const override {
        // Fold a physical light into window-local space. A light whose window-local
        // coord lands outside the window is dropped; window cells with no physical
        // light under them (the off-screen part) get no source and stay dark.
        pos = pos - start_;
        return pos.x >= 0 && pos.x < region_.x &&
               pos.y >= 0 && pos.y < region_.y &&
               pos.z >= 0 && pos.z < region_.z;
    }

private:
    Coord3D start_;    // window start pixel (may be negative), stashed for the fold
    Coord3D region_;   // window size (logical box), stashed for the bound check

    // Lower window edge: floor(start% · extent). Unclamped — may be negative.
    // Floored toward −∞ (not truncated) so a negative percentage rounds down
    // consistently with the positive case.
    static lengthType floorPx(lengthType pct, lengthType extent) {
        if (extent <= 0) return 0;
        long num = static_cast<long>(pct) * extent;
        long q = num / 100;
        if (num % 100 != 0 && num < 0) q -= 1;   // floor toward −∞ for negatives
        return static_cast<lengthType>(q);
    }

    // Upper window edge (EXCLUSIVE): ceil(end% · extent). Unclamped — may exceed
    // extent. Ceiled toward +∞.
    static lengthType ceilPx(lengthType pct, lengthType extent) {
        if (extent <= 0) return 0;
        long num = static_cast<long>(pct) * extent;
        long q = num / 100;
        if (num % 100 != 0 && num > 0) q += 1;    // ceil toward +∞ for positives
        return static_cast<lengthType>(q);
    }

    // Window span on an axis = end − start, floored to ≥1 on a non-empty axis so the
    // effect always has a box to render into. A 0-extent axis stays 0. (0..50 then
    // 50..100 on 128 → 64 + 64, tiling; −50..50 on 100 → span 100, slid half off.)
    static lengthType span(lengthType startPx, lengthType endPx, lengthType extent) {
        if (extent <= 0) return 0;
        lengthType s = static_cast<lengthType>(endPx - startPx);
        return s >= 1 ? s : 1;
    }
};

} // namespace mm
