#pragma once

#include <cstdint>
#include "light/layouts/Layouts.h"
#include "light/light_types.h"   // lengthType, nrOfLightsType
#include "core/math8.h"          // sin8, cos8 — integer trig LUT

namespace mm {

// A bicycle-wheel arrangement: `spokes` straight spokes radiate from a centre hub, each
// carrying `ledsPerSpoke` LEDs spaced one unit apart from the centre outward. Spoke k
// points at angle k/spokes of a full turn; LED r on it sits at radius r+1 along that
// angle. lightCount() = spokes * ledsPerSpoke.
//
// Integer-only (same discipline as SphereLayout, even though layout iteration is a cold
// path): angles are uint8_t (256 = full turn) and the project's sin8/cos8 LUT gives the
// direction. sin8/cos8 return 0..255 centred at 128, so (val-128) is the signed
// component in [-128,127]; a radius-r offset is (r*(val-128))>>7 (÷128 → back to unit
// scale). The whole wheel is shifted by +maxRadius so every coordinate is ≥ 0 (the
// physical address space starts at 0), giving a (2R+1)-wide bounding box.
//
// Prior art: MoonLight ring/spoke layouts (L_MoonLight.h); projectMM v2 WheelLayoutModule
// (those used double cos/sin/round — this is the integer-LUT equivalent).
class WheelLayout : public LayoutBase {
public:
    uint16_t spokes = 8;         // number of spokes, 2..64
    uint16_t ledsPerSpoke = 10;  // LEDs along each spoke, 1..256

    void onBuildControls() override {
        controls_.addUint16("spokes", spokes, 2, 64);
        controls_.addUint16("ledsPerSpoke", ledsPerSpoke, 1, 256);
    }

    nrOfLightsType lightCount() const override {
        return static_cast<nrOfLightsType>(spokes) * static_cast<nrOfLightsType>(ledsPerSpoke);
    }

    void forEachCoord(CoordCallback cb, void* ctx) const override {
        const int32_t maxR = ledsPerSpoke;             // outermost radius (centre shift)
        nrOfLightsType idx = 0;
        for (uint16_t s = 0; s < spokes; s++) {
            // Spoke angle in uint8 turn units; the signed direction components.
            const uint8_t a = static_cast<uint8_t>((static_cast<uint32_t>(s) * 256u) / spokes);
            const int32_t cx = static_cast<int32_t>(cos8(a)) - 128;   // [-128,127]
            const int32_t sy = static_cast<int32_t>(sin8(a)) - 128;
            for (uint16_t r = 0; r < ledsPerSpoke; r++) {
                const int32_t radius = r + 1;          // first LED sits one step from centre
                // offset = radius * component / 128, then shift so coords are ≥ 0.
                const int32_t x = maxR + ((radius * cx) >> 7);
                const int32_t y = maxR + ((radius * sy) >> 7);
                cb(ctx, idx,
                   static_cast<lengthType>(x),
                   static_cast<lengthType>(y),
                   0);
                idx++;
            }
        }
    }
};

} // namespace mm
