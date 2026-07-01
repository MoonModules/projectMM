#pragma once

#include "light/effects/EffectBase.h"
#include "light/layers/Layer.h"   // layer()->buffer()
#include "light/Palette.h"        // colorFromPalette, Palettes::active()
#include "light/draw.h"           // draw::pixel, draw::fade, draw::fill, draw::blur
#include "core/math8.h"           // Random8
#include "core/AudioModule.h"     // AudioModule::latestFrame()
#include "core/AudioFrame.h"      // AudioFrame::bands[16], peakHz

#include <cmath>                  // log10f, roundf (once-per-frame freqMap, not per-light)

namespace mm {

// Blurz — an audio-reactive "blurred dot" effect. Each frame it lights ONE pixel coloured by the
// current frequency band's magnitude, then blurs the whole strip, so the dot bleeds into a soft
// glowing smear that drifts and fades. A `freqBand` cursor advances one band per frame (0..15,
// wrapping), so over 16 frames the colour cycles through the whole spectrum. Where the lit pixel
// lands is the lever:
//   - freqMap on:  the dot's position maps to the dominant frequency (majorPeak) — bass at one end,
//                  treble at the other, so the spectrum scrolls spatially with pitch.
//   - geqScanner:  the dot scans steadily across the strip (a sweeping cursor).
//   - default:     the dot jumps to a random position each frame (WLED's classic Blurz).
// fadeRate dims the trail each frame; blur is the box-blur strength applied after the dot is drawn.
//
// Prior art: WLED's "Blurz" audio effect (Andrew Tuline / WLED SR), carried into MoonLight. The
// per-band colour cursor, the frequency→position map, and the fade-then-blur pipeline are reproduced
// here, written fresh on EffectBase + the shared draw primitives. Reads AudioModule::latestFrame();
// silence → bands all 0 → the strip fades to black, safe on any target and grid size.
//
// 1D in spirit (a strip of `nrOfLights` pixels) but declared D2 so it spans a 2D panel as a flat run
// along the buffer's pixel index; the dot and blur work over the whole pixel count either way.
class BlurzEffect : public EffectBase {
public:
    const char* tags() const override { return "🐙📊"; }  // WLED-lineage · audio
    Dim dimensions() const override { return Dim::D2; }

    // MoonLight/WLED defaults.
    uint8_t fadeRate   = 16;     // per-frame fade-to-black strength (1..255)
    uint8_t blur       = 128;    // box-blur strength applied after drawing the dot (1..255)
    bool    freqMap    = false;  // position the dot by dominant frequency instead of scanning/random
    bool    geqScanner = false;  // steady sweep across the strip (vs. random jump) when freqMap is off

    void onBuildControls() override {
        controls_.addUint8("fadeRate", fadeRate, 1, 255);
        controls_.addUint8("blur", blur, 1, 255);
        controls_.addBool("freqMap", freqMap);
        controls_.addBool("geqScanner", geqScanner);
    }

    // WLED clears the segment once on the first call (SEGENV.call == 0 → fadeToBlackBy(255), a full
    // wipe to black). A grid rebuild (resize / re-add) restarts the effect, so reset the one-shot
    // clear + the scanner/band cursors here so the dot starts from a known state on any reconfig.
    void onBuildState() override {
        firstFrame_ = true;
        freqBand_   = 0;
        scanPos_    = 0;
        MoonModule::onBuildState();
    }

