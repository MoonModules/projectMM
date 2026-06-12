#pragma once

#include "light/layers/Layer.h"
#include "core/color.h"        // hsvToRgb, RGB
#include "core/AudioModule.h"    // AudioModule::latestFrame()

#include <cstring>             // std::memset

namespace mm {

// Audio-reactive spectrum analyser: the classic equalizer display. The 16
// frequency bands (AudioFrame::bands, bass→treble) spread across the grid's X
// axis, each column lit from the bottom up in proportion to its band's magnitude.
// The 16 bands scale to any width: a 32-wide grid gives each band two columns, an
// 8-wide grid samples every other band, a 16-wide grid is one column per band.
// On a grid at least 3 rows tall, the bottom row is an overall level/volume meter
// (a horizontal VU bar) and the bars sit above it; shorter grids use the full
// height for the spectrum.
//
// Reads the live frame from AudioModule::latestFrame(); no mic / silence → all
// bands zero → dark, so it is safe on any target and any grid size (including
// 0×0). On a 1D strip (height 1) the bars collapse to per-column brightness.
class AudioSpectrumEffect : public EffectBase {
public:
    const char* tags() const override { return "📊"; }

    // 0 = height gradient (green base → red top, the VU look); 1 = per-band hue
    // (each column its own colour across the spectrum, the rainbow analyser look).
    uint8_t colorMode = 0;

    void onBuildControls() override {
        static constexpr const char* kColorOptions[] = {"height", "per-band"};
        controls_.addSelect("colorMode", colorMode, kColorOptions, 2);
    }

    void loop() override {
        uint8_t* buf = buffer();
        const lengthType w = width();
        const lengthType h = height();
        const lengthType d = depth();
        const uint8_t cpl = channelsPerLight();
        if (w == 0 || h == 0 || d == 0 || cpl == 0) return;

        std::memset(buf, 0, static_cast<size_t>(w) * h * d * cpl);

        const AudioFrame* f = AudioModule::latestFrame();

        auto setRGB = [&](lengthType x, lengthType y, lengthType z,
                          uint8_t r, uint8_t g, uint8_t b) {
            size_t off = (static_cast<size_t>(z) * h * w
                         + static_cast<size_t>(y) * w + x) * cpl;
            if (cpl >= 1) buf[off + 0] = r;
            if (cpl >= 2) buf[off + 1] = g;
            if (cpl >= 3) buf[off + 2] = b;
        };

        // On a grid tall enough (h >= 3), reserve the BOTTOM row for an overall
        // level/volume meter — a horizontal bar lit left-to-right in proportion to
        // `level` — and draw the spectrum bars in the rows ABOVE it. On a short
        // grid (h < 3) there's no room to spare, so the whole height is spectrum.
        const bool levelRow = (h >= 3);
        const lengthType specH = levelRow ? static_cast<lengthType>(h - 1) : h;

        if (levelRow) {
            const lengthType y = static_cast<lengthType>(h - 1);   // bottom row
            const lengthType litW = static_cast<lengthType>(
                static_cast<uint32_t>(f->level > 255 ? 255 : f->level) * w / 255u);
            for (lengthType x = 0; x < litW; x++) {
                // Green → red across the width, the VU-meter look.
                const uint8_t frac = static_cast<uint8_t>(
                    static_cast<uint32_t>(x) * 255u / (w > 1 ? w : 1));
                for (lengthType z = 0; z < d; z++)
                    setRGB(x, y, z, frac, static_cast<uint8_t>(255 - frac), 0);
            }
        }

        for (lengthType x = 0; x < w; x++) {
            // Map this column onto one of the 16 bands (scales to any width).
            const uint8_t band = static_cast<uint8_t>(
                static_cast<uint32_t>(x) * 16u / static_cast<uint32_t>(w));
            const uint8_t mag = f->bands[band];   // 0..255

            // Bar height over the SPECTRUM area (above the level row): magnitude →
            // lit rows from the bottom of that area up. h==1 lights the one row.
            const lengthType lit = (h == 1)
                ? 1
                : static_cast<lengthType>(static_cast<uint32_t>(mag) * specH / 255u);

            // Per-band hue: spread the 16 bands across the full colour wheel so
            // each column is a distinct colour (bass red → treble violet).
            const uint8_t bandHue = static_cast<uint8_t>(band * 16);

            // Spectrum bars sit ABOVE the level row: their bottom is row h-2 when a
            // level row is reserved, else h-1 (the grid bottom).
            const lengthType specBottom = static_cast<lengthType>(levelRow ? h - 2 : h - 1);
            for (lengthType row = 0; row < lit; row++) {
                const lengthType y = static_cast<lengthType>(specBottom - row);

                uint8_t r, g, b;
                if (colorMode == 1) {
                    // Per-band: the column's hue at full brightness (a strip dims
                    // its single row by magnitude instead).
                    const uint8_t v = (h == 1) ? mag : 255;
                    const RGB c = hsvToRgb(bandHue, 255, v);
                    r = c.r; g = c.g; b = c.b;
                } else {
                    // Height gradient: green at the base → red at the top. The
                    // gradient runs over the spectrum's own rows (specH), not the
                    // full grid height: when the bottom row is reserved as the level
                    // meter, specH == h-1, so dividing by (h-1) would stop the top
                    // spectrum row one step short of full red. On a 1D strip
                    // (specH <= 1) there's no height to gradient over, so the one
                    // row is a green→red ramp driven by magnitude (dark at mag 0).
                    const uint8_t frac = (specH > 1)
                        ? static_cast<uint8_t>(static_cast<uint32_t>(row) * 255u / (specH - 1))
                        : mag;
                    r = (specH > 1) ? frac : static_cast<uint8_t>(static_cast<uint32_t>(frac) * mag / 255u);
                    g = (specH > 1) ? static_cast<uint8_t>(255 - frac)
                                    : static_cast<uint8_t>(static_cast<uint32_t>(255 - frac) * mag / 255u);
                    b = 0;
                }
                for (lengthType z = 0; z < d; z++)
                    setRGB(x, y, z, r, g, b);
            }
        }
    }
};

} // namespace mm
