#pragma once

#include "light/layers/Buffer.h"
#include "light/layers/MappingLUT.h"

#include <cstring>

namespace mm {

/// @defgroup BlendMap Composing a layer into the frame
/// @{
/// The one pass that reads a layer's logical buffer and writes it, mapped and blended, into the physical frame.
///
/// @moreinfo
///
/// The pass runs once per enabled layer each frame, driven by the Drivers container, which reads each layer's own `blendMode` and `opacity` controls along with the stack order.
///
/// `clearFirst` clears the destination before writing, which the first and bottom layer of a composite does.
/// That is what keeps physical cells with no source, such as a sparse layout's lattice gaps, black.
/// Later layers pass false and blend onto the frame accumulated below.
/// A single layer passes `Overwrite` at full opacity with a clear, which takes the exact fast path this had before composition existed.
///
/// ## The combine math is integer-only
///
/// The hot-path per-light rule applies, so one specialised loop is chosen once before the per-light loop and there is no per-pixel mode check.
///
/// | Mode | What it does |
/// |------|--------------|
/// | Overwrite | a plain copy with no read-back. A dense grid with no LUT is a `memcpy`; a single-write LUT copies per mapped light |
/// | Additive | the destination plus the source scaled by opacity, summed with saturation at 255 |
/// | Alpha | the textbook 8-bit alpha-over, the source and destination weighted by opacity, divided by 255 through the fast reciprocal. Full opacity collapses to a plain overwrite at no blend cost |
///
/// A non-overwriting LUT, one folding several logical lights onto a single physical cell, routes through the additive accumulate path so overlaps sum with clamping rather than last-writer-wins.
///
/// ## How a light finds its destination
///
/// A dense-grid layer has no LUT, so its buffer blends one to one with the source index equal to the physical index and no lookup at all.
/// A layer with a LUT maps each logical light to its physical destinations first.
/// Physical indices come from the LUT, which is built in range from the shared Layouts, so they address the destination in bounds by construction.
///
/// ## Why every mapped access is bounded
///
/// A reshape rebuilds the mapping and the driver's output buffer in separate steps of one `prepareTree` sweep, Layouts first, then the Layer, then Drivers.
/// A render tick can land between them, holding the new mapping's physical indices and the old, smaller buffer.
/// Unbounded, that writes past the end and corrupts the heap, and the failure then surfaces in an unrelated allocation later, which is what made resizing a layout look intermittently fatal.
///
/// The identity path has always clamped to the smaller of the two buffers for the same reason; the mapped path did not.
/// This is a bound rather than a fix for the ordering.
/// The window is still there and the frame drawn inside it is briefly wrong, but it cannot corrupt memory, which is the property that matters.
///
/// ## What Overwrite defers to
///
/// Overwrite is the default, for a single layer or the bottom of a composite, and it defers to the LUT's own overwrites flag.
/// A mapping writing each physical cell once, a mirror, a shuffle or a sparse box, plain-copies.
/// A mapping folding several logical lights onto one cell accumulates additively within the layer, with clamping, while cross-layer Additive and Alpha remain the explicit ops.

/// How a layer's pixels combine into the destination during composition.
enum class BlendOp : uint8_t {
    Overwrite,  ///< `dst = src` (replace; the first/bottom layer, fastest — no read-back)
    Alpha,      ///< `dst = src*opacity + dst*(255-opacity)` (opacity-weighted over)
    Additive,   ///< `dst = clamp(dst + src*opacity/255)` (adds light, never dims)
};

/// Fast 8-bit divide by 255, exact over the full 16-bit range, avoiding a real divide on the hot path.
inline uint8_t div255(uint16_t x) { return static_cast<uint8_t>((x + (x >> 8) + 1) >> 8); }

/// Blend one layer's `src` into `dst` through its LUT, once per frame.
inline void blendMap(const Buffer& src, Buffer& dst, const MappingLUT& lut,
                     uint8_t channelsPerLight,
                     BlendOp op = BlendOp::Overwrite, uint8_t opacity = 255,
                     bool clearFirst = true) {
    // No LUT is an identity map, the dense-grid case: blend one to one with no lookup.
    if (!lut.hasLUT()) {
        const size_t n = src.bytes() < dst.bytes() ? src.bytes() : dst.bytes();
        const uint8_t* s = src.data();
        uint8_t* d = dst.data();
        if (op == BlendOp::Overwrite && opacity == 255) {
            std::memcpy(d, s, n);
            return;
        }
        const bool full = (opacity == 255);
        if (op == BlendOp::Additive) {
            for (size_t i = 0; i < n; i++) {
                uint16_t sv = full ? s[i] : div255(static_cast<uint16_t>(s[i]) * opacity);
                uint16_t sum = static_cast<uint16_t>(d[i]) + sv;
                d[i] = sum > 255 ? 255 : static_cast<uint8_t>(sum);
            }
        } else {  // Alpha (over)
            const uint16_t inv = static_cast<uint16_t>(255 - opacity);
            for (size_t i = 0; i < n; i++) {
                d[i] = full ? s[i]
                            : div255(static_cast<uint16_t>(s[i]) * opacity +
                                     static_cast<uint16_t>(d[i]) * inv);
            }
        }
        return;
    }

    if (clearFirst) dst.clear();   // bottom layer: cells with no source stay black
    const nrOfLightsType logCount = lut.logicalCount();
    const bool full = (opacity == 255);

    // Every mapped access below is bounded by these: a LUT entry is valid only against its own buffer.
    const size_t dstLights = channelsPerLight ? dst.bytes() / channelsPerLight : 0;
    const size_t srcLights = channelsPerLight ? src.bytes() / channelsPerLight : 0;

    // Overwrite defers to the LUT's own overwrites() flag, so a folding mapping accumulates.
    const bool effectiveAdditive = (op == BlendOp::Additive) ||
                                   (op == BlendOp::Overwrite && !lut.overwrites());

    // --- Plain overwrite (replace) — single-write LUT; copy, no read-back. ---
    if (op == BlendOp::Overwrite && full && lut.overwrites()) {
        for (nrOfLightsType li = 0; li < logCount; li++) {
            if (li >= srcLights) break;
            const uint8_t* srcLight = src.data() + static_cast<size_t>(li) * channelsPerLight;
            lut.forEachDestination(li, [&](nrOfLightsType physIdx) {
                if (physIdx >= dstLights) return;
                uint8_t* dstLight = dst.data() + static_cast<size_t>(physIdx) * channelsPerLight;
                for (uint8_t c = 0; c < channelsPerLight; c++) dstLight[c] = srcLight[c];
            });
        }
        return;
    }

    // --- Additive with clamp; opacity scales the source. full-opacity skips the scale. ---
    if (effectiveAdditive) {
        for (nrOfLightsType li = 0; li < logCount; li++) {
            if (li >= srcLights) break;
            const uint8_t* srcLight = src.data() + static_cast<size_t>(li) * channelsPerLight;
            lut.forEachDestination(li, [&](nrOfLightsType physIdx) {
                if (physIdx >= dstLights) return;
                uint8_t* dstLight = dst.data() + static_cast<size_t>(physIdx) * channelsPerLight;
                for (uint8_t c = 0; c < channelsPerLight; c++) {
                    uint16_t s = full ? srcLight[c] : div255(static_cast<uint16_t>(srcLight[c]) * opacity);
                    uint16_t sum = static_cast<uint16_t>(dstLight[c]) + s;
                    dstLight[c] = sum > 255 ? 255 : static_cast<uint8_t>(sum);
                }
            });
        }
        return;
    }

    // --- Alpha (over): dst = src*α + dst*(255-α). full-opacity collapses to overwrite. ---
    const uint16_t inv = static_cast<uint16_t>(255 - opacity);
    for (nrOfLightsType li = 0; li < logCount; li++) {
        if (li >= srcLights) break;
        const uint8_t* srcLight = src.data() + static_cast<size_t>(li) * channelsPerLight;
        lut.forEachDestination(li, [&](nrOfLightsType physIdx) {
            if (physIdx >= dstLights) return;
            uint8_t* dstLight = dst.data() + static_cast<size_t>(physIdx) * channelsPerLight;
            for (uint8_t c = 0; c < channelsPerLight; c++) {
                if (full) { dstLight[c] = srcLight[c]; continue; }
                dstLight[c] = div255(static_cast<uint16_t>(srcLight[c]) * opacity +
                                     static_cast<uint16_t>(dstLight[c]) * inv);
            }
        });
    }
}

/// @}
} // namespace mm
