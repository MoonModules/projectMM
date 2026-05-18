#pragma once

#include "light/Layer.h"
#include "light/Pixel.h"
#include <algorithm>
#include <cstring>
#include <span>

namespace mm::light {

// Blend+map all layers into a destination buffer.
// For each layer: read logical pixels, apply LUT, blend into dest.
// dest must be pre-allocated to the LayoutGroup's totalPixelCount().
// Blending: additive with clamp to 255.
inline void blendMap(std::span<RGB> dest,
                     const Layer* layers, size_t layerCount) {
    // Clear destination
    std::memset(dest.data(), 0, dest.size_bytes());

    for (size_t l = 0; l < layerCount; ++l) {
        const auto& layer = layers[l];
        const auto& lut = layer.lut();
        auto srcPixels = layer.buffer().pixels();

        for (size_t logIdx = 0; logIdx < lut.logicalCount(); ++logIdx) {
            size_t destCount = lut.destinationCount(logIdx);
            if (destCount == 0) continue;

            // Read source pixel — for a 1:1 LUT the logical index
            // IS the source index. For mirrored LUTs the destination
            // tells us where to write, the source is always logIdx.
            RGB src = (logIdx < srcPixels.size())
                ? srcPixels[logIdx]
                : RGB::black();

            const uint16_t* dests = lut.destinations(logIdx);
            for (size_t d = 0; d < destCount; ++d) {
                uint16_t physIdx = dests[d];
                if (physIdx < dest.size()) {
                    // Additive blend with clamp
                    auto& dst = dest[physIdx];
                    dst.r = static_cast<uint8_t>(std::min(255, dst.r + src.r));
                    dst.g = static_cast<uint8_t>(std::min(255, dst.g + src.g));
                    dst.b = static_cast<uint8_t>(std::min(255, dst.b + src.b));
                }
            }
        }
    }
}

} // namespace mm::light
