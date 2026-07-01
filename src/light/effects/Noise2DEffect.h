#pragma once

#include "light/effects/EffectBase.h"
#include "light/layers/Layer.h"   // layer()->buffer()
#include "light/Palette.h"        // colorFromPalette, Palettes::active()
#include "light/draw.h"           // draw::pixel
#include "core/noise.h"           // inoise8 (3-arg value noise)

namespace mm {

// Noise 2D: a smoothly drifting value-noise field. Each (x,y) pixel reads a 3D noise sample whose
// X/Y coordinates are the grid position scaled by `scale` (larger scale = finer, more detailed
// noise; smaller = broad, smooth blobs) and whose Z coordinate is time, so the whole field flows /
// morphs over the frames. The 0..255 noise value indexes the active palette directly, giving the
// classic organic, plasma-like colour wash.
//
// Source math (MoonLight's Noise2D): for every (x,y),
//   pixelHue8 = inoise8(x*scale, y*scale, millis()/(16-speed));
//   setRGB(x,y, ColorFromPalette(pal, pixelHue8));
// `16-speed` is the time divisor: a higher `speed` (max 15) shrinks the divisor, so time advances
// faster through the noise and the field morphs quicker. speed maxes at 15, so 16-speed is at least
// 1 — the division can never be by zero.
//
// Prior art: MoonLight's Noise2D effect (E_MoonModules / MoonModules), itself in the WLED
// noise-effect lineage (FastLED inoise8 — Perlin/value noise, Mark Kriegsman / Ken Perlin). The
// per-pixel coordinate-scale + time-on-Z animation and the direct palette indexing are reproduced
// exactly here, written fresh on EffectBase + the shared draw / noise primitives.
// Author: WLED (Noise 2D) — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h
class Noise2DEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🌙🐙"; }  // MoonLight origin · MoonModules · 2D
    Dim dimensions() const override { return Dim::D2; }

    uint8_t speed = 8;    // time-flow rate (0..15); higher = faster morph (divisor is 16-speed)
    uint8_t scale = 64;   // noise zoom (2..255); higher = finer/more-detailed field

    void onBuildControls() override {
        controls_.addUint8("speed", speed, 0, 15);
        controls_.addUint8("scale", scale, 2, 255);
    }

    void loop() override {
        const int cols = width();
        const int rows = height();
        if (cols <= 0 || rows <= 0 || channelsPerLight() < 3) return;

        Buffer& buf = layer()->buffer();
        const Coord3D dims{static_cast<lengthType>(cols), static_cast<lengthType>(rows), depthDim()};

        // Time coordinate on the noise Z axis: millis() / (16 - speed). speed <= 15 keeps the
        // divisor >= 1 (no divide-by-zero). uint32_t throughout — matches inoise8's coordinate type.
        const uint32_t t = elapsed() / static_cast<uint32_t>(16 - speed);

        for (int y = 0; y < rows; y++) {
            for (int x = 0; x < cols; x++) {
                const uint8_t pixelHue8 = inoise8(static_cast<uint32_t>(x) * scale,
                                                  static_cast<uint32_t>(y) * scale, t);
                const RGB c = colorFromPalette(*Palettes::active(), pixelHue8);
                draw::pixel(buf, dims, {static_cast<lengthType>(x), static_cast<lengthType>(y), 0}, c);
            }
        }
    }

private:
    lengthType depthDim() const { return depth() > 0 ? depth() : 1; }
};

}  // namespace mm