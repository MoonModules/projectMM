#pragma once

#include <limits>
#include <cstdint>
#include "light/layouts/Layouts.h"

namespace mm {

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
        nrOfLightsType idx = 0;
        for (lengthType z = 0; z < depth; z++) {
            for (lengthType y = 0; y < height; y++) {
                for (lengthType x = 0; x < width; x++) {
                    cb(ctx, idx++, x, y, z);
                }
            }
        }
    }
};

} // namespace mm
