#pragma once

#include "light/layers/Layer.h"
#include "light/Palette.h"      // colorFromPalette + the global active palette
#include "core/AudioModule.h"   // AudioModule::latestFrame()

namespace mm {

// Audio-reactive VU effect: the whole grid pulses with the microphone's sound
// level. The simplest audio consumer — one scalar (AudioFrame::level) drives a
// single brightness, a colour shifting from calm to hot as it rises. Reads the
// live frame from AudioModule::latestFrame(); with no mic (or silence) the frame is
// zero and the grid stays dark, so the effect is safe on any target.
class AudioVolumeEffect : public EffectBase {
public:
    const char* tags() const override { return "🔊"; }

    uint8_t brightness = 255;   // overall ceiling

    void onBuildControls() override {
        controls_.addUint8("brightness", brightness, 1, 255);
    }

    void loop() override {
        uint8_t* buf = buffer();
        const lengthType w = width();
        const lengthType h = height();
        const lengthType d = depth();
        const uint8_t cpl = channelsPerLight();
        const size_t total = static_cast<size_t>(w) * h * d * cpl;

        // level is ~0..255; scale to the brightness ceiling.
        const uint16_t level = AudioModule::latestFrame()->level;
        const uint16_t v = static_cast<uint16_t>(
            (level > 255 ? 255 : level) * brightness / 255);

        // Level drives BOTH the palette index and the brightness: quiet maps to the palette's
        // start, loud to its end, dimmed by v. Fill every light identically — a VU meter on the
        // whole surface; modifiers/layouts give it shape.
        const uint8_t lvl = static_cast<uint8_t>(level > 255 ? 255 : level);
        const RGB c = colorFromPalette(*Palettes::active(), lvl, static_cast<uint8_t>(v));
        const uint8_t r = c.r, g = c.g, b = c.b;

        // Write logical RGB only: channel order and any RGBW white are the
        // driver's Correction (W = min(r,g,b)), like every other effect. Effect
        // buffers are RGB (cpl == 3) today; the loop stays correct for any cpl by
        // writing R/G/B where they fit and zeroing any extra channels, so no stale
        // bytes survive from a prior, differently-shaped frame.
        for (size_t off = 0; off < total; off += cpl) {
            if (cpl >= 1) buf[off + 0] = r;
            if (cpl >= 2) buf[off + 1] = g;
            if (cpl >= 3) buf[off + 2] = b;
            for (uint8_t ch = 3; ch < cpl; ch++) buf[off + ch] = 0;
        }
    }
};

} // namespace mm
