#pragma once

#include "light/layers/Layer.h"
#include "light/Palette.h"   // colorFromPalette, Palettes::active — the blob field → palette colour
#include "core/color.h"
#include "core/math8.h"   // sin8/cos8/dist8/atan2_8

namespace mm {

// Atmospheric lava-lamp: three slow blobs whose summed field is mapped
// through a black → red → orange → yellow → white palette.
// Distinct from MetaballsEffect (which is fast, HSV-coloured).
// Author: projectMM original (metaball lava lamp)
class LavaLampEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🦅"; }  // MoonLight origin · David Jupijn / Rising Step
    // Iterates y and x only; Layer::extrude fills z on 3D layers.
    Dim dimensions() const override { return Dim::D2; }

    static constexpr uint8_t NUM_BLOBS = 3;

    uint8_t bpm = 8;
    uint8_t radius = 36;
    uint8_t intensity = 200;

    void onBuildControls() override {
        controls_.addUint8("bpm", bpm, 1, 255);
        controls_.addUint8("radius", radius, 8, 255);
        controls_.addUint8("intensity", intensity, 1, 255);
    }

    void loop() override {
        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();
        if (w <= 0 || h <= 0) return;

        uint32_t now = elapsed();
        uint32_t dt = now - lastElapsed_;
        lastElapsed_ = now;
        // Accumulate the raw (dt * bpm) product; divide only at the read site.
        // Per-tick `dt*bpm*256/60000` rounds to 0 on desktop (dt ≈ 0..1ms) and
        // freezes the animation; see MetaballsEffect for the same fix.
        phase_num_ += static_cast<uint64_t>(dt) * bpm;
        uint8_t t = static_cast<uint8_t>((phase_num_ * 256) / 60000);

        int16_t bx[NUM_BLOBS] = {};
        int16_t by[NUM_BLOBS] = {};
        static constexpr uint8_t SPEED_MUL[NUM_BLOBS] = { 1, 2, 1 };
        static constexpr uint8_t PHASE_X[NUM_BLOBS]   = { 0, 80, 160 };
        static constexpr uint8_t PHASE_Y[NUM_BLOBS]   = { 64, 200, 100 };
        for (uint8_t b = 0; b < NUM_BLOBS; b++) {
            uint8_t tb = static_cast<uint8_t>(t * SPEED_MUL[b]);
            bx[b] = static_cast<int16_t>((sin8(static_cast<uint8_t>(tb + PHASE_X[b])) * w) >> 8);
            by[b] = static_cast<int16_t>((sin8(static_cast<uint8_t>(tb + PHASE_Y[b])) * h) >> 8);
        }
        int32_t r2 = static_cast<int32_t>(radius) * radius;

        for (lengthType y = 0; y < h; y++) {
            uint8_t* row = buf + static_cast<size_t>(y) * static_cast<size_t>(w) * cpl;
            for (lengthType x = 0; x < w; x++) {
                uint32_t field = 0;
                for (uint8_t b = 0; b < NUM_BLOBS; b++) {
                    int32_t dx = static_cast<int32_t>(x) - bx[b];
                    int32_t dy = static_cast<int32_t>(y) - by[b];
                    int32_t d2 = dx * dx + dy * dy + 1;
                    field += static_cast<uint32_t>((r2 * 64) / d2);
                }
                uint32_t scaled = (field * intensity) >> 8;
                uint8_t idx = scaled > 255 ? 255 : static_cast<uint8_t>(scaled);
                // The metaball field value (0 = between blobs, 255 = blob core) is the palette index,
                // so the lamp takes the active palette. Lava gives the classic molten look (its low
                // end is black, so the space between blobs stays dark); any palette recolours the blobs.
                const RGB c = colorFromPalette(*Palettes::active(), idx);
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
