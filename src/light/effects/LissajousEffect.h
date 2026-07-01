#pragma once

#include "light/effects/EffectBase.h"
#include "light/layers/Layer.h"   // layer()->buffer()
#include "light/Palette.h"        // colorFromPalette, Palettes::active()
#include "light/draw.h"           // draw::pixel, draw::fade
#include "core/math8.h"           // sin8, cos8 (pure 256-step LUT)

namespace mm {

// Lissajous: traces a Lissajous figure across the 2D grid. 256 sample points sweep an
// x-oscillator (sin8) and a y-oscillator (cos8) whose relative frequencies and shared phase
// drift over time, so the closed curve continuously morphs and rotates. Each sample is mapped
// onto the grid and painted from the palette, with a per-frame fade leaving a decaying trail.
//
// `speed` advances the shared phase (a function of elapsed time), `xFrequency` sets how many
// x-oscillations occur per y-oscillation (the figure's lobe count), and `fadeRate` controls the
// trail length. The x-oscillator runs at half-phase plus i·xFrequency/64, the y-oscillator at
// half-phase plus i·2, exactly as the source. `phase` (= elapsed·speed/256) is kept wide; only
// the final sin8/cos8 argument is truncated to a uint8_t, so it wraps at 256 — that wrap over the
// 256 samples is what produces the closed figure, and it is the source's only truncation point.
//
// Prior art: MoonLight's Lissajous (E_MoonModules / MoonModules), itself the WLED "Lissajous"
// effect (Andrew Tuline / WLED). The oscillator phase math, the sin8/cos8 split, the
// 2·locn → 2·(size−1) remap, and the elapsed()/100 + i palette walk are reproduced exactly here,
// written fresh on EffectBase + the shared draw primitives.
class LissajousEffect : public EffectBase {
public:
    const char* tags() const override { return "🐙"; }  // WLED-lineage
    Dim dimensions() const override { return Dim::D2; }

    uint8_t xFrequency = 64;   // x-oscillation count per y-oscillation (the lobe count)
    uint8_t fadeRate   = 128;  // per-frame trail fade (higher = shorter trail)
    uint8_t speed      = 128;  // phase advance rate

    void onBuildControls() override {
        controls_.addUint8("xFrequency", xFrequency, 0, 255);
        controls_.addUint8("fadeRate", fadeRate, 0, 255);
        controls_.addUint8("speed", speed, 0, 255);
    }

    void loop() override {
        const int w = width();
        const int h = height();
        if (w <= 0 || h <= 0 || channelsPerLight() < 3) return;

        Buffer& buf = layer()->buffer();
        const Coord3D dims{static_cast<lengthType>(w), static_cast<lengthType>(h), depthDim()};

        // Motion trail: dim the whole buffer each frame (source: layer->fadeToBlackBy(fadeRate)).
        layer()->fadeToBlackBy(fadeRate);

        // Shared phase, advancing with elapsed time. Kept wide (16-bit) like the source; only the
        // sin8/cos8 LUT argument below is truncated to uint8_t (the mod-256 wrap), so the high bits
        // of phase survive into half-phase exactly as in MoonLight (phase = millis()*speed/256).
        const uint32_t ms = elapsed();
        const uint16_t phase = static_cast<uint16_t>(ms * speed / 256);
        const uint16_t halfPhase = static_cast<uint16_t>(phase / 2);

        for (int i = 0; i < 256; i++) {
            // x oscillator: sin8 of half-phase + i·xFrequency/64; y oscillator: cos8 of half-phase + i·2.
            // The uint8_t cast at the call is the source's truncation point (sin8/cos8 take a uint8_t).
            const uint8_t sx = sin8(static_cast<uint8_t>(halfPhase + (i * xFrequency) / 64));
            const uint8_t sy = cos8(static_cast<uint8_t>(halfPhase + i * 2));

            // Map the 0..255 oscillator value onto the grid: MoonLight's
            // (map(2·s, 0, 511, 0, 2·(size−1)) + 1) / 2. When an axis has only one cell the only
            // valid index is 0 (MoonLight's fallback of 1 assumes a 1-based extent and clips on our
            // 0-indexed grid), so a size-1 axis maps to coordinate 0.
            const int lx = (w < 2) ? 0 : (((2 * sx) * (2 * (w - 1))) / 511 + 1) / 2;
            const int ly = (h < 2) ? 0 : (((2 * sy) * (2 * (h - 1))) / 511 + 1) / 2;

            const uint8_t colorIndex = static_cast<uint8_t>(ms / 100 + i);
            draw::pixel(buf, dims, {static_cast<lengthType>(lx), static_cast<lengthType>(ly), 0},
                        colorFromPalette(*Palettes::active(), colorIndex, 255));
        }
    }

private:
    // depth() (the inherited grid-depth accessor) isn't shadowed here — this effect has no `depth`
    // member — but mirror the GEQ3D helper shape for the z extent the draw primitives expect.
    lengthType depthDim() const { return depth() > 0 ? depth() : 1; }
};

} // namespace mm
