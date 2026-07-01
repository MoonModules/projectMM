#pragma once

#include <limits>
#include <cstdint>
#include "light/layouts/Layouts.h"
#include "light/light_types.h"  // lengthType, nrOfLightsType

// Prior art: MoonLight SingleColumnLayout (MoonModules/MoonLight, layout nodes).
// A vertical line of lights at a fixed x, running along y. Geometry reproduced
// exactly: MoonLight emits addLight(Coord3D(xposition, y, 0)) for y over
// [start_y, start_y + height), forward or reversed. MoonLight's pin/wiring
// controls (ledPinDIO select, the LED-pin text menu, nextPin()) are dropped —
// projectMM layouts emit coordinates only; the driver owns pins.

namespace mm {

// Author: MoonLight — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h
class SingleColumnLayout : public LayoutBase {
public:
    // Geometry controls mirror MoonLight's defaults and ranges 1:1.
    uint8_t  start_y = 0;          // "starting Y", 0..255
    uint16_t height = 30;          // "height", 1..1000
    uint16_t xposition = 0;        // "X position", 0..255
    bool     reversed_order = false;  // "reversed order"

    void onBuildControls() override {
        controls_.addUint8("starting Y", start_y, 0, 255);
        controls_.addUint16("height", height, 1, 1000);
        controls_.addUint16("X position", xposition, 0, 255);
        controls_.addBool("reversed order", reversed_order);
    }

    nrOfLightsType lightCount() const override {
        // One light per y step. Clamp to the index type max, as GridLayout does.
        constexpr uint32_t kMax = std::numeric_limits<nrOfLightsType>::max();
        const uint32_t n = static_cast<uint32_t>(height);
        return static_cast<nrOfLightsType>(n > kMax ? kMax : n);
    }

    void forEachCoord(CoordCallback cb, void* ctx) const override {
        // Emit the column in wiring order. The COORDINATE is (xposition, y, 0);
        // reversed_order walks y from the high end down, matching MoonLight's
        // addLight order. Stop at the clamped lightCount() so emitted indices
        // stay within the allocated buffer.
        const uint32_t limit = lightCount();
        const lengthType x = static_cast<lengthType>(xposition);
        uint32_t idx = 0;
        if (reversed_order) {
            for (int32_t y = static_cast<int32_t>(start_y) + static_cast<int32_t>(height) - 1;
                 y >= static_cast<int32_t>(start_y) && idx < limit; y--) {
                cb(ctx, static_cast<nrOfLightsType>(idx++), x, static_cast<lengthType>(y), 0);
            }
        } else {
            for (int32_t y = static_cast<int32_t>(start_y);
                 y < static_cast<int32_t>(start_y) + static_cast<int32_t>(height) && idx < limit; y++) {
                cb(ctx, static_cast<nrOfLightsType>(idx++), x, static_cast<lengthType>(y), 0);
            }
        }
    }
};

} // namespace mm