    void loop() override {
        const int cols = width();
        const int rows = height();
        if (cols <= 0 || rows <= 0 || channelsPerLight() < 3) return;

        Buffer& buf = layer()->buffer();
        const Coord3D dims{static_cast<lengthType>(cols), static_cast<lengthType>(rows), depthDim()};

        // The strip is the flat run of all pixels; the dot is addressed by a single linear index.
        const int maxLen = static_cast<int>(nrOfLights());
        if (maxLen <= 0) return;

        // One-shot clear on the first frame after (re)build — WLED's SEGENV.call == 0 fadeToBlackBy(255).
        if (firstFrame_) { draw::fill(buf, RGB{0, 0, 0}); firstFrame_ = false; }

        // Per-frame fade gives the blurred dot its decaying trail (WLED fadeToBlackBy(fadeRate)).
        draw::fade(buf, fadeRate);

        const AudioFrame* f = AudioModule::latestFrame();
        if (!f) return;

        // Advance the band cursor: one band per frame, wrapping 0..15 (NUM_GEQ_CHANNELS).
        freqBand_ = static_cast<uint8_t>((freqBand_ + 1) % 16);

        // Decide where the dot lands this frame.
        int segLoc;
        if (freqMap && f->peakHz > 0) {
            // Map the dominant frequency to a position. WLED uses a log10 placement between ~1.78
            // (10^1.78 ≈ 60 Hz, just under the 80 Hz floor) and MAX_FREQ_LOG10 = log10(11025) ≈ 4.0424.
            // RECONSTRUCTED: the BlurzEffect source in the fetch is incomplete; this freqMap placement
            // is the documented WLED-SR mapping (round((log10(majorPeak) - 1.78) * maxLen / (MAX_FREQ_LOG10 - 1.78))).
            constexpr float kMaxFreqLog10 = 4.0424f;   // log10(11025)
            constexpr float kLoLog10      = 1.78f;     // ~60 Hz, just below the 80 Hz mic floor
            const float lp = log10f(static_cast<float>(f->peakHz));
            const int freqLocn = static_cast<int>(
                roundf((lp - kLoLog10) * static_cast<float>(maxLen) / (kMaxFreqLog10 - kLoLog10)));
            segLoc = freqLocn;
        } else if (geqScanner) {
            // RECONSTRUCTED: a steady sweep across the strip (one pixel per frame, wrapping). The doc
            // describes a scanning cursor for geqScanner; this is the textbook scanning-position form.
            segLoc = scanPos_;
            scanPos_ = static_cast<int>((scanPos_ + 1) % maxLen);
        } else {
            // WLED's classic Blurz: the dot jumps to a random position each frame (random16(SEGLEN)).
            segLoc = static_cast<int>(rng_.next16() % static_cast<uint16_t>(maxLen));
        }

        // Clamp the position into [0, maxLen-1] (the freqMap path can land out of range):
        // segLoc = max(0, min(nrOfLights-1, segLoc)).
        if (segLoc < 0) segLoc = 0;
        if (segLoc > maxLen - 1) segLoc = maxLen - 1;

        // Colour the dot by the current band's magnitude, scaled across the strip the way WLED's
        // Blurz does: pixColor = (2 * fftResult[band] * 240) / max(1, maxLen - 1). WLED passes this
        // straight to ColorFromPalette, whose index is a uint8_t — so a value above 255 WRAPS around
        // the palette wheel (mod 256), it does NOT clamp. We reproduce that by truncating to uint8_t.
        const int denom = (maxLen - 1) > 1 ? (maxLen - 1) : 1;   // max(1, maxLen-1)
        const int pixColor = (2 * static_cast<int>(f->bands[freqBand_]) * 240) / denom;
        const RGB c = colorFromPalette(*Palettes::active(), static_cast<uint8_t>(pixColor));

        // Address the linear dot index as an (x,y) on the flat run: x = idx % cols, y = idx / cols.
        const int dx = segLoc % cols;
        const int dy = segLoc / cols;
        draw::pixel(buf, dims, {static_cast<lengthType>(dx), static_cast<lengthType>(dy), 0}, c);

        // Blur the whole buffer — the defining smear (WLED SEGMENT.blur(custom1)).
        draw::blur(buf, dims, blur);
    }

private:
    lengthType depthDim() const { return depth() > 0 ? depth() : 1; }

    bool    firstFrame_ = true;   // WLED SEGENV.call == 0 one-shot clear
    uint8_t freqBand_   = 0;      // band cursor, 0..15, ++ each frame
    int     scanPos_    = 0;      // geqScanner sweep position
    Random8 rng_;                 // random dot position (WLED random16(SEGLEN))
};

} // namespace mm