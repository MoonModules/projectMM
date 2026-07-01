#pragma once

#include "light/effects/EffectBase.h"
#include "light/layers/Layer.h"   // layer()->buffer()
#include "light/Palette.h"        // colorFromPalette, Palettes::active()
#include "light/draw.h"           // draw::pixel, draw::fade
#include "core/math8.h"           // (no beat/trig here; included for parity with the effect family)
#include "core/AudioModule.h"     // AudioModule::latestFrame()
#include "core/AudioFrame.h"      // AudioFrame::bands[16]
#include "platform/platform.h"    // platform::alloc / platform::free (per-column peak-fall state)

namespace mm {

// GEQ: the classic flat 2D graphic equaliser. The 16 audio bands are spread across the columns of a
// 2D panel; each column rises from the bottom to a bar height set by its band's loudness, and a peak
// "dot" sits at the highest the bar has recently reached and falls back down slowly — the recognisable
// WLED "GEQ" look (distinct from the 3D-perspective GEQ3D effect in this folder).
//
// Per frame the whole buffer fades a little (fadeOut → motion trail), then for each column x its band
// is read, optionally smoothed against its neighbours (smoothBars), mapped to a bar height, and the
// column is filled from the floor up. The bar colour is either per-column (colorBars) or per-row (the
// gradient runs up the bar). A per-column peak tracker remembers the tallest the bar reached; when the
// live bar is shorter, the remembered peak is drawn as a single dot and decays downward at a rate set
// by `ripple` (0 = the peak dot is disabled; otherwise it falls one row every `ripple` frames).
//
// Prior art: WLED's "GEQ" / 2D GEQ (mode_2DGEQ, Aircoookie / Andrew Tuline lineage), carried into
// MoonLight as the GEQ effect. The band→column mapping, the 7·band + 3·prev + 3·next smoothing weights,
// the bottom-up bar fill, the colorBars / smoothBars toggles, and the falling-peak dot are reproduced
// here, written fresh on projectMM's EffectBase + the shared draw / palette primitives. Reads
// AudioModule::latestFrame(); silence → bars flat → peaks fall away → dark, safe on any target and grid
// size. The per-column peak-fall state lives on the heap (sized to width()), allocated in onBuildState
// and freed in teardown — never a large inline member.
class GEQEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🐙📊"; }  // MoonLight origin · 2D · audio
    Dim dimensions() const override { return Dim::D2; }      // writes only the z=0 slice; extrude fills z

    // Defaults match the WLED/MoonLight GEQ.
    uint8_t fadeOut    = 248;   // per-frame fade-to-black amount (motion trail). WLED maps its 0..255
                                // "fade" slider straight onto fadeToBlackBy; the GEQ default is a fast
                                // fade so bars snap rather than smear.
    uint8_t ripple     = 4;     // peak-dot fall rate: the dot drops one row every `ripple` frames
                                // (0 = no peak dot). WLED's "ripple" slider gates the falling peak.
    bool    colorBars  = false; // colour each bar by its column (true) instead of by row height (false)
    bool    smoothBars = false; // blend each band with its neighbours for a smoother profile

    void onBuildControls() override {
        controls_.addUint8("fadeOut", fadeOut, 0, 255);
        controls_.addUint8("ripple", ripple, 0, 255);
        controls_.addBool("colorBars", colorBars);
        controls_.addBool("smoothBars", smoothBars);
    }

    // One peak tracker per column: previousBarHeight[width]. WLED stores this in the segment's data
    // block, sized to the column count and zero-initialised; matched here with a heap allocation that
    // re-sizes only when the column count changes, zeroed on (re)build so a grid/control change starts
    // every peak at the floor. Entries are lengthType (the row-count type) so a panel taller than 255
    // rows doesn't truncate the remembered peak height.
    void onBuildState() override {
        const size_t cols = static_cast<size_t>(width() > 0 ? width() : 0);
        if (enabled() && cols > 0) {
            if (cols != peakCount_) {
                releasePeaks();
                peaks_ = static_cast<lengthType*>(platform::alloc(cols * sizeof(lengthType)));
                if (peaks_) peakCount_ = cols;
            }
            if (peaks_) for (size_t i = 0; i < peakCount_; i++) peaks_[i] = 0;  // zero-init, like WLED
        } else {
            releasePeaks();
        }
        rippleCounter_ = 0;
        setDynamicBytes(peakCount_ * sizeof(lengthType));
        MoonModule::onBuildState();
    }

    void teardown() override {
        releasePeaks();
        setDynamicBytes(0);
    }

    ~GEQEffect() override { releasePeaks(); }

