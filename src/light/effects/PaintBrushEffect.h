#pragma once

#include "light/layers/Layer.h"
#include "light/Palette.h"        // colorFromPalette, Palettes::active
#include "light/draw.h"           // draw::line, draw::fade
#include "core/math8.h"           // beatsin8, map8, Random8
#include "core/AudioModule.h"     // AudioModule::latestFrame()
#include "core/AudioFrame.h"      // AudioFrame, bands[]

namespace mm {

// Audio-reactive "paintbrush": a set of lines whose endpoints oscillate in 3D on the beat, each
// line shortened toward its first endpoint by an audio band's magnitude so the strokes curve and
// sweep. The field fades a little each frame so the moving lines leave brush strokes rather than
// redrawing cleanly. A line only draws when it's longer than `minLength`, so quiet bands stay dark.
//
// Prior art: MoonLight's PaintBrush (@TroyHacks, E_MoonModules, MoonModules). Behaviour reproduced
// exactly — the same six oscillating endpoints, the per-band Euclidean length fed back through the
// draw-line shorten parameter (this is what makes the strokes curve), the per-frame fade and the
// length gate — written fresh on projectMM's EffectBase + the shared primitives (beatsin8, map8,
// draw::line, draw::fade, the audio frame). Reads AudioModule::latestFrame(); silence → no lines →
// fades to dark, safe on any target and any grid size. The 'soft' anti-alias control is omitted
// (the one approved omission — draw::line is crisp, projectMM has no Xiaolin-Wu line yet).
class PaintBrushEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🌙📊"; }  // MoonLight origin · MoonModules · audio
    Dim dimensions() const override { return Dim::D3; }

    uint8_t oscillatorOffset = 6 * 160 / 255;  // = 3; phase-spread multiplier (0..16)
    uint8_t numLines   = 255;                  // parallel animated lines (2..255)
    uint8_t fadeRate   = 40;                   // background decay per frame (0..128)
    uint8_t minLength  = 0;                    // a line draws only if longer than this (slider 0..255)
    bool    color_chaos = false;               // per-line hue variation vs a per-band gradient
    bool    phase_chaos = false;               // random per-frame phase jitter

    void onBuildControls() override {
        controls_.addUint8("oscillatorOffset", oscillatorOffset, 0, 16);
        controls_.addUint8("numLines", numLines, 2, 255);
        controls_.addUint8("fadeRate", fadeRate, 0, 128);
        controls_.addUint8("minLength", minLength);   // slider over the full 0..255 range
        controls_.addBool("color_chaos", color_chaos);
        controls_.addBool("phase_chaos", phase_chaos);
    }

