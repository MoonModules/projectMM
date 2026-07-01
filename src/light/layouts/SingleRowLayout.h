#pragma once

#include <cstdint>
#include "light/layouts/Layouts.h"
#include "light/light_types.h"  // lengthType, nrOfLightsType

namespace mm {

// A single horizontal row of lights: `width` lights along the x-axis starting at
// `start_x`, all sharing row `yposition` (y) at z=0. The 1D strip primitive — a
// bare LED strip laid out straight. `reversedOrder` flips the wiring direction so
// driver index 0 lands at the high-x end instead of the low-x end; the emitted
// COORDINATES are identical either way, only the index→position order changes
// (same distinction GridLayout draws for serpentine).
//
// Prior art: MoonLight's SingleRowLayout (L_MoonLight.h) — same start_x/width/
// yposition/reversed row. We drop MoonLight's per-strip pin plumbing (the
// ledPinDIO select, its "LED NN" pin menu, and nextPin()): a projectMM layout
// emits coordinates only, the driver owns pin assignment.
// Author: MoonLight — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h
class SingleRowLayout : public LayoutBase {
public:
    const char* tags() const override { return "💫"; }  // MoonLight origin

    // First x of the row. uint8_t (0..255) — MoonLight's exact type/range.
    uint8_t startX = 0;
    // Number of lights in the row. uint16_t, 1..1000 — MoonLight's exact range.
    uint16_t width = 30;
    // Row index (y). uint16_t storage, 0..255 range — MoonLight's exact bounds.
    uint16_t yPosition = 0;
    // Wire the row from the high-x end back to start_x (index 0 at x=start_x+width-1).
    bool reversedOrder = false;

    void onBuildControls() override {
        controls_.addUint8("starting X", startX, 0, 255);
        controls_.addUint16("width", width, 1, 1000);
        controls_.addUint16("Y position", yPosition, 0, 255);
        controls_.addBool("reversed order", reversedOrder);
    }

    nrOfLightsType lightCount() const override {
        // width is the whole count; it already fits nrOfLightsType (max 1000 < the
        // uint16_t/uint32_t index range), so no clamp is needed.
        return static_cast<nrOfLightsType>(width);
    }

    void forEachCoord(CoordCallback cb, void* ctx) const override {
        // The coordinate at index i is fixed at row yPosition, z=0; only the x walk
        // direction depends on reversedOrder — the two branches mirror MoonLight's
        // onLayout() forward/reverse loops exactly.
        const lengthType y = static_cast<lengthType>(yPosition);
        nrOfLightsType idx = 0;
        if (reversedOrder) {
            for (int32_t x = static_cast<int32_t>(startX) + width - 1;
                 x >= static_cast<int32_t>(startX); x--) {
                cb(ctx, idx++, static_cast<lengthType>(x), y, 0);
            }
        } else {
            for (int32_t x = static_cast<int32_t>(startX);
                 x < static_cast<int32_t>(startX) + width; x++) {
                cb(ctx, idx++, static_cast<lengthType>(x), y, 0);
            }
        }
    }
};

} // namespace mm