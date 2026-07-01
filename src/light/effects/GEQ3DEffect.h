#pragma once

#include "light/effects/EffectBase.h"
#include "light/layers/Layer.h"   // layer()->buffer()
#include "light/Palette.h"        // colorFromPalette, blend, Palettes::active()
#include "light/draw.h"           // draw::line (perspective edges + depth shorten), draw::fade
#include "core/AudioModule.h"     // AudioModule::latestFrame()
#include "core/AudioFrame.h"      // AudioFrame::bands[16]
#include "core/math8.h"           // map8

#include <cmath>                  // lroundf (once-per-frame maxHeight, not per-light)

namespace mm {

// GEQ 3D: a 3D-perspective graphic equaliser. The 16 audio bands rise as bars on a 2D grid, drawn
// with faked depth. Each bar's side faces and top surface are drawn as lines that run FROM a bar
// pixel TOWARD a sweeping "projector" vanishing point at (projector, horizon), each line shortened
// by `depth` so it stops partway — that converging foreshortening is the defining look. The
// projector sweeps left↔right; bands left of it are painted right-to-left, bands right of it
// left-to-right, so the perspective always points away from the moving vanishing point. The bar
// front faces are filled flat (frontFill), and an optional border outlines each bar.
//
// Prior art: MoonLight's GEQ3D (E_MoonModules / MoonModules, TroyHacks), itself descended from the
// WLED-MM "GEQ 3D" effect. The perspective-bar geometry, the projector split, the per-face
// darkening, and the `depth` line-shorten are reproduced exactly here, written fresh on EffectBase
// + the shared draw primitives. Reads AudioModule::latestFrame(); silence → flat → dark, safe on
// any target and grid size. (MoonLight's `softHack` anti-alias toggle is dropped — draw::line is a
// crisp Bresenham; the `soft` arg has no projectMM equivalent.)
// Author: @TroyHacks (MoonModules, GPLv3) — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonModules.h
class GEQ3DEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🌙📊"; }  // MoonLight origin · MoonModules · audio
    Dim dimensions() const override { return Dim::D2; }

    uint8_t speed     = 2;     // projector sweep rate (1..10; higher = faster). Time-based (BPM), so
                               // the sweep is at the same wall-clock position on every device — a
                               // fast board just renders it more smoothly, a slow board choppier.
    uint8_t frontFill = 228;   // bar front-face fill strength (0..255)
    uint8_t horizon   = 0;     // vanishing-point Y row (0..rows-1); the projector sits at this row
    uint8_t depth     = 176;   // perspective depth: how far the side/top lines reach toward the projector
    uint8_t numBands  = 16;    // bands shown (2..16); fewer = wider bars
    bool    borders   = true;  // outline each bar

    void onBuildControls() override {
        controls_.addUint8("speed", speed, 1, 10);
        controls_.addUint8("frontFill", frontFill, 0, 255);
        // MoonLight's horizon range is 0..size.x-1 (set at runtime). The control descriptor here is a
        // fixed 0..255 slider — the source's row index — and the value is clamped to the live row
        // count in loop(). A width/height-relative descriptor range isn't expressible at build time.
        controls_.addUint8("horizon", horizon, 0, 255);
        controls_.addUint8("depth", depth, 0, 255);
        controls_.addUint8("numBands", numBands, 2, 16);
        controls_.addBool("borders", borders);
    }

