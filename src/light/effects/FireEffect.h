#pragma once

#include "light/layers/Layer.h"
#include "light/Palette.h"        // colorFromPalette, Palettes::active — heat → palette colour
#include "core/color.h"
#include "platform/platform.h"

#include <cstring>

namespace mm {

// Author: Mark Kriegsman's Fire2012 (FastLED); MoonLight adapts MatrixFireFast by toggledbits — https://github.com/toggledbits/MatrixFireFast
class FireEffect : public EffectBase {
public:
    const char* tags() const override { return "⚡️🦅"; }  // FastLED origin (Fire2012-style) · David Jupijn / Rising Step
    // Iterates y and x only; Layer::extrude fills z on 3D layers. The heat
    // buffer covers only the z=0 plane (w*h), not the full 3D buffer.
    Dim dimensions() const override { return Dim::D2; }

    uint8_t cooling = 55;
    uint8_t sparking = 120;

    void onBuildControls() override {
        controls_.addUint8("cooling", cooling, 1, 255);
        controls_.addUint8("sparking", sparking, 1, 255);
    }

    void onBuildState() override {
        // D2 effect: heat grid covers only the z=0 plane (w*h). Extrude fills
        // z on 3D layers. Avoids allocating depth× more heap than needed.
        nrOfLightsType count = static_cast<nrOfLightsType>(width()) * height();
        if (enabled() && count > 0) {
            if (count != heatCount_) {
                releaseHeat();
                heat_ = static_cast<uint8_t*>(platform::alloc(count));
                if (heat_) {
                    std::memset(heat_, 0, count);
                    heatCount_ = count;
                }
            }
        } else {
            releaseHeat();
        }
        setDynamicBytes(heatCount_);
    }

    void teardown() override {
        releaseHeat();
        setDynamicBytes(0);
    }

    ~FireEffect() override {
        releaseHeat();
    }

    void loop() override {
        if (!heat_) return;

        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();

        // 1. Cool every cell by a small random amount
        uint8_t coolMax = static_cast<uint8_t>((static_cast<uint16_t>(cooling) * 10) / (h > 0 ? h : 1) + 2);
        for (nrOfLightsType i = 0; i < heatCount_; i++) {
            uint8_t c = rand8() % coolMax;
            heat_[i] = (heat_[i] > c) ? static_cast<uint8_t>(heat_[i] - c) : 0;
        }

        // 2. Heat rises: each row averages from the row below (y = h-1 is "bottom", rises to y=0)
        for (lengthType y = 0; y + 1 < h; y++) {
            for (lengthType x = 0; x < w; x++) {
                lengthType yb = y + 1;
                lengthType xl = x > 0 ? x - 1 : 0;
                lengthType xr = (x + 1 < w) ? x + 1 : x;
                uint16_t sum = heat_[yb * w + xl];
                sum += heat_[yb * w + x];
                sum += heat_[yb * w + xr];
                sum += heat_[yb * w + x];
                heat_[y * w + x] = static_cast<uint8_t>(sum >> 2);
            }
        }

        // 3. Random sparks at the bottom row. Scale the number of spark attempts with width so a
        //    wide grid lights up across its whole base instead of leaving most columns cold — a
        //    fixed 4 sparks fills a 16-wide grid but barely speckles a 256-wide one. One attempt
        //    per ~4 columns (min 4) keeps the bottom row evenly seeded at any width.
        if (h > 0 && w > 0) {
            lengthType bottomRow = static_cast<lengthType>(h - 1);
            lengthType sparks = w / 4;
            if (sparks < 4) sparks = 4;
            for (lengthType i = 0; i < sparks; i++) {
                if (rand8() < sparking) {
                    // 16-bit random scaled by width: a single rand8 (8-bit) maps to only 256
                    // buckets, leaving columns >256 unreachable on a wide grid (width goes to 512).
                    const uint16_t r16 = static_cast<uint16_t>((rand8() << 8) | rand8());
                    lengthType sx = static_cast<lengthType>((static_cast<uint32_t>(r16) * w) >> 16);
                    uint8_t add = static_cast<uint8_t>(160 + (rand8() & 0x5F));
                    uint16_t newHeat = static_cast<uint16_t>(heat_[bottomRow * w + sx]) + add;
                    heat_[bottomRow * w + sx] = newHeat > 255 ? 255 : static_cast<uint8_t>(newHeat);
                }
            }
        }

        // 4. Render heat to RGB through the active palette: the heat value (0 = cold, 255 = hottest)
        //    is the palette index, so the flame takes the palette's low→high gradient. The Lava
        //    palette (black→red→orange→yellow→white) gives the classic fire look and is the intended
        //    default; any palette works (an Ocean/Forest palette makes a blue/green "fire").
        //    A completely cold cell (heat 0) always stays black — the "sky" above the flame — rather
        //    than taking the palette's index-0 colour (Lava's is black, but Ocean's is blue, which
        //    would tint the whole background). Only a warm cell is coloured.
        const Palette& pal = *Palettes::active();
        for (nrOfLightsType i = 0; i < heatCount_; i++) {
            RGB c = heat_[i] == 0 ? RGB{0, 0, 0} : colorFromPalette(pal, heat_[i]);
            size_t off = static_cast<size_t>(i) * cpl;
            if (cpl >= 1) buf[off + 0] = c.r;
            if (cpl >= 2) buf[off + 1] = c.g;
            if (cpl >= 3) buf[off + 2] = c.b;
        }
    }

private:
    uint8_t* heat_ = nullptr;
    nrOfLightsType heatCount_ = 0;
    uint32_t rngState_ = 0xC0FFEEu;

    uint8_t rand8() {
        rngState_ = rngState_ * 1103515245u + 12345u;
        return static_cast<uint8_t>((rngState_ >> 16) & 0xFF);
    }

    void releaseHeat() {
        if (heat_) {
            platform::free(heat_);
            heat_ = nullptr;
        }
        heatCount_ = 0;
    }
};

} // namespace mm
