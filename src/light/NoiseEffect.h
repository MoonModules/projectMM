#pragma once

#include "light/Layer.h"
#include "core/color.h"

namespace mm {

class NoiseEffect : public EffectBase {
public:
    uint8_t scale = 4;  // spatial frequency (1-32)
    uint8_t speed = 50; // animation speed (0-255)

    void onBuildControls() override {
        controls_.addUint8("scale", scale, 1, 32);
        controls_.addUint8("speed", speed, 0, 255);
    }

    void loop() override {
        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();
        nrOfLightsType count = nrOfLights();
        nrOfLightsType wh = static_cast<nrOfLightsType>(w) * h;

        // Time phase from elapsed millis, scaled by speed
        uint32_t t = static_cast<uint32_t>(
            static_cast<uint64_t>(elapsed()) * speed / 1000
        );

        for (nrOfLightsType i = 0; i < count; i++) {
            nrOfLightsType rem = i % wh;
            lengthType x = static_cast<lengthType>(rem % w);
            lengthType y = static_cast<lengthType>(rem / w);

            uint8_t n = noise2d(x, y, t);
            RGB c = hsvToRgb(n, 200, 255);

            size_t offset = static_cast<size_t>(i) * cpl;
            if (cpl >= 1) buf[offset + 0] = c.r;
            if (cpl >= 2) buf[offset + 1] = c.g;
            if (cpl >= 3) buf[offset + 2] = c.b;
        }
    }

private:
    // Hash function for value noise (uint32_t to avoid signed overflow UB)
    static uint8_t hash(uint32_t x, uint32_t y, uint32_t t) {
        uint32_t h = x * 1619u + y * 31337u + t * 6271u;
        h = (h >> 13) ^ h;
        h = h * (h * h * 60493u + 19990303u) + 1376312589u;
        return static_cast<uint8_t>((h >> 16) & 0xFF);
    }

    // Smoothstep: 3t^2 - 2t^3, input/output in 0-255 range
    static uint8_t smoothstep(uint8_t t) {
        uint16_t t2 = static_cast<uint16_t>(t) * t / 255;
        uint16_t t3 = static_cast<uint16_t>(t2) * t / 255;
        return static_cast<uint8_t>((3 * t2 - 2 * t3) & 0xFF);
    }

    // Linear interpolation: a + (b-a) * t/255
    static uint8_t lerp8(uint8_t a, uint8_t b, uint8_t t) {
        int16_t delta = static_cast<int16_t>(b) - static_cast<int16_t>(a);
        return static_cast<uint8_t>(static_cast<int16_t>(a) + delta * t / 255);
    }

    // 2D value noise with bilinear interpolation
    uint8_t noise2d(lengthType px, lengthType py, uint32_t t) const {
        // Scale coordinates: divide by scale to get noise cell
        int32_t sx = static_cast<int32_t>(px) * 256 / scale;
        int32_t sy = static_cast<int32_t>(py) * 256 / scale;

        // Integer cell coordinates
        int32_t ix = sx >> 8;
        int32_t iy = sy >> 8;

        // Fractional part (0-255)
        uint8_t fx = smoothstep(static_cast<uint8_t>(sx & 0xFF));
        uint8_t fy = smoothstep(static_cast<uint8_t>(sy & 0xFF));

        // Hash at four corners
        uint8_t v00 = hash(static_cast<uint32_t>(ix),     static_cast<uint32_t>(iy),     t);
        uint8_t v10 = hash(static_cast<uint32_t>(ix + 1), static_cast<uint32_t>(iy),     t);
        uint8_t v01 = hash(static_cast<uint32_t>(ix),     static_cast<uint32_t>(iy + 1), t);
        uint8_t v11 = hash(static_cast<uint32_t>(ix + 1), static_cast<uint32_t>(iy + 1), t);

        // Bilinear interpolation
        uint8_t top = lerp8(v00, v10, fx);
        uint8_t bot = lerp8(v01, v11, fx);
        return lerp8(top, bot, fy);
    }
};

} // namespace mm
