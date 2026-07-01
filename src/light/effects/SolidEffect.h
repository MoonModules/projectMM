#pragma once

#include "light/effects/EffectBase.h"
#include "light/layers/Layer.h"   // layer()->buffer()
#include "light/Palette.h"        // colorFromPalette, Palettes::active()
#include "light/draw.h"           // draw::pixel, draw::fill
#include "core/math8.h"           // (scale8 lives in color.h, pulled in via draw.h)
#include "platform/platform.h"    // alloc/free — heap validIndices table

#include <cmath>                  // sqrtf — RMS palette average (cold, once per frame)

namespace mm {

// Solid colour fill with five colour modes: a flat RGB(W) colour, the active palette laid across
// the lights, an RMS-averaged single palette colour, or the palette banded along the rows / columns
// of the grid. A brightness scales the flat and palette-spread results. In the two band modes a
// `minRGB` floor drops near-black palette entries, and `randomColors` shuffles the surviving entries
// with a fixed LCG so the bands re-order deterministically. The R/G/B/white members are the flat-
// colour source; in the palette modes they're unused.
//
// Prior art: MoonLight's Solid effect (E_MoonModules / MoonModules). The five colour modes, the
// brightness scaling, the RMS palette average (skip black, sqrt of the mean of squares), the
// minRGB valid-entry filter, and the deterministic LCG shuffle (seed 12345, *25173 +13849) are
// reproduced exactly here, written fresh on EffectBase + the shared palette/draw primitives.
// MoonLight iterates a 256-entry palette; projectMM's palette is a 16-entry gradient read through
// colorFromPalette()'s 0..255 wheel index, so the 256 wheel positions sampled below reproduce
// MoonLight's per-entry scan one-for-one. The optional white channel is written only when the layer
// carries a 4th channel (channelsPerLight() >= 4); on RGB layers the white member is ignored.
// Author: MoonLight — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h
class SolidEffect : public EffectBase {
public:
    const char* tags() const override { return "💫"; }   // MoonLight origin
    // Modes 3/4 read y vs x and write every (x,y,z); modes 0–2 fill flat. Iterating all axes keeps
    // the band orientation correct on a 3D layer, so declare D3 (no extrude on our behalf).
    Dim dimensions() const override { return Dim::D3; }

    // Defaults match MoonLight's Solid exactly.
    uint8_t red = 182, green = 15, blue = 98, white = 0;
    uint8_t brightness = 255;
    uint8_t colorMode = 0;          // 0 RGB(W) · 1 Palette · 2 Palette avg · 3 Palette rows · 4 Palette cols
    uint8_t minRGB = 10;            // band modes: drop palette entries whose every channel < minRGB
    bool    randomColors = false;   // band modes: LCG-shuffle the surviving palette entries

    static constexpr const char* kColorModeOptions[] = {
        "RGB(W)", "Palette", "Palette avg", "Palette rows", "Palette cols"};
    static constexpr uint8_t kColorModeCount = 5;

    void onBuildControls() override {
        controls_.addUint8("red", red, 0, 255);
        controls_.addUint8("green", green, 0, 255);
        controls_.addUint8("blue", blue, 0, 255);
        controls_.addUint8("white", white, 0, 255);
        controls_.addUint8("brightness", brightness, 0, 255);
        controls_.addSelect("colorMode", colorMode, kColorModeOptions, kColorModeCount);
        controls_.addUint8("minRGB", minRGB, 0, 255);
        controls_.addBool("randomColors", randomColors);
    }

    // The band modes (3/4) need a 256-entry table of valid wheel indices. 256 bytes is small, but
    // the contract keeps per-effect buffers off the inline footprint (the registerType<T> probe
    // lives on an 8 KB stack), so it's a lazily-allocated heap buffer, freed on teardown.
    void onBuildState() override {
        if (enabled() && !validIndices_) {
            validIndices_ = static_cast<uint8_t*>(platform::alloc(256));
        } else if (!enabled() && validIndices_) {
            release();
        }
        setDynamicBytes(validIndices_ ? 256 : 0);
    }

    void teardown() override { release(); setDynamicBytes(0); }
    ~SolidEffect() override { release(); }

