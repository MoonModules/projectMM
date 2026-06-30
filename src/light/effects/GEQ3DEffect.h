#pragma once

#include "light/layers/Layer.h"
#include "light/Palette.h"        // colorFromPalette, blend
#include "light/draw.h"           // draw::line — the perspective edges
#include "core/AudioModule.h"     // AudioModule::latestFrame()
#include "core/AudioFrame.h"

#include <cstring>

namespace mm {

// A 3D-perspective graphic equaliser: the 16 audio bands rise as bars on a 2D grid, drawn with a
// faked depth — each bar has darker side edges and a top surface whose lines converge toward a
// "projector" (vanishing point) that sweeps left and right across the display. Bands to the left
// of the projector are drawn right-to-left, bands to the right left-to-right, so the perspective
// always points away from the moving vanishing point.
//
// Prior art: MoonLight's GEQ3D (E_MoonModules, MoonModules) — the perspective-bar look + sweeping
// projector reproduced, written fresh on EffectBase + draw::line + the audio frame. Reads
// AudioModule::latestFrame(); silence → flat → dark, safe on any target. (The 'softHack'
// anti-alias control is omitted — draw::line is crisp.)
class GEQ3DEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🌙📊"; }  // MoonLight origin · MoonModules · audio
    Dim dimensions() const override { return Dim::D2; }

    uint8_t speed       = 5;     // projector sweep rate (1..10; higher = faster)
    uint8_t frontFill   = 128;   // brightness of the bar's front face (0..255)
    uint8_t horizon     = 0;     // vanishing-point row offset toward the top
    uint8_t perspective = 128;   // depth strength (how far the top lines reach toward the projector)
    uint8_t numBands    = 16;    // bands shown (2..16); fewer = wider bars
    bool    borders     = false; // outline each bar

    void onBuildControls() override {
        controls_.addUint8("speed", speed, 1, 10);
        controls_.addUint8("frontFill", frontFill, 0, 255);
        controls_.addUint8("horizon", horizon, 0, 255);
        controls_.addUint8("perspective", perspective, 0, 255);
        controls_.addUint8("numBands", numBands, 2, 16);
        controls_.addBool("borders", borders);
    }

    void loop() override {
        uint8_t* buf = buffer();
        const lengthType w = width(), h = height(), d = depth();
        const uint8_t cpl = channelsPerLight();
        if (w == 0 || h == 0 || cpl < 3) return;
        std::memset(buf, 0, static_cast<size_t>(w) * h * d * cpl);

        Buffer& bufRef = layer()->buffer();
        const Coord3D dims{w, h, d};
        const AudioFrame* f = AudioModule::latestFrame();

        // Sweep the projector (vanishing point) left/right; bounce at the edges. The throttle
        // (11-speed) means a higher speed steps more often.
        if (++counter_ >= static_cast<uint32_t>(11 - speed)) {
            counter_ = 0;
            int32_t np = static_cast<int32_t>(projector_) + projectorDir_;
            if (np <= 0)         { np = 0;         projectorDir_ = 1; }
            else if (np >= w - 1){ np = w - 1;     projectorDir_ = -1; }
            projector_ = static_cast<lengthType>(np);
        }

        const uint8_t n = numBands < 2 ? 2 : (numBands > 16 ? 16 : numBands);
        const lengthType barW = w > n ? static_cast<lengthType>(w / n) : 1;
        const lengthType vy   = static_cast<lengthType>(horizon * (h - 1) / 255);   // vanishing row

        for (uint8_t b = 0; b < n; b++) {
            const uint8_t band = static_cast<uint8_t>(static_cast<uint16_t>(b) * 16 / n);
            const uint8_t mag = f->bands[band];
            const lengthType barH = static_cast<lengthType>(static_cast<uint32_t>(mag) * (h - 1) * 85 / (255 * 100));
            if (barH <= 0) continue;

            const lengthType x0 = static_cast<lengthType>(b * barW);
            const lengthType x1 = static_cast<lengthType>((b + 1) * barW - 1 < w ? (b + 1) * barW - 1 : w - 1);
            const lengthType top = static_cast<lengthType>(h - 1 - barH);
            const uint8_t hue = static_cast<uint8_t>(band * 16);
            const RGB front = colorFromPalette(*Palettes::active(), hue, frontFill);
            const RGB edge  = colorFromPalette(*Palettes::active(), hue, static_cast<uint8_t>(frontFill / 2));

            // Front face: vertical lines from the floor up to the bar height.
            for (lengthType x = x0; x <= x1; x++)
                draw::line(bufRef, dims, {x, static_cast<lengthType>(h - 1), 0}, {x, top, 0}, front);

            // Side edges (the depth illusion): the bar's left and right verticals, darker.
            draw::line(bufRef, dims, {x0, static_cast<lengthType>(h - 1), 0}, {x0, top, 0}, edge);
            draw::line(bufRef, dims, {x1, static_cast<lengthType>(h - 1), 0}, {x1, top, 0}, edge);

            // Top surface: a line from the bar's near top edge toward the projector vanishing point,
            // pulled back by `depth`. The further the projector, the more the top "tilts".
            const lengthType px = projector_;
            const lengthType ty = static_cast<lengthType>(top - (top - vy) * perspective / 255);
            draw::line(bufRef, dims, {x0, top, 0}, {px > x0 ? x1 : x0, ty, 0}, edge);

            if (borders)
                draw::line(bufRef, dims, {x0, top, 0}, {x1, top, 0}, front);
        }
    }

private:
    lengthType projector_ = 0;
    int8_t     projectorDir_ = 1;
    uint32_t   counter_ = 0;
};

} // namespace mm