    void loop() override {
        const int cols = width();
        const int rows = height();
        if (cols <= 0 || rows <= 0 || channelsPerLight() < 3) return;
        if (!peaks_) return;   // build hasn't allocated yet (e.g. disabled) — nothing to draw

        const AudioFrame* f = AudioModule::latestFrame();
        if (!f) return;   // null-safe (latestFrame returns silence, never null, but guard regardless)

        Buffer& buf = layer()->buffer();
        const Coord3D dims{static_cast<lengthType>(cols), static_cast<lengthType>(rows), depthDim()};

        // Motion trail: dim the whole buffer each frame (WLED: fadeToBlackBy(fadeOut)).
        layer()->fadeToBlackBy(fadeOut);

        // Advance the peak-fall clock once per frame. The remembered peaks drop one row whenever the
        // counter wraps `ripple`; ripple == 0 disables the dot entirely (handled at draw time).
        bool fallThisFrame = false;
        if (ripple > 0) {
            if (++rippleCounter_ >= ripple) { rippleCounter_ = 0; fallThisFrame = true; }
        }

        for (int x = 0; x < cols; x++) {
            // Map this column onto one of the 16 GEQ bands (band = map(x, 0, cols-1, 0, 15)). The
            // 0..cols-1 / 0..15 form (vs the spec's literal map(x,0,size.x,0,16)) is the real WLED
            // mode_2DGEQ shape and keeps the last column on band 15 rather than an out-of-range 16.
            int band = imap(x, 0, cols - 1, 0, NUM_GEQ_CHANNELS - 1);
            if (band < 0) band = 0;
            if (band > NUM_GEQ_CHANNELS - 1) band = NUM_GEQ_CHANNELS - 1;

            int bandHeight = f->bands[band];

            // smoothBars: weighted blend with the neighbouring bands so the bar profile is less spiky.
            // WLED weights: (7·band + 3·prev + 3·next) / 12, only for interior bands. RECONSTRUCTED from
            // WLED's mode_2DGEQ smoothing (the fetched source did not include the exact body; the 7/3/3
            // weights and /12 divisor are the WLED constants).
            if (smoothBars && band > 0 && band < NUM_GEQ_CHANNELS - 1) {
                const int lastBandHeight = f->bands[band - 1];
                const int nextBandHeight = f->bands[band + 1];
                bandHeight = (7 * bandHeight + 3 * lastBandHeight + 3 * nextBandHeight) / 12;
                if (bandHeight < 0)   bandHeight = 0;
                if (bandHeight > 255) bandHeight = 255;
            }

            // Bar height in rows: map the 0..255 band magnitude onto 0..rows.
            int barHeight = imap(bandHeight, 0, 255, 0, rows);
            if (barHeight < 0)    barHeight = 0;
            if (barHeight > rows) barHeight = rows;

            // Per-column peak: rise instantly to a new high, otherwise fall slowly. peaks_[x] is the
            // row count (0..rows) the dot currently sits at, measured from the floor.
            if (barHeight > peaks_[x]) {
                peaks_[x] = static_cast<lengthType>(barHeight);
            } else if (fallThisFrame && peaks_[x] > 0) {
                peaks_[x] = static_cast<lengthType>(peaks_[x] - 1);   // RECONSTRUCTED: WLED's peak decays one row per ripple tick
            }

            // Fill the bar from the floor (row rows-1) up to barHeight rows.
            for (int h = 0; h < barHeight; h++) {
                const int y = rows - 1 - h;          // row 0 = top, so the bar grows upward from the floor
                if (y < 0) break;
                // colorBars: one hue per column. else: the gradient runs up the bar by row height.
                const uint8_t colorIndex = colorBars
                    ? static_cast<uint8_t>(imap(x, 0, cols - 1, 0, 255))
                    : static_cast<uint8_t>(imap(h, 0, rows - 1, 0, 255));
                const RGB col = colorFromPalette(*Palettes::active(), colorIndex);
                draw::pixel(buf, dims, {static_cast<lengthType>(x), static_cast<lengthType>(y), 0}, col);
            }

            // Falling peak dot, drawn at the remembered peak row if it stands above the live bar.
            // RECONSTRUCTED: WLED draws a single peak pixel (white-ish / palette top) at previousBarHeight.
            if (ripple > 0 && peaks_[x] > 0 && peaks_[x] > barHeight) {
                const int y = rows - peaks_[x];      // peaks_[x] rows up from the floor
                if (y >= 0 && y < rows) {
                    // Peak colour: top of the palette (index 255) so the dot reads as the crest.
                    const RGB peakCol = colorFromPalette(*Palettes::active(), 255);
                    draw::pixel(buf, dims, {static_cast<lengthType>(x), static_cast<lengthType>(y), 0}, peakCol);
                }
            }
        }
    }

private:
    static constexpr int NUM_GEQ_CHANNELS = 16;

    // Standard integer map (WLED/MoonLight's ::map), used for the band/colour/height remaps. Guards a
    // zero input span so a degenerate grid (cols/rows <= 1) can't divide by zero.
    static int imap(int v, int inLo, int inHi, int outLo, int outHi) {
        const int den = inHi - inLo;
        if (den == 0) return outLo;
        return (v - inLo) * (outHi - outLo) / den + outLo;
    }

    lengthType depthDim() const { return depth() > 0 ? depth() : 1; }

    void releasePeaks() {
        if (peaks_) {
            platform::free(peaks_);
            peaks_ = nullptr;
        }
        peakCount_ = 0;
    }

    lengthType* peaks_ = nullptr;  // previousBarHeight[width]: per-column peak-dot row (0..rows from floor)
    size_t   peakCount_ = 0;       // number of peak entries allocated (== width)
    uint8_t  rippleCounter_ = 0;   // counts frames toward the next peak-fall step (gated by `ripple`)
};

} // namespace mm