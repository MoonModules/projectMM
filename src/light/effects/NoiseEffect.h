#pragma once

#include "core/math16.h"            // BeatPhase — the shared BPM accumulator
#include "light/effects/EffectBase.h"

namespace mm {

// Author: FastLED inoise field (Mark Kriegsman)
/// Smooth animated gradient-noise effect (3D on volumetric layouts).
/// @card NoiseEffect.gif
class NoiseEffect : public EffectBase {
public:
    const char* tags() const override { return "⚡️"; }  // FastLED-style noise
    Dim dimensions() const override { return Dim::D3; }

    uint8_t scale = 4;  // spatial frequency (1-32)
    uint8_t bpm = 60;   // beats per minute — scrolls 8 noise cells per beat

    void defineControls() override {
        controls_.addControl("scale", scale, 1, 32);
        controls_.addControl("bpm", bpm, 1, 255);
    }

    void tick() MM_NONBLOCKING override {
        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        lengthType d = depth();
        uint8_t cpl = channelsPerLight();
        nrOfLightsType count = nrOfLights();
        nrOfLightsType wh = static_cast<nrOfLightsType>(w) * h;

        // Accumulate phase incrementally — changing BPM doesn't cause a jump. phase_ holds the RAW
        // numerator (dt·bpm·w·64) and the /60000 divide runs only at the sampling point below; dividing
        // per tick would truncate sub-unit progress to zero on a fast board (short dt) or small grid,
        // stalling the field. Factor 64 tuned so 60 BPM at 128-wide gives smooth motion.
        // The rate carries the grid width, so it is fed pre-scaled: advanceScaled takes the whole
        // dt multiplicand rather than a plain BPM. BeatPhase owns the first-tick seed (the field
        // starts at phase 0 rather than jumping by the device uptime) and the divide-late rule.
        phase_.advanceScaled(elapsed(), static_cast<uint64_t>(bpm) * w * 64);
        uint32_t t = phase_.phase(1);

        // Buffer layout is (z * h * w + y * w + x). For a 2D grid (d == 1) z
        // is always 0 and we sample 2D noise — no perf cost vs. the old 2D-only
        // path. For d > 1 we sample 3D noise so each z-slice differs.
        for (nrOfLightsType i = 0; i < count; i++) {
            nrOfLightsType rem = i % wh;
            lengthType x = static_cast<lengthType>(rem % w);
            lengthType y = static_cast<lengthType>(rem / w);
            lengthType z = static_cast<lengthType>(i / wh);

            // Scale coords into noise space: a finer `scale` packs more cells across the grid,
            // and the time offset scrolls each axis at a slightly different rate so the field
            // flows rather than slides flat. inoise8's high byte selects the cell, low byte the
            // position within it.
            const uint32_t nx = (static_cast<uint32_t>(x) * 256u + t)        / scale;
            const uint32_t ny = (static_cast<uint32_t>(y) * 256u + t / 3u)   / scale;
            const uint8_t n = (d > 1)
                ? inoise8(nx, ny, (static_cast<uint32_t>(z) * 256u + t / 5u) / scale)
                : inoise8(nx, ny);
            RGB c = colorFromPalette(*Palettes::active(), n);

            size_t offset = static_cast<size_t>(i) * cpl;
            if (cpl >= 1) buf[offset + 0] = c.r;
            if (cpl >= 2) buf[offset + 1] = c.g;
            if (cpl >= 3) buf[offset + 2] = c.b;
        }
    }

private:
    BeatPhase phase_;
    // The gradient-noise field itself (lattice hash, gradient dot products, quintic fade, bi- or
    // trilinear blend) is the shared inoise8 in core/noise.h: this effect just scales coordinates into it and colors the
    // result through the palette.
};

} // namespace mm
