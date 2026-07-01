#pragma once

#include "light/layers/Layer.h"
#include "light/Palette.h"   // colorFromPalette + the global active palette
#include "core/color.h"

namespace mm {

// Author: FastLED rainbow (Mark Kriegsman) — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_FastLED.h
class RainbowEffect : public EffectBase {
public:
    const char* tags() const override { return "💫"; }  // MoonLight origin
    // Writes only the z=0 slice — Layer::extrude duplicates it across z on
    // 3D layouts. Opt-in to that promise so the framework doesn't iterate z.
    Dim dimensions() const override { return Dim::D2; }

    uint8_t speed = 20; // BPM — one full hue cycle every 3 s; 60 (a whole rainbow per second) reads too fast

    void onBuildControls() override {
        controls_.addUint8("speed", speed, 1, 255);
    }

    void loop() override {
        // D2 effect — writes only z=0; Layer::extrude duplicates across z.
        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();

        // BPM to phase: one full hue cycle (256) per beat
        // Use 64-bit to avoid overflow (uint32 overflows after ~5.5 minutes)
        uint32_t phase = static_cast<uint32_t>(
            static_cast<uint64_t>(elapsed()) * speed * 256 / 60000
        );

        for (lengthType y = 0; y < h; y++) {
            for (lengthType x = 0; x < w; x++) {
                // Diagonal rainbow: hue varies with x + y + time
                uint8_t hue = static_cast<uint8_t>(
                    (static_cast<uint32_t>(x + y) * 256 / (w + h)) + phase
                );

                RGB c = colorFromPalette(*Palettes::active(), hue);
                size_t offset = (static_cast<size_t>(y) * w + x) * cpl;
                if (cpl >= 1) buf[offset + 0] = c.r;
                if (cpl >= 2) buf[offset + 1] = c.g;
                if (cpl >= 3) buf[offset + 2] = c.b;
            }
        }
    }
};

} // namespace mm
