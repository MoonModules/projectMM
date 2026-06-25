#pragma once

#include "light/layers/Buffer.h"
#include "light/layers/MappingLUT.h"

#include <cstring>

namespace mm {

// How a layer's pixels combine into the destination during composition.
//   Overwrite — dst = src (replace; the first/bottom layer, fastest)
//   Alpha     — dst = src*opacity + dst*(255-opacity)  (opacity-weighted over)
//   Additive  — dst = clamp(dst + src*opacity/255)      (adds light, never dims)
enum class BlendOp : uint8_t { Overwrite, Alpha, Additive };

// Fast 8-bit "divide by 255": exact for 0..65535. Avoids a real divide on the
// hot path (the textbook (x + (x>>8) + 1) >> 8 trick).
inline uint8_t div255(uint16_t x) { return static_cast<uint8_t>((x + (x >> 8) + 1) >> 8); }

// Reads from logical buffer (src), writes/blends to physical buffer (dst) via LUT.
//
// `op` + `opacity` decide how each light combines into dst; `clearFirst` clears
// dst before writing (the first/bottom layer in a composite — so physical cells
// with no source stay black; subsequent layers blend ONTO the accumulated frame).
// For a single layer the caller passes op=Overwrite, opacity=255, clearFirst=true,
// which takes the exact fast path this had before composition (memcpy / plain copy).
//
// The op/opacity branch is resolved ONCE here, before the per-light loop, so each
// path is a tight specialized loop with no per-pixel mode check (hot-path rule).
inline void blendMap(const Buffer& src, Buffer& dst, const MappingLUT& lut,
                     uint8_t channelsPerLight,
                     BlendOp op = BlendOp::Overwrite, uint8_t opacity = 255,
                     bool clearFirst = true) {
    // No LUT = identity map (dense grid, logical index == physical index — the
    // common case). Blend 1:1, src byte i → dst byte i, no LUT lookup. The
    // first/bottom full-opacity overwrite is a plain memcpy (the fast path);
    // a composited layer above it blends per op/opacity straight over dst.
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

    // Overwrite is the default op (single layer / bottom of a composite). It
    // defers to the LUT's own overwrites() flag: a mapping where each physical
    // cell is written once (mirror, shuffle, sparse box→driver) plain-copies;
    // a mapping that folds several logical lights onto one physical cell
    // (overwrites()=false) additively accumulates *within the layer* with clamp.
    // (Cross-layer Additive/Alpha are the explicit ops below.) So a full-opacity
    // Overwrite on a non-overwriting LUT routes to the additive accumulate path.
    const bool effectiveAdditive = (op == BlendOp::Additive) ||
                                   (op == BlendOp::Overwrite && !lut.overwrites());

    // --- Plain overwrite (replace) — single-write LUT; copy, no read-back. ---
    if (op == BlendOp::Overwrite && full && lut.overwrites()) {
        for (nrOfLightsType li = 0; li < logCount; li++) {
            const uint8_t* srcLight = src.data() + static_cast<size_t>(li) * channelsPerLight;
            lut.forEachDestination(li, [&](nrOfLightsType physIdx) {
                uint8_t* dstLight = dst.data() + static_cast<size_t>(physIdx) * channelsPerLight;
                for (uint8_t c = 0; c < channelsPerLight; c++) dstLight[c] = srcLight[c];
            });
        }
        return;
    }

    // --- Additive with clamp; opacity scales the source. full-opacity skips the scale. ---
    if (effectiveAdditive) {
        for (nrOfLightsType li = 0; li < logCount; li++) {
            const uint8_t* srcLight = src.data() + static_cast<size_t>(li) * channelsPerLight;
            lut.forEachDestination(li, [&](nrOfLightsType physIdx) {
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
        const uint8_t* srcLight = src.data() + static_cast<size_t>(li) * channelsPerLight;
        lut.forEachDestination(li, [&](nrOfLightsType physIdx) {
            uint8_t* dstLight = dst.data() + static_cast<size_t>(physIdx) * channelsPerLight;
            for (uint8_t c = 0; c < channelsPerLight; c++) {
                if (full) { dstLight[c] = srcLight[c]; continue; }
                dstLight[c] = div255(static_cast<uint16_t>(srcLight[c]) * opacity +
                                     static_cast<uint16_t>(dstLight[c]) * inv);
            }
        });
    }
}

} // namespace mm
