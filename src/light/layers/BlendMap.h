#pragma once

#include "light/layers/Buffer.h"
#include "light/layers/MappingLUT.h"

#include <cstring>

namespace mm {

// Reads from logical buffer (src), writes to physical buffer (dst) via LUT.
// Additive blending with clamping (for future multi-layer support).
inline void blendMap(const Buffer& src, Buffer& dst, const MappingLUT& lut, uint8_t channelsPerLight) {
    if (!lut.hasLUT()) {
        std::memcpy(dst.data(), src.data(), src.bytes());
        return;
    }

    // Clear first so physical cells with no source (sparse layouts — a sphere's
    // lattice gaps) stay black.
    dst.clear();
    nrOfLightsType logCount = lut.logicalCount();

    if (lut.overwrites()) {
        // Each destination is written by at most one source (mirror, shuffle,
        // sparse box→driver), so plain-copy — no read-back, no clamp. This is
        // ~4× the additive path and is the case every current layout takes.
        for (nrOfLightsType li = 0; li < logCount; li++) {
            const uint8_t* srcLight = src.data() + static_cast<size_t>(li) * channelsPerLight;
            lut.forEachDestination(li, [&](nrOfLightsType physIdx) {
                uint8_t* dstLight = dst.data() + static_cast<size_t>(physIdx) * channelsPerLight;
                for (uint8_t c = 0; c < channelsPerLight; c++) dstLight[c] = srcLight[c];
            });
        }
        return;
    }

    // Additive blend with clamping — for a map that folds multiple sources onto
    // one destination (e.g. future multi-layer compositing).
    for (nrOfLightsType li = 0; li < logCount; li++) {
        const uint8_t* srcLight = src.data() + static_cast<size_t>(li) * channelsPerLight;
        lut.forEachDestination(li, [&](nrOfLightsType physIdx) {
            uint8_t* dstLight = dst.data() + static_cast<size_t>(physIdx) * channelsPerLight;
            for (uint8_t c = 0; c < channelsPerLight; c++) {
                uint16_t sum = static_cast<uint16_t>(dstLight[c]) + srcLight[c];
                dstLight[c] = sum > 255 ? 255 : static_cast<uint8_t>(sum);
            }
        });
    }
}

} // namespace mm
