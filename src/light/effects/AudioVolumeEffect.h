#pragma once

#include "light/layers/Layer.h"
#include "core/MicModule.h"   // MicModule::latestFrame()

namespace mm {

// Audio-reactive VU effect: the whole grid pulses with the microphone's sound
// level. The simplest audio consumer — one scalar (AudioFrame::level) drives a
// single brightness, a colour shifting from calm to hot as it rises. Reads the
// live frame from MicModule::latestFrame(); with no mic (or silence) the frame is
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
        const uint16_t level = MicModule::latestFrame()->level;
        const uint16_t v = static_cast<uint16_t>(
            (level > 255 ? 255 : level) * brightness / 255);

        // A simple level-driven colour ramp: green (quiet) → red (loud), with v
        // setting overall intensity. Fill every light identically — a VU meter on
        // the whole surface; modifiers/layouts give it shape.
        const uint8_t r = static_cast<uint8_t>(v);
        const uint8_t g = static_cast<uint8_t>(v > 128 ? (255 - v) * 2 : 255 * v / 128);
        const uint8_t b = 0;

        for (size_t off = 0; off < total; off += cpl) {
            if (cpl >= 1) buf[off + 0] = r;
            if (cpl >= 2) buf[off + 1] = g;
            if (cpl >= 3) buf[off + 2] = b;
            if (cpl >= 4) buf[off + 3] = 0;   // RGBW: leave white off
        }
    }
};

} // namespace mm
