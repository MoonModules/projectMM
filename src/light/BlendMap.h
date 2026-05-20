#pragma once

#include "light/Buffer.h"
#include "light/MappingLUT.h"

#include <cstring>

namespace mm {

// Reads from logical buffer (src), writes to physical buffer (dst) via LUT.
// Additive blending with clamping (for future multi-layer support).
inline void blendMap(const Buffer& src, Buffer& dst, const MappingLUT& lut, uint8_t channelsPerLight) {
    if (!lut.hasLUT()) {
        std::memcpy(dst.data(), src.data(), src.bytes());
        return;
    }

    dst.clear();
    nrOfLightsType logCount = lut.logicalCount();

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