    void loop() override {
        const int w = width();
        const int h = height();
        const int d = depth();
        if (w <= 0 || h <= 0 || channelsPerLight() < 3) return;

        Buffer& buf = layer()->buffer();
        const lengthType dz = d > 0 ? static_cast<lengthType>(d) : 1;
        const Coord3D dims{static_cast<lengthType>(w), static_cast<lengthType>(h), dz};
        const Palette& pal = *Palettes::active();
        const uint8_t cpl = channelsPerLight();
        const nrOfLightsType nLights = nrOfLights();

        switch (colorMode) {
            case 0: {  // RGB(W): flat colour, brightness pre-applied per channel (CRGB(red*bri/255,…)).
                const RGB c{static_cast<uint8_t>(red   * brightness / 255),
                            static_cast<uint8_t>(green * brightness / 255),
                            static_cast<uint8_t>(blue  * brightness / 255)};
                draw::fill(buf, c);
                // Write W every frame (white may be 0) so a stale W from a prior frame/effect is cleared.
                // Scale W by brightness like RGB, so the whole RGBW colour dims together.
                if (cpl >= 4) writeWhite(buf, nLights, cpl, static_cast<uint8_t>(white * brightness / 255));
                break;
            }
            case 1: {  // Palette spread across the lights: light i → wheel index map(i,0,nLights,0,256).
                uint8_t* data = buf.data();
                const size_t bytes = buf.bytes();
                for (nrOfLightsType i = 0; i < nLights; i++) {
                    const uint8_t idx = static_cast<uint8_t>(mapI(static_cast<int>(i), 0, static_cast<int>(nLights), 0, 256));
                    const RGB c = colorFromPalette(pal, idx, brightness);
                    const size_t off = static_cast<size_t>(i) * cpl;
                    if (off + 3 > bytes) break;
                    data[off + 0] = c.r; data[off + 1] = c.g; data[off + 2] = c.b;
                }
                // Palette modes carry no white source: clear W so an RGBW buffer doesn't keep stale white.
                if (cpl >= 4) writeWhite(buf, nLights, cpl, 0);
                break;
            }
            case 2: {  // RMS average of the (non-black) palette colours, filled solid (no brightness — source).
                uint32_t sumR = 0, sumG = 0, sumB = 0;
                int n = 0;
                for (int i = 0; i < 256; i++) {
                    const RGB e = colorFromPalette(pal, static_cast<uint8_t>(i));
                    if (e.r == 0 && e.g == 0 && e.b == 0) continue;   // skip black
                    sumR += static_cast<uint32_t>(e.r) * e.r;
                    sumG += static_cast<uint32_t>(e.g) * e.g;
                    sumB += static_cast<uint32_t>(e.b) * e.b;
                    n++;
                }
                RGB avg{0, 0, 0};
                if (n > 0) {
                    avg.r = static_cast<uint8_t>(sqrtf(static_cast<float>(sumR) / n));
                    avg.g = static_cast<uint8_t>(sqrtf(static_cast<float>(sumG) / n));
                    avg.b = static_cast<uint8_t>(sqrtf(static_cast<float>(sumB) / n));
                }
                draw::fill(buf, avg);
                if (cpl >= 4) writeWhite(buf, nLights, cpl, 0);   // no white source: clear stale W
                break;
            }
            default: {  // 3 rows / 4 cols: band the (filtered, optionally shuffled) palette along an axis.
                const bool rows = (colorMode == 3);
                const int axisSize = rows ? h : w;

                // Collect the wheel indices whose any channel >= minRGB.
                int nrValid = 0;
                if (validIndices_) {
                    for (int i = 0; i < 256; i++) {
                        const RGB e = colorFromPalette(pal, static_cast<uint8_t>(i));
                        if (e.r >= minRGB || e.g >= minRGB || e.b >= minRGB)
                            validIndices_[nrValid++] = static_cast<uint8_t>(i);
                    }
                    if (randomColors && nrValid > 1) {
                        // Fisher-Yates with MoonLight's exact LCG (seed 12345, *25173 +13849).
                        uint32_t seed = 12345u;
                        for (int i = nrValid - 1; i > 0; i--) {
                            seed = seed * 25173u + 13849u;
                            const int j = static_cast<int>(seed % static_cast<uint32_t>(i + 1));
                            const uint8_t t = validIndices_[i];
                            validIndices_[i] = validIndices_[j];
                            validIndices_[j] = t;
                        }
                    }
                }

                for (int z = 0; z < dz; z++) {
                    for (int y = 0; y < h; y++) {
                        for (int x = 0; x < w; x++) {
                            const int axisValue = rows ? y : x;
                            uint8_t idx;
                            if (nrValid > 0 && validIndices_) {
                                const int vi = axisSize <= 1 ? 0
                                             : mapI(axisValue, 0, axisSize - 1, 0, nrValid - 1);
                                idx = validIndices_[vi];
                            } else {
                                // No surviving entry: map the axis straight onto the 0..255 wheel.
                                idx = axisSize <= 1 ? 0
                                    : static_cast<uint8_t>(mapI(axisValue, 0, axisSize - 1, 0, 255));
                            }
                            const RGB c = colorFromPalette(pal, idx, brightness);
                            draw::pixel(buf, dims, {static_cast<lengthType>(x), static_cast<lengthType>(y),
                                                    static_cast<lengthType>(z)}, c);
                        }
                    }
                }
                // Band modes carry no white source: clear W so an RGBW buffer doesn't keep stale white.
                if (cpl >= 4) writeWhite(buf, nLights, cpl, 0);
                break;
            }
        }
    }

private:
    uint8_t* validIndices_ = nullptr;

    void release() {
        if (validIndices_) { platform::free(validIndices_); validIndices_ = nullptr; }
    }

    // Write the white channel (4th) on every light. RGB stays as already filled. `w` may be 0 to
    // clear a stale white the palette modes never overwrite (draw::pixel/draw::fill touch RGB only).
    static void writeWhite(Buffer& buf, nrOfLightsType n, uint8_t cpl, uint8_t w) {
        uint8_t* data = buf.data();
        const size_t bytes = buf.bytes();
        for (nrOfLightsType i = 0; i < n; i++) {
            const size_t off = static_cast<size_t>(i) * cpl + 3;
            if (off >= bytes) break;
            data[off] = w;
        }
    }

    // FastLED-style integer map (inHi exclusive at the call sites that pass nLights/256, matching
    // MoonLight's ::map). Guards a zero span so a degenerate axis maps to outLo.
    static int mapI(int x, int inLo, int inHi, int outLo, int outHi) {
        const int den = inHi - inLo;
        if (den == 0) return outLo;
        return (x - inLo) * (outHi - outLo) / den + outLo;
    }
};

} // namespace mm
