#pragma once

#include "light/layers/Layer.h"
#include "light/Palette.h"   // colorFromPalette + active palette
#include "core/color.h"

namespace mm {

// Expanding concentric rings from random centre points.
// Each ring grows continuously and respawns at a fresh random
// position once it leaves the visible area. Multiple rings overlap.
// (Renamed from RipplesEffect: the Ripples name now holds the MoonLight
// sine-wave water-surface port; this concentric-rings effect is Rings.)
class RingsEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🦅"; }  // MoonLight origin · David Jupijn / Rising Step
    // Iterates y and x only; Layer::extrude fills z on 3D layers.
    Dim dimensions() const override { return Dim::D2; }

    static constexpr uint8_t MAX_RIPPLES = 8;

    // Calm defaults: a couple of slow rings read as clean expanding circles; more/faster reads as
    // chaos. Raise count/speed in the UI for a busier field.
    uint8_t count = 2;
    uint8_t speed = 30;
    uint8_t thickness = 3;
    uint8_t hue_shift = 0;

    void onBuildControls() override {
        controls_.addUint8("count", count, 1, 255);
        controls_.addUint8("speed", speed, 1, 255);
        controls_.addUint8("thickness", thickness, 1, 255);
        controls_.addUint8("hue_shift", hue_shift, 0, 255);
    }

    void loop() override {
        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();
        if (w <= 0 || h <= 0) return;

        // Visible radius limit (octagonal distance to far corner)
        uint8_t maxR = dist8(static_cast<int16_t>(w), static_cast<int16_t>(h));

        if (!initialized_) {
            for (uint8_t i = 0; i < MAX_RIPPLES; i++) {
                spawn(i, w, h);
                // Stagger initial radii so ripples are spread across all sizes
                radius_[i] = static_cast<uint8_t>((i * maxR) / MAX_RIPPLES);
            }
            initialized_ = true;
        }

        uint32_t now = elapsed();
        uint32_t dt = now - lastElapsed_;
        lastElapsed_ = now;
        // growth in radius units per frame (scaled by speed control + dt)
        uint16_t growth = static_cast<uint16_t>((static_cast<uint32_t>(speed) * dt) >> 7);
        if (growth == 0) growth = 1;

        for (uint8_t i = 0; i < count && i < MAX_RIPPLES; i++) {
            uint16_t next = static_cast<uint16_t>(radius_[i]) + growth;
            if (next > maxR) {
                spawn(i, w, h);
            } else {
                radius_[i] = static_cast<uint8_t>(next);
            }
        }

        for (lengthType y = 0; y < h; y++) {
            uint8_t* row = buf + static_cast<size_t>(y) * static_cast<size_t>(w) * cpl;
            for (lengthType x = 0; x < w; x++) {
                uint16_t r_acc = 0, g_acc = 0, b_acc = 0;
                for (uint8_t i = 0; i < count && i < MAX_RIPPLES; i++) {
                    int16_t dx = static_cast<int16_t>(x - cx_[i]);
                    int16_t dy = static_cast<int16_t>(y - cy_[i]);
                    uint8_t d = dist8(dx, dy);
                    int16_t diff = static_cast<int16_t>(d) - static_cast<int16_t>(radius_[i]);
                    if (diff < 0) diff = static_cast<int16_t>(-diff);
                    if (diff < thickness) {
                        // Brightness peaks at ring centre, falls off with distance from ring.
                        uint8_t falloff = static_cast<uint8_t>(((thickness - diff) * 255) / thickness);
                        // Older ripples (large radius) fade out.
                        uint8_t age_fade = static_cast<uint8_t>(255 - ((radius_[i] * 255u) / maxR));
                        uint8_t intensity = scale8(falloff, age_fade);
                        RGB c = colorFromPalette(*Palettes::active(), static_cast<uint8_t>(hue_[i] + hue_shift), intensity);
                        r_acc = static_cast<uint16_t>(r_acc + c.r);
                        g_acc = static_cast<uint16_t>(g_acc + c.g);
                        b_acc = static_cast<uint16_t>(b_acc + c.b);
                    }
                }
                if (cpl >= 1) row[0] = r_acc > 255 ? 255 : static_cast<uint8_t>(r_acc);
                if (cpl >= 2) row[1] = g_acc > 255 ? 255 : static_cast<uint8_t>(g_acc);
                if (cpl >= 3) row[2] = b_acc > 255 ? 255 : static_cast<uint8_t>(b_acc);
                row += cpl;
            }
        }
    }

private:
    lengthType cx_[MAX_RIPPLES] = {};
    lengthType cy_[MAX_RIPPLES] = {};
    uint8_t radius_[MAX_RIPPLES] = {};
    uint8_t hue_[MAX_RIPPLES] = {};
    bool initialized_ = false;
    uint32_t lastElapsed_ = 0;
    uint32_t rngState_ = 0xC0DECAFEu;

    uint8_t rand8() {
        rngState_ = rngState_ * 1103515245u + 12345u;
        return static_cast<uint8_t>((rngState_ >> 16) & 0xFF);
    }

    void spawn(uint8_t i, lengthType w, lengthType h) {
        cx_[i] = static_cast<lengthType>((static_cast<uint16_t>(rand8()) * w) >> 8);
        cy_[i] = static_cast<lengthType>((static_cast<uint16_t>(rand8()) * h) >> 8);
        radius_[i] = 0;
        hue_[i] = rand8();
    }
};

} // namespace mm