    void loop() override {
        if (numBands == 0) return;

        const int cols = width();
        const int rows = height();
        if (cols <= 0 || rows <= 0 || channelsPerLight() < 3) return;

        Buffer& buf = layer()->buffer();
        const Coord3D dims{static_cast<lengthType>(cols), static_cast<lengthType>(rows), depthDim()};

        // Motion trail: dim the whole buffer each frame instead of clearing it (source:
        // layer->fadeToBlackBy(16) per frame).
        layer()->fadeToBlackBy(16);

        // Projector (vanishing point) position along X, as a TIME-BASED triangle wave: it sweeps
        // 0→cols→0 driven by elapsed(), so at any wall-clock instant every device shows the projector
        // at the same place — a fast board renders the sweep more smoothly, a slow one choppier, but
        // neither is throttled to the other's pace. (MoonLight advanced it by a per-frame counter, so
        // its sweep ran faster on a high-FPS device; this is the frame-rate-independent improvement.)
        // triwave8(beat8(bpm)) is the textbook time triangle: beat8 ramps 0..255 at `bpm`, triwave8
        // folds it into an up-then-down 0..255, scaled to the column span. speed 1..10 → ~3..30 BPM.
        const uint8_t bpm = static_cast<uint8_t>(speed * 3);
        const uint8_t sweep = triwave8(beat8(bpm, elapsed()));   // 0..255 triangle over time
        // Clamp the drawn band count to the column count: MoonLight's bar width is `cols / NUM_BANDS`,
        // which truncates to 0 when there are fewer columns than bands (e.g. 8 cols, 16 bands), so
        // every bar piles at x=0. Capping bands to cols keeps the width ≥ 1 so the bars spread across
        // the available width. Invisible on normal grids (cols ≥ numBands → this is a no-op); it only
        // fixes the degenerate narrow-grid case. Once per frame, off the per-pixel path.
        const int NUM_BANDS = numBands <= cols ? static_cast<int>(numBands) : cols;
        const int projector = static_cast<int>(static_cast<uint32_t>(sweep) * cols / 255u);
        // horizon is a Y row used as the vanishing point's y; clamp the 0..255 control to the grid.
        const int hzn = horizon < rows ? horizon : rows - 1;
        const int split = imap(projector, 0, cols, 0, NUM_BANDS - 1);

        const AudioFrame* f = AudioModule::latestFrame();

        // Bar heights: map each band magnitude onto maxHeight (slightly reduced on small panels).
        uint8_t heights[16] = {0};
        const int maxHeight = lroundf(float(rows) * ((rows < 18) ? 0.75f : 0.85f));
        for (int i = 0; i < NUM_BANDS; i++) {
            int band = i;
            if (NUM_BANDS < 16) band = imap(band, 0, NUM_BANDS, 0, 16);  // always use the full 16-band range
            if (band > 15) band = 15;
            heights[i] = map8(f->bands[band], 0, static_cast<uint8_t>(maxHeight));
        }

        const RGB black{0, 0, 0};

        // Right vertical faces + top — bands at/left of the split, painted LEFT to RIGHT.
        for (int i = 0; i <= split; i++) {
            const uint16_t colorIndex = imap(cols / NUM_BANDS * i, 0, cols, 0, 256);
            const RGB ledColor = colorFromPalette(*Palettes::active(), static_cast<uint8_t>(colorIndex));
            const int linex = i * (cols / NUM_BANDS);

            if (heights[i] > 1) {
                const RGB sideColor = blend(ledColor, black, static_cast<uint8_t>(255 - 32));
                const int pPos = MAXi(0, linex + (cols / NUM_BANDS) - 1);
                // Right side face: stacked perspective lines from the bar's right edge toward the projector.
                for (int y = (i < NUM_BANDS - 1) ? heights[i + 1] : 0; y <= heights[i]; y++) {
                    if (rows - y > 0)
                        draw::line(buf, dims, {static_cast<lengthType>(pPos), static_cast<lengthType>(rows - y - 1), 0},
                                   {static_cast<lengthType>(projector), static_cast<lengthType>(hzn), 0}, sideColor, depth);
                }

                const RGB topColor = blend(ledColor, black, static_cast<uint8_t>(255 - 128));
                // Top surface: skip when directly under the projector (handled as a special case below).
                if (heights[i] < rows - hzn && (projector <= linex || projector >= pPos)) {
                    if (rows - heights[i] > 1) {
                        for (int x = linex; x <= pPos; x++)
                            draw::line(buf, dims, {static_cast<lengthType>(x), static_cast<lengthType>(rows - heights[i] - 2), 0},
                                       {static_cast<lengthType>(projector), static_cast<lengthType>(hzn), 0}, topColor, depth);
                    }
                }
            }
        }

        // Left vertical faces + top — bands right of the split, painted RIGHT to LEFT.
        for (int i = NUM_BANDS - 1; i > split; i--) {
            const uint16_t colorIndex = imap(cols / NUM_BANDS * i, 0, cols - 1, 0, 255);
            const RGB ledColor = colorFromPalette(*Palettes::active(), static_cast<uint8_t>(colorIndex));
            const int linex = i * (cols / NUM_BANDS);
            const int pPos = MAXi(0, linex + (cols / NUM_BANDS) - 1);

            if (heights[i] > 1) {
                const RGB sideColor = blend(ledColor, black, static_cast<uint8_t>(255 - 32));
                // Left side face: stacked perspective lines from the bar's left edge toward the projector.
                for (int y = (i > 0) ? heights[i - 1] : 0; y <= heights[i]; y++) {
                    if (rows - y > 0)
                        draw::line(buf, dims, {static_cast<lengthType>(linex), static_cast<lengthType>(rows - y - 1), 0},
                                   {static_cast<lengthType>(projector), static_cast<lengthType>(hzn), 0}, sideColor, depth);
                }

                const RGB topColor = blend(ledColor, black, static_cast<uint8_t>(255 - 128));
                if (heights[i] < rows - hzn && (projector <= linex || projector >= pPos)) {
                    if (rows - heights[i] > 1) {
                        for (int x = linex; x <= pPos; x++)
                            draw::line(buf, dims, {static_cast<lengthType>(x), static_cast<lengthType>(rows - heights[i] - 2), 0},
                                       {static_cast<lengthType>(projector), static_cast<lengthType>(hzn), 0}, topColor, depth);
                    }
                }
            }
        }

        // Projector special-case top + front fill + borders, all bands left to right.
        for (int i = 0; i < NUM_BANDS; i++) {
            const uint16_t colorIndex = imap(cols / NUM_BANDS * i, 0, cols - 1, 0, 255);
            const RGB ledColor = colorFromPalette(*Palettes::active(), static_cast<uint8_t>(colorIndex));
            const int linex = i * (cols / NUM_BANDS);
            const int pPos  = linex + (cols / NUM_BANDS) - 1;
            const int pPos1 = linex + (cols / NUM_BANDS);

            // Special case: top perspective for the bar directly under the projector (skipped above).
            if (projector >= linex && projector <= pPos) {
                if ((heights[i] > 1) && (heights[i] < rows - hzn) && (rows - heights[i] > 1)) {
                    const RGB topColor = blend(ledColor, black, static_cast<uint8_t>(255 - 128));
                    for (int x = linex; x <= pPos; x++)
                        draw::line(buf, dims, {static_cast<lengthType>(x), static_cast<lengthType>(rows - heights[i] - 2), 0},
                                   {static_cast<lengthType>(projector), static_cast<lengthType>(hzn), 0}, topColor, depth);
                }
            }

            if ((heights[i] > 1) && (rows - heights[i] > 0)) {
                RGB frontColor = blend(ledColor, black, static_cast<uint8_t>(255 - frontFill));
                // Front fill: vertical lines across the bar face from the floor up to its height.
                for (int x = linex; x < pPos1; x++)
                    draw::line(buf, dims, {static_cast<lengthType>(x), static_cast<lengthType>(rows - 1), 0},
                               {static_cast<lengthType>(x), static_cast<lengthType>(rows - heights[i] - 1), 0}, frontColor);

                if (!borders && heights[i] > rows - hzn) {
                    // Match the side fill in blackout mode, then draw a top line to simulate hidden top fill.
                    if (frontFill == 0) frontColor = blend(ledColor, black, static_cast<uint8_t>(255 - 32));
                    draw::line(buf, dims, {static_cast<lengthType>(linex), static_cast<lengthType>(rows - heights[i] - 1), 0},
                               {static_cast<lengthType>(linex + (cols / NUM_BANDS) - 1), static_cast<lengthType>(rows - heights[i] - 1), 0}, frontColor);
                }

                if (borders && (rows - heights[i] > 1)) {
                    const lengthType bottom = static_cast<lengthType>(rows - 1);
                    const lengthType topY   = static_cast<lengthType>(rows - heights[i] - 1);
                    const lengthType topY2  = static_cast<lengthType>(rows - heights[i] - 2);
                    const lengthType lx     = static_cast<lengthType>(linex);
                    const lengthType rx     = static_cast<lengthType>(linex + (cols / NUM_BANDS) - 1);
                    draw::line(buf, dims, {lx, bottom, 0}, {lx, topY, 0}, ledColor);   // left side line
                    draw::line(buf, dims, {rx, bottom, 0}, {rx, topY, 0}, ledColor);   // right side line
                    draw::line(buf, dims, {lx, topY2, 0}, {rx, topY2, 0}, ledColor);   // top line
                    draw::line(buf, dims, {lx, bottom, 0}, {rx, bottom, 0}, ledColor); // bottom line
                }
            }
        }
    }

private:
    // Standard integer map (MoonLight's ::map), used for the color index / split / band remaps.
    static int imap(int x, int inLo, int inHi, int outLo, int outHi) {
        const int den = inHi - inLo;
        if (den == 0) return outLo;
        return (x - inLo) * (outHi - outLo) / den + outLo;
    }
    static int MAXi(int a, int b) { return a > b ? a : b; }
    // The member `depth` (control) hides the inherited grid-depth accessor name; qualify it.
    lengthType depthDim() const { return EffectBase::depth() > 0 ? EffectBase::depth() : 1; }
};

} // namespace mm
