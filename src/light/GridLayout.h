#pragma once

#include "light/LayoutGroup.h"

namespace mm {

class GridLayout : public LayoutBase {
public:
    lengthType width = defaultGridSize;
    lengthType height = defaultGridSize;
    lengthType depth = 1;

    void onBuildControls() override {
        controls_.addUint16("width", reinterpret_cast<uint16_t&>(width));
        controls_.addUint16("height", reinterpret_cast<uint16_t&>(height));
        controls_.addUint16("depth", reinterpret_cast<uint16_t&>(depth));
    }

    nrOfLightsType lightCount() const override {
        return static_cast<nrOfLightsType>(width) * height * depth;
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
