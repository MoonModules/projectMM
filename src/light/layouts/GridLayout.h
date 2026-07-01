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

// Author: MoonLight — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h
class GridLayout : public LayoutBase {
public:
    lengthType width = defaultGridSize;
    lengthType height = defaultGridSize;
    lengthType depth = 1;
    bool serpentine = false;   // odd rows wired in reverse (boustrophedon) — the standard matrix
                               // strip layout where the strip snakes back and forth row to row.

    void onBuildControls() override {
        controls_.addInt16("width",  width,  1, 512);
        controls_.addInt16("height", height, 1, 512);
        controls_.addInt16("depth",  depth,  1, 512);
        controls_.addBool("serpentine", serpentine);
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
                // Serpentine: the strip enters odd rows from the high-x end, so the driver index
                // walks x in reverse there. The emitted COORDINATE is still the true (x,y,z) — only
                // the index→position order changes, which is exactly what makes the mapping
                // non-identity (driver index i ≠ box cell i).
                const bool reverse = serpentine && (y & 1);
                for (lengthType i = 0; i < width && idx < limit; i++) {
                    const lengthType x = reverse ? static_cast<lengthType>(width - 1 - i) : i;
                    cb(ctx, static_cast<nrOfLightsType>(idx++), x, y, z);
                }
            }
        }
    }
};

} // namespace mm
