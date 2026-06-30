#pragma once

#include "light/layers/Layer.h"
#include "core/color.h"
#include "platform/platform.h"

#include <cstring>

namespace mm {

class FireEffect : public EffectBase {
public:
    const char* tags() const override { return "⚡️🦅"; }  // FastLED origin (Fire2012-style) · David Jupijn / Rising Step
    // Iterates y and x only; Layer::extrude fills z on 3D layers. The heat
    // buffer covers only the z=0 plane (w*h), not the full 3D buffer.
    Dim dimensions() const override { return Dim::D2; }

    uint8_t cooling = 55;
    uint8_t sparking = 120;
    uint8_t hue_shift = 0;

    void onBuildControls() override {
        controls_.addUint8("cooling", cooling, 1, 255);
        controls_.addUint8("sparking", sparking, 1, 255);
        controls_.addUint8("hue_shift", hue_shift, 0, 255);
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

        // 4. Render heat to RGB via fire palette (with optional hue rotation)
        for (nrOfLightsType i = 0; i < heatCount_; i++) {
            RGB c = heatToRgb(heat_[i], hue_shift);
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

    // Heat 0-255 mapped to black -> red -> yellow -> white.
    // hue_shift rotates the resulting RGB around the colour wheel (via HSV).
    static RGB heatToRgb(uint8_t heat, uint8_t hue_shift) {
        uint8_t r, g, b;
        if (heat < 85) {
            r = static_cast<uint8_t>(heat * 3);
            g = 0;
            b = 0;
        } else if (heat < 170) {
            r = 255;
            g = static_cast<uint8_t>((heat - 85) * 3);
            b = 0;
        } else {
            r = 255;
            g = 255;
            b = static_cast<uint8_t>((heat - 170) * 3);
        }
        if (hue_shift == 0) return {r, g, b};
        // Cheap hue rotation: re-encode via HSV using max-channel as value
        uint8_t maxc = r > g ? (r > b ? r : b) : (g > b ? g : b);
        if (maxc == 0) return {0, 0, 0};
        // Approximate hue from RGB by walking the existing fire ramp range
        // and rotating it by hue_shift. Simpler: just rotate via hsvToRgb with
        // heat as the hue input.
        return hsvToRgb(static_cast<uint8_t>(heat + hue_shift), 255, maxc);
    }
};

} // namespace mm
