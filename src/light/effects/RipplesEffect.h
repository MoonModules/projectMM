#pragma once

#include "light/layers/Layer.h"
#include "core/color.h"

#include <cmath>
#include <cstring>

namespace mm {

// 3D dancing sine-wave ripples — a reimplementation of MoonLight's Ripples
// against our architecture (studied, not copied; see docs/history).
//
// For each (x, z) column on the floor plane, the distance from the centre sets a
// wave phase; one pixel per column is lit at the height
//   y = floor(h/2 * (1 + sin(dist / interval + time)))
// so the lit surface ripples like water filling the 3D volume. The hue cycles
// over time and position (a palette substitute). Genuinely 3D: it writes a
// height across the y-axis, so it needs real depth — on a 2D grid (depth 1) it
// degenerates to a single y-row, which is honest for a flat layout.
//
// Float trig in the loop matches the existing wave effects (Plasma, LavaLamp);
// the hot-path integer-math preference is for per-light colour work, not the
// handful of transcendental ops a wave front needs.
// Author: MoonLight — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h
class RipplesEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🟦🦅"; }  // MoonLight origin · water-ripple
    Dim dimensions() const override { return Dim::D3; }

    uint8_t speed = 50;     // 0 = stopped, 99 = fast
    uint8_t interval = 128; // wavefront spacing: low = tight rings, high = wide

    void onBuildControls() override {
        controls_.addUint8("speed", speed, 0, 99);
        controls_.addUint8("interval", interval, 1, 254);
    }

    void loop() override {
        uint8_t* buf = buffer();
        const lengthType w = width();
        const lengthType h = height();
        const lengthType d = depth();
        const uint8_t cpl = channelsPerLight();
        if (w <= 0 || h <= 0 || d <= 0) return;

        // Clear: every column lights at most one y, so the rest must be black.
        std::memset(buf, 0, static_cast<size_t>(nrOfLights()) * cpl);

        // Wavefront spacing, scaled to the layout height so the look is grid-size
        // independent. Guard against a degenerate (near-zero) spacing.
        const float rippleInterval = 1.3f * ((255.0f - static_cast<float>(interval)) / 128.0f)
                                     * std::sqrt(static_cast<float>(h));
        if (rippleInterval < 0.01f) return;

        // Animate. speed 99 → fast, 0 → frozen. elapsed() is ms since effect start.
        const float timeInterval = static_cast<float>(elapsed())
                                  / (100.0f - static_cast<float>(speed)) / 6.4f;

        const float cx = static_cast<float>(w - 1) / 2.0f;
        const float cz = static_cast<float>(d - 1) / 2.0f;
        const nrOfLightsType wh = static_cast<nrOfLightsType>(w) * h;

        for (lengthType z = 0; z < d; z++) {
            for (lengthType x = 0; x < w; x++) {
                const float dx = static_cast<float>(x) - cx;
                const float dz = static_cast<float>(z) - cz;
                // Distance from the floor-plane centre, scaled to height.
                const float dist = std::sqrt(dx * dx + dz * dz) / 9.899495f * static_cast<float>(h);
                const float phase = dist / rippleInterval + timeInterval;
                const lengthType y = static_cast<lengthType>(
                    std::floor(static_cast<float>(h) / 2.0f * (1.0f + std::sin(phase))));
                if (y < 0 || y >= h) continue;

                const uint8_t hue = static_cast<uint8_t>(
                    elapsed() / 50u + static_cast<uint32_t>(x) * 3u + static_cast<uint32_t>(z) * 7u);
                const RGB c = hsvToRgb(hue, 255, 255);

                // Buffer layout is (z * h * w + y * w + x) — see NoiseEffect.
                const nrOfLightsType idx = static_cast<nrOfLightsType>(z) * wh
                                         + static_cast<nrOfLightsType>(y) * w + x;
                uint8_t* px = buf + static_cast<size_t>(idx) * cpl;
                if (cpl >= 1) px[0] = c.r;
                if (cpl >= 2) px[1] = c.g;
                if (cpl >= 3) px[2] = c.b;
            }
        }
    }
};

} // namespace mm
