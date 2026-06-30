#pragma once

#include "light/layers/Layer.h"
#include "light/Palette.h"        // colorFromPalette, fadeToBlackBy
#include "light/draw.h"           // draw::line
#include "core/math8.h"           // beatsin8
#include "core/AudioModule.h"     // AudioModule::latestFrame()
#include "core/AudioFrame.h"

namespace mm {

// Audio-reactive "paintbrush": a set of lines whose endpoints oscillate in 3D on the beat, each
// line's length driven by an audio band's magnitude. The field fades a little each frame so the
// moving lines leave soft brush strokes rather than redrawing cleanly. A line only draws when it's
// longer than `minLength`, so quiet bands stay dark and loud ones sweep across the volume.
//
// Prior art: MoonLight's PaintBrush (E_MoonModules, MoonModules) — the beat-driven oscillating
// endpoints + per-band length + fading trail reproduced, written fresh on EffectBase + beatsin8 +
// draw::line + the audio frame. Reads AudioModule::latestFrame(); silence → no lines → fades to
// dark, safe on any target. (The 'soft' anti-alias control is omitted — draw::line is crisp.)
class PaintBrushEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🌙📊"; }  // MoonLight origin · MoonModules · audio

    uint8_t oscillator = 4;     // phase-modulation multiplier (0..16; spreads the lines out)
    uint8_t numLines   = 16;    // parallel animated lines (2..255)
    uint8_t fadeRate   = 64;    // background decay per frame (0..128; higher = shorter strokes)
    uint8_t minLength  = 8;     // a line draws only if longer than this (suppresses quiet bands)
    bool    colorChaos = false; // per-line hue variation vs a per-band gradient
    bool    phaseChaos = false; // random per-frame phase jitter

    void onBuildControls() override {
        controls_.addUint8("oscillator", oscillator, 0, 16);
        controls_.addUint8("numLines", numLines, 2, 255);
        controls_.addUint8("fadeRate", fadeRate, 0, 128);
        controls_.addUint8("minLength", minLength, 0, 255);
        controls_.addBool("colorChaos", colorChaos);
        controls_.addBool("phaseChaos", phaseChaos);
    }

    void loop() override {
        uint8_t* buf = buffer();
        const lengthType w = width(), h = height(), d = depth();
        const uint8_t cpl = channelsPerLight();
        if (w == 0 || h == 0 || cpl < 3) return;

        // Fade the whole field toward black so the lines leave a decaying trail.
        const size_t total = static_cast<size_t>(w) * h * d * cpl;
        for (size_t off = 0; off + 2 < total; off += cpl) {
            RGB c{buf[off + 0], buf[off + 1], buf[off + 2]};
            fadeToBlackBy(c, fadeRate);
            buf[off + 0] = c.r; buf[off + 1] = c.g; buf[off + 2] = c.b;
        }

        hue_++;
        if (phaseChaos) chaos_ = rng_.next8();
        else chaos_ = 0;

        Buffer& bufRef = layer()->buffer();
        const Coord3D dims{w, h, d};
        const AudioFrame* f = AudioModule::latestFrame();
        const uint32_t ms = elapsed();
        const uint8_t n = numLines < 2 ? 2 : numLines;

        for (uint8_t i = 0; i < n; i++) {
            const uint8_t band = static_cast<uint8_t>(static_cast<uint16_t>(i) * 16 / n);
            const uint8_t mag = f->bands[band];
            // Each endpoint oscillates on the beat; the per-line index + oscillator multiplier
            // offsets the phase so the lines fan out, and chaos jitters it per frame.
            const uint8_t ph = static_cast<uint8_t>(i * oscillator + chaos_);
            const lengthType x1 = axis(beatsin8(static_cast<uint8_t>(12 + (i & 7)), ms, 0, 255), w, ph);
            const lengthType y1 = axis(beatsin8(static_cast<uint8_t>(15 + (i & 7)), ms, 0, 255), h, static_cast<uint8_t>(ph + 64));
            const lengthType z1 = d > 1 ? axis(beatsin8(static_cast<uint8_t>(9 + (i & 7)), ms, 0, 255), d, static_cast<uint8_t>(ph + 128)) : 0;
            const lengthType x2 = axis(beatsin8(static_cast<uint8_t>(13 + (i & 7)), ms, 0, 255), w, static_cast<uint8_t>(ph + 96));
            const lengthType y2 = axis(beatsin8(static_cast<uint8_t>(11 + (i & 7)), ms, 0, 255), h, static_cast<uint8_t>(ph + 160));
            const lengthType z2 = d > 1 ? axis(beatsin8(static_cast<uint8_t>(7 + (i & 7)), ms, 0, 255), d, static_cast<uint8_t>(ph + 200)) : 0;

            // Length gate: the line's span scaled by the band magnitude. Quiet bands (small mag)
            // fall below minLength and don't draw.
            const lengthType dx = x2 > x1 ? static_cast<lengthType>(x2 - x1) : static_cast<lengthType>(x1 - x2);
            const lengthType dy = y2 > y1 ? static_cast<lengthType>(y2 - y1) : static_cast<lengthType>(y1 - y2);
            const lengthType dz = z2 > z1 ? static_cast<lengthType>(z2 - z1) : static_cast<lengthType>(z1 - z2);
            const uint16_t span = static_cast<uint16_t>(dx + dy + dz);
            const uint16_t scaledLen = static_cast<uint16_t>(span * mag / 255);
            if (scaledLen < minLength) continue;

            const uint8_t hue = colorChaos
                ? static_cast<uint8_t>(i * 255 / n + hue_)
                : static_cast<uint8_t>(band * 16);
            const RGB c = colorFromPalette(*Palettes::active(), hue, mag);
            draw::line(bufRef, dims, {x1, y1, z1}, {x2, y2, z2}, c);
        }
    }

private:
    uint16_t hue_ = 0;
    uint8_t  chaos_ = 0;
    Random8  rng_{0xB17EB00Bu};

    // Map a 0..255 oscillator value (offset by `phase`) onto an axis of `extent` cells.
    static lengthType axis(uint8_t v, lengthType extent, uint8_t phase) {
        const uint8_t s = static_cast<uint8_t>(v + phase);
        return extent > 0 ? static_cast<lengthType>(static_cast<uint16_t>(s) * extent / 256) : 0;
    }
};

} // namespace mm
