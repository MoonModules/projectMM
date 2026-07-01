#pragma once

#include "light/effects/EffectBase.h"
#include "light/layers/Layer.h"   // layer()->buffer()
#include "light/Palette.h"        // colorFromPalette, Palettes::active()
#include "light/draw.h"           // draw::pixel, draw::fade
#include "core/math8.h"           // Random8

#include <cmath>                  // sinf, cosf, sqrtf — per-frame origin + per-pixel distance (float kept for fidelity)

namespace mm {

// SphereMove: a hollow sphere whose surface shell sweeps through the 3D volume. Each frame the
// buffer is fully cleared, a sphere origin is driven on a Lissajous-like path (sin on x, cos on
// y and z) and a slowly breathing diameter is computed; every voxel whose Euclidean distance from
// the origin falls within the one-unit-thick shell (d > diameter && d < diameter+1) is lit from
// the active palette, the palette index drifting with time plus a small per-pixel random jitter so
// the shell shimmers. The origin sweep speeds up with `speed` (the divisor 100-speed shrinks).
//
// This is float-per-pixel (a sqrtf distance per voxel) — kept deliberately for exact visual
// fidelity with the original, as the contract allows when the source is float per-pixel.
//
// Prior art: MoonLight's SphereMove (E_MoonModules / MoonModules). The origin oscillator math
// (millis()/(100-speed)/6.4, the sin/cos origin path), the diameter = 2 + sin(ti/3) breathing,
// the one-unit shell test, and the millis()/50 + random8(64) palette index are reproduced.
// time_interval is computed in full float (ms and 100-speed as float through the whole
// expression) so the origin sweep and diameter breathing integrate continuously rather than in
// quantised integer-time steps — a smooth sweep at every speed.
class SphereMoveEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🧊"; }  // MoonLight origin · 3D-native
    Dim dimensions() const override { return Dim::D3; }

    uint8_t speed = 50;  // origin sweep rate (0..99); higher = faster (divisor is 100-speed)

    void onBuildControls() override {
        controls_.addUint8("speed", speed, 0, 99);
    }

    void loop() override {
        const int w = width();
        const int h = height();
        const int d = depth();
        if (w <= 0 || h <= 0 || d <= 0 || channelsPerLight() < 3) return;

        Buffer& buf = layer()->buffer();
        const Coord3D dims{static_cast<lengthType>(w), static_cast<lengthType>(h), static_cast<lengthType>(d)};

        // Full clear each frame (source: fadeToBlackBy(255)).
        layer()->fadeToBlackBy(255);

        const uint32_t ms = elapsed();

        // Origin oscillator: time_interval = ms / (100-speed) / 6.4, all-float so time advances
        // continuously (integer-dividing ms/(100-speed) first would truncate the sub-step time and
        // quantise the sweep). The divisor 100-speed (speed clamped 0..99 → divisor 1..100, never 0)
        // and the (256-128)/20.0 == 6.4 factor are MoonLight's exact constants.
        const float time_interval = static_cast<float>(ms) / static_cast<float>(100 - speed) / 6.4f;

        const float ox = w / 2.0f * (1.0f + sinf(time_interval));
        const float oy = h / 2.0f * (1.0f + cosf(time_interval));
        const float oz = d / 2.0f * (1.0f + cosf(time_interval));

        const float diameter = 2.0f + sinf(time_interval / 3.0f);

        // Palette index base drifts with time; a per-pixel random jitter (0..63) is added per lit voxel.
        const uint8_t indexBase = static_cast<uint8_t>(ms / 50);

        for (int z = 0; z < d; z++) {
            const float dz = z - oz;
            for (int y = 0; y < h; y++) {
                const float dy = y - oy;
                for (int x = 0; x < w; x++) {
                    const float dx = x - ox;
                    const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (dist > diameter && dist < diameter + 1.0f) {
                        const uint8_t index = static_cast<uint8_t>(indexBase + rng_.below(64));
                        draw::pixel(buf, dims, {static_cast<lengthType>(x), static_cast<lengthType>(y), static_cast<lengthType>(z)},
                                    colorFromPalette(*Palettes::active(), index));
                    }
                }
            }
        }
    }

private:
    Random8 rng_;  // per-pixel palette jitter (MoonLight's random8(64))
};

} // namespace mm