    void loop() override {
        const lengthType cols = width(), rows = height(), depth = this->depth();
        const uint8_t cpl = channelsPerLight();
        if (cols == 0 || rows == 0 || cpl < 3) return;   // 0×0×0 and short-channel guard

        Buffer& buf = layer()->buffer();

        // Per-frame: advance the hue, then fade the whole field toward black (a decaying trail).
        aux0Hue++;
        draw::fade(buf, fadeRate);

        // Optional per-frame phase jitter shared by every endpoint this frame.
        aux1Chaos = phase_chaos ? rng_.next8() : 0;

        const Coord3D dims{cols, rows, depth};
        const AudioFrame* f = AudioModule::latestFrame();
        const uint32_t ms = elapsed();
        // bass term added to every oscillator bpm (MoonLight's bands[0]/NUM_GEQ_CHANNELS).
        const uint8_t base = static_cast<uint8_t>(f->bands[0] / kBands);

        for (size_t i = 0; i < numLines; i++) {
            const uint8_t bin = static_cast<uint8_t>(
                map(static_cast<int>(i), 0, numLines, 0, kBands - 1));
            const uint8_t band = f->bands[bin];

            // Six endpoints: each axis pair oscillates at a multiple of oscillatorOffset (+ a bass
            // term), timebased on the band magnitude, phase-jittered by aux1Chaos.
            const uint8_t x1 = beatsin8(static_cast<uint8_t>(oscillatorOffset * 1 + base), ms, 0, static_cast<uint8_t>(cols - 1), band, aux1Chaos);
            const uint8_t x2 = beatsin8(static_cast<uint8_t>(oscillatorOffset * 2 + base), ms, 0, static_cast<uint8_t>(cols - 1), band, aux1Chaos);
            const uint8_t y1 = beatsin8(static_cast<uint8_t>(oscillatorOffset * 3 + base), ms, 0, static_cast<uint8_t>(rows - 1), band, aux1Chaos);
            const uint8_t y2 = beatsin8(static_cast<uint8_t>(oscillatorOffset * 4 + base), ms, 0, static_cast<uint8_t>(rows - 1), band, aux1Chaos);

            uint8_t z1 = 0, z2 = 0;
            int length;
            if (depth > 1) {
                z1 = beatsin8(static_cast<uint8_t>(oscillatorOffset * 5 + base), ms, 0, static_cast<uint8_t>(depth - 1), band, aux1Chaos);
                z2 = beatsin8(static_cast<uint8_t>(oscillatorOffset * 6 + base), ms, 0, static_cast<uint8_t>(depth - 1), band, aux1Chaos);
                length = isqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1) + (z2 - z1) * (z2 - z1));
            } else {
                length = isqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
            }

            // The Euclidean span scaled by the band magnitude becomes the shorten amount: louder
            // bands draw a longer fraction of the line, so the strokes curve as the length pulses.
            length = map8(band, 0, static_cast<uint8_t>(length));

            if (length > MAX(1, minLength)) {
                const uint8_t index = color_chaos
                    ? static_cast<uint8_t>(i * 255 / numLines + (aux0Hue & 0xFF))
                    : static_cast<uint8_t>(map(static_cast<int>(i), 0, numLines, 0, 255));
                const RGB color = colorFromPalette(*Palettes::active(), index, 255);
                draw::line(buf, dims, {x1, y1, z1}, {x2, y2, z2}, color, static_cast<uint8_t>(length));
            }
        }
    }

private:
    // The audio frame's frequency-band count (MoonLight's NUM_GEQ_CHANNELS), tied to the array so
    // the band-index map and the bass term stay in sync with AudioFrame::bands if it ever changes.
    static constexpr int kBands = static_cast<int>(sizeof(AudioFrame::bands) / sizeof(AudioFrame::bands[0]));

    uint16_t aux0Hue = 0;    // running hue, incremented each frame
    uint8_t  aux1Chaos = 0;  // per-frame phase jitter (0 unless phase_chaos)
    Random8  rng_{0xB17EB00Bu};

    // FastLED-style MAX/map kept local so the loop reads like the MoonLight source.
    static constexpr int MAX(int a, int b) { return a > b ? a : b; }

    // Standard integer map (outlo + (i-inlo)*(outhi-outlo)/(inhi-inlo)); guards a zero input span.
    static constexpr int map(int i, int inlo, int inhi, int outlo, int outhi) {
        return inhi == inlo ? outlo : outlo + (i - inlo) * (outhi - outlo) / (inhi - inlo);
    }

    // Integer square root (binary digit-by-digit) — the true Euclidean length the source takes via
    // float sqrt(), without floats in the hot path; dist8() is an octagonal approximation, not this.
    static int isqrt(int n) {
        if (n <= 0) return 0;
        unsigned int x = static_cast<unsigned int>(n), res = 0, bit = 1u << 30;
        while (bit > x) bit >>= 2;
        while (bit != 0) {
            if (x >= res + bit) { x -= res + bit; res = (res >> 1) + bit; }
            else res >>= 1;
            bit >>= 2;
        }
        return static_cast<int>(res);
    }
};

} // namespace mm
