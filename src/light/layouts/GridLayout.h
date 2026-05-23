#pragma once

#include "light/layouts/LayoutGroup.h"

namespace mm {

class GridLayout : public LayoutBase {
public:
    lengthType width = defaultGridSize;
    lengthType height = defaultGridSize;
    lengthType depth = 1;

    void onBuildControls() override {
        controls_.addInt16("width", width);
        controls_.addInt16("height", height);
        controls_.addInt16("depth", depth);
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
