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
        // Accumulate the raw (dt * bpm) product; divide only at the read site.
        // Per-tick `dt*bpm*256/60000` rounds to 0 on desktop (dt ≈ 0..1ms) and
        // freezes the animation; see MetaballsEffect for the same fix.
        phase_num_ += static_cast<uint64_t>(dt) * bpm;
        uint8_t phaseCell = static_cast<uint8_t>((phase_num_ * 16) / 60000);

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
    // Numerator-only accumulator (units of dt*bpm). See loop() for why.
    uint64_t phase_num_ = 0;
    uint32_t lastElapsed_ = 0;
};

} // namespace mm
