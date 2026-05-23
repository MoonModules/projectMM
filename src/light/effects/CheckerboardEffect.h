#pragma once

#include "light/Layer.h"
#include "core/color.h"

namespace mm {

class CheckerboardEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🦅"; }  // MoonLight origin · David Jupijn / Rising Step
    // Iterates y and x only; Layer::extrude fills z on 3D layers.
    Dim dimensions() const override { return Dim::D2; }

    uint8_t cell_size = 4;
    uint8_t bpm = 60;
    uint8_t hue_a = 0;
    uint8_t hue_b = 128;

    void onBuildControls() override {
        controls_.addUint8("cell_size", cell_size, 1, 255);
        controls_.addUint8("bpm", bpm, 1, 255);
        controls_.addUint8("hue_a", hue_a, 0, 255);
        controls_.addUint8("hue_b", hue_b, 0, 255);
    }

    void loop() override {
        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();

        uint32_t now = elapsed();
        uint32_t dt = now - lastElapsed_;
        lastElapsed_ = now;
        phase_ += static_cast<uint64_t>(dt) * bpm * 256 / 60000;
        uint8_t phaseCell = static_cast<uint8_t>(phase_ >> 4);

        RGB ca = hsvToRgb(hue_a, 255, 255);
        RGB cb = hsvToRgb(hue_b, 255, 255);

        for (lengthType y = 0; y < h; y++) {
            uint8_t cy = static_cast<uint8_t>(static_cast<uint8_t>(y) / cell_size);
            uint8_t* row = buf + static_cast<size_t>(y) * static_cast<size_t>(w) * cpl;
            for (lengthType x = 0; x < w; x++) {
                uint8_t cx = static_cast<uint8_t>(static_cast<uint8_t>(x) / cell_size);
                bool on = ((cx + cy + phaseCell) & 1) != 0;
                RGB c = on ? cb : ca;

                if (cpl >= 1) row[0] = c.r;
                if (cpl >= 2) row[1] = c.g;
                if (cpl >= 3) row[2] = c.b;
                row += cpl;
            }
        }
    }

private:
    uint64_t phase_ = 0;
    uint32_t lastElapsed_ = 0;
};

} // namespace mm
