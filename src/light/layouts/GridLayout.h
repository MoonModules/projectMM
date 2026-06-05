#pragma once

#include <limits>
#include <cstdint>
#include "light/layouts/Layouts.h"
#include "light/light_types.h"  // lengthType, nrOfLightsType

namespace mm {

// Default grid edge. Small by default so a fresh device shows a manageable
// 16×16×1 (and the desktop boot grid the same); users scale up. Owned by
// GridLayout — it's this layout's default, also read by the composition roots
// (main.cpp / main_desktop.cpp) to size the boot grid.
constexpr lengthType defaultGridSize = 16;

class GridLayout : public LayoutBase {
public:
    lengthType width = defaultGridSize;
    lengthType height = defaultGridSize;
    lengthType depth = 1;

    void onBuildControls() override {
        controls_.addInt16("width",  width,  1, 512);
        controls_.addInt16("height", height, 1, 512);
        controls_.addInt16("depth",  depth,  1, 512);
    }

    nrOfLightsType lightCount() const override {
        // Multiply in uint32_t to detect overflow before casting.
        uint32_t n = static_cast<uint32_t>(width) * height * depth;
        constexpr uint32_t kMax = std::numeric_limits<nrOfLightsType>::max();
        return static_cast<nrOfLightsType>(n > kMax ? kMax : n);
    }

    void forEachCoord(CoordCallback cb, void* ctx) const override {
        // Use uint32_t for idx so it never wraps on uint16_t nrOfLightsType
        // (e.g. no-PSRAM ESP32 where 512×512 > 65535). Stop at the clamped
        // lightCount() so emitted indices stay within the allocated buffer.
        const uint32_t limit = lightCount();
        uint32_t idx = 0;
        for (lengthType z = 0; z < depth && idx < limit; z++) {
            for (lengthType y = 0; y < height && idx < limit; y++) {
                for (lengthType x = 0; x < width && idx < limit; x++) {
                    cb(ctx, static_cast<nrOfLightsType>(idx++), x, y, z);
                }
            }
        }
    }
};

} // namespace mm
