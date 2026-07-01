#pragma once

#include "light/effects/EffectBase.h"
#include "light/layers/Layer.h"   // layer()->buffer()
#include "light/Palette.h"        // colorFromPalette, Palettes::active()
#include "light/draw.h"           // draw::pixel, draw::fade
#include "core/math8.h"           // beatsin8
#include "core/noise.h"           // inoise8 (2-arg 2D field)
#include "core/AudioModule.h"     // AudioModule::latestFrame()
#include "core/AudioFrame.h"      // AudioFrame::level

namespace mm {

// Noise Meter: a vertical VU column whose height tracks the overall sound level and whose colour is a
// scrolling 2D value-noise field, so a loud moment fills the panel from the bottom up with a drifting,
// organic gradient instead of a flat bar. Each frame the buffer fades a little (motion trail), the
// audio level (scaled by `width`) sets how many rows light up from the bottom, and for each lit row a
// noise sample — taken from a field that both scrolls (the aux0/aux1 phase accumulators) and is
// modulated by the live level — picks the palette colour. The colour depends only on the row (y), so
// the effect writes the x=0 column and Layer::extrude fans each row across every x and z — the meter
// reads as one wide block of light without the effect duplicating the broadcast itself (that is the
// framework's job; see architecture.md § Dimensionality).
//
// Prior art: WLED's "Noisemeter" sound-reactive effect (Andrew Tuline / WLED-SR). The fadeRate/width
// knobs, the level→length mapping, the inoise8(row·level + aux0, aux1 + row·level) field sampling, and
// the bottom-up fill are reproduced here, written fresh on projectMM's EffectBase + the shared draw /
// palette / noise / beatsin8 primitives. Reads AudioModule::latestFrame(); silence → level 0 →
// maxLen 0 → the panel fades to dark, safe on any target and grid size.
class NoiseMeterEffect : public EffectBase {
public:
    const char* tags() const override { return "🐙📊"; }   // WLED origin · audio
    Dim dimensions() const override { return Dim::D1; }    // writes the x=0 column; extrude fans x and z

    // Defaults match WLED's Noisemeter exactly.
    uint8_t fadeRate = 240;   // per-frame fade-to-black amount (motion trail), range 200..254
    uint8_t width    = 128;   // level→length gain: how much of the column a given level fills (0..255)

    void onBuildControls() override {
        controls_.addUint8("fadeRate", fadeRate, 200, 254);
        controls_.addUint8("width", width, 0, 255);
    }

    void loop() override {
        const int sizeX = width_();
        const int sizeY = height();
        const int sizeZ = depthDim();
        if (sizeX <= 0 || sizeY <= 0 || channelsPerLight() < 3) return;

        const AudioFrame* f = AudioModule::latestFrame();
        if (!f) return;   // null-safe (latestFrame returns silence, never null, but guard regardless)

        Buffer& buf = layer()->buffer();
        const Coord3D dims{static_cast<lengthType>(sizeX), static_cast<lengthType>(sizeY),
                           static_cast<lengthType>(sizeZ)};

        layer()->fadeToBlackBy(fadeRate);

        // Level scaled by `width` into a 0..255 length proxy, then mapped onto the column height.
        // tmpSound2 = level * 2 * width / 255. WLED uses volumeRaw here (the instantaneous, un-smoothed
        // level) so the meter snaps to transients; projectMM's f->level IS that raw value (computeLevel
        // recomputes it per block with no smoothing — see AudioFrame::level), so this is faithful.
        const uint16_t level = f->level;
        uint32_t tmpSound2 = (static_cast<uint32_t>(level) * 2u * width) / 255u;
        if (tmpSound2 > 255u) tmpSound2 = 255u;   // tmpSound2 feeds map(0..255 → 0..sizeY); cap the
                                                  // 0..255 byte WLED's map() takes (the maxLen
                                                  // constrain below lands on sizeY either way)

        // maxLen = map(tmpSound2, 0, 255, 0, sizeY); constrain 0..sizeY.
        int maxLen = static_cast<int>((tmpSound2 * static_cast<uint32_t>(sizeY)) / 255u);
        if (maxLen < 0) maxLen = 0;
        if (maxLen > sizeY) maxLen = sizeY;

        for (int y = 0; y < maxLen; y++) {
            // Scrolling, level-modulated 2D noise field. The two coordinates are 16.8 fixed (our
            // inoise8 treats the high byte as the cell), exactly as WLED feeds inoise8: the row index
            // times the live level walks one axis, the aux phase the other, so the colour drifts both
            // with motion (aux) and with loudness (level).
            const uint32_t coordA = static_cast<uint32_t>(y) * level + aux0_;
            const uint32_t coordB = aux1_ + static_cast<uint32_t>(y) * level;
            const uint8_t index = inoise8(coordA, coordB);
            const RGB col = colorFromPalette(*Palettes::active(), index);

            // D1: write only the x=0 column; Layer::extrude fans this row across every x and z.
            const lengthType drawY = static_cast<lengthType>(sizeY - 1 - y);
            draw::pixel(buf, dims, {0, drawY, 0}, col);
        }

        // Scroll the noise field each frame. aux0 and aux1 are advanced by two slow, slightly-detuned
        // oscillators (bpm 5 and 4) so the noise field weaves rather than scrolling at a constant rate.
        aux0_ += beatsin8(5, elapsed(), 0, 10);
        aux1_ += beatsin8(4, elapsed(), 0, 10);
    }

private:
    lengthType depthDim() const { return depth() > 0 ? depth() : 1; }
    // The inherited EffectBase::width() (grid width) is shadowed by the `width` control member, so
    // reach the grid width through a thin alias.
    lengthType width_() const { return EffectBase::width(); }

    // Two scrolling-noise phase accumulators (WLED's SEGENV.aux0 / aux1). Scalar state, tiny — stays
    // inline (the "no large inline members" rule targets per-light buffers sized to nrOfLights).
    uint16_t aux0_ = 0;
    uint16_t aux1_ = 0;
};

} // namespace mm
