#pragma once

#include "light/layers/Layer.h"
#include "core/color.h"

namespace mm {

class NoiseEffect : public EffectBase {
public:
    const char* tags() const override { return "⚡️"; }  // FastLED-style noise
    Dim dimensions() const override { return Dim::D3; }

    uint8_t scale = 4;  // spatial frequency (1-32)
    uint8_t bpm = 60;   // beats per minute — scrolls 8 noise cells per beat

    void onBuildControls() override {
        controls_.addUint8("scale", scale, 1, 32);
        controls_.addUint8("bpm", bpm, 1, 255);
    }

    void loop() override {
        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        lengthType d = depth();
        uint8_t cpl = channelsPerLight();
        nrOfLightsType count = nrOfLights();
        nrOfLightsType wh = static_cast<nrOfLightsType>(w) * h;

        // Accumulate phase incrementally — changing BPM doesn't cause a jump.
        // Factor 32 tuned so 60 BPM at 128-wide gives smooth motion.
        uint32_t now = elapsed();
        uint32_t dt = now - lastElapsed_;
        lastElapsed_ = now;
        phase_ += static_cast<uint64_t>(dt) * bpm * w * 64 / 60000;
        uint32_t t = static_cast<uint32_t>(phase_);

        // Buffer layout is (z * h * w + y * w + x). For a 2D grid (d == 1) z
        // is always 0 and we sample 2D noise — no perf cost vs. the old 2D-only
        // path. For d > 1 we sample 3D noise so each z-slice differs.
        for (nrOfLightsType i = 0; i < count; i++) {
            nrOfLightsType rem = i % wh;
            lengthType x = static_cast<lengthType>(rem % w);
            lengthType y = static_cast<lengthType>(rem / w);
            lengthType z = static_cast<lengthType>(i / wh);

            uint8_t n = (d > 1) ? noise3d(x, y, z, t) : noise2d(x, y, t);
            RGB c = hsvToRgb(n, 200, 255);

            size_t offset = static_cast<size_t>(i) * cpl;
            if (cpl >= 1) buf[offset + 0] = c.r;
            if (cpl >= 2) buf[offset + 1] = c.g;
            if (cpl >= 3) buf[offset + 2] = c.b;
        }
    }

private:
    uint64_t phase_ = 0;
    uint32_t lastElapsed_ = 0;

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
    // Time scrolls the noise field smoothly (offset, not hash seed)
    uint8_t noise2d(lengthType px, lengthType py, uint32_t timeOffset) const {
        // Scale coordinates and add time as smooth scroll offset
        int32_t sx = (static_cast<int32_t>(px) * 256 + static_cast<int32_t>(timeOffset)) / scale;
        int32_t sy = (static_cast<int32_t>(py) * 256 + static_cast<int32_t>(timeOffset / 3)) / scale;

        // Integer cell coordinates
        int32_t ix = sx >> 8;
        int32_t iy = sy >> 8;

        // Fractional part (0-255)
        uint8_t fx = smoothstep(static_cast<uint8_t>(sx & 0xFF));
        uint8_t fy = smoothstep(static_cast<uint8_t>(sy & 0xFF));

        // Hash at four corners (time=0, motion comes from coordinate scrolling)
        uint8_t v00 = hash(static_cast<uint32_t>(ix),     static_cast<uint32_t>(iy),     0);
        uint8_t v10 = hash(static_cast<uint32_t>(ix + 1), static_cast<uint32_t>(iy),     0);
        uint8_t v01 = hash(static_cast<uint32_t>(ix),     static_cast<uint32_t>(iy + 1), 0);
        uint8_t v11 = hash(static_cast<uint32_t>(ix + 1), static_cast<uint32_t>(iy + 1), 0);

        // Bilinear interpolation
        uint8_t top = lerp8(v00, v10, fx);
        uint8_t bot = lerp8(v01, v11, fx);
        return lerp8(top, bot, fy);
    }

    // 3D value noise with trilinear interpolation over 8 cube corners. The
    // hash's third argument doubles as the z axis (kept consistent with 2D —
    // the 2D path passes 0 there). Time scrolls all three axes at slightly
    // different rates so the field flows rather than slides flat.
    uint8_t noise3d(lengthType px, lengthType py, lengthType pz, uint32_t timeOffset) const {
        int32_t sx = (static_cast<int32_t>(px) * 256 + static_cast<int32_t>(timeOffset))     / scale;
        int32_t sy = (static_cast<int32_t>(py) * 256 + static_cast<int32_t>(timeOffset / 3)) / scale;
        int32_t sz = (static_cast<int32_t>(pz) * 256 + static_cast<int32_t>(timeOffset / 5)) / scale;

        int32_t ix = sx >> 8;
        int32_t iy = sy >> 8;
        int32_t iz = sz >> 8;

        uint8_t fx = smoothstep(static_cast<uint8_t>(sx & 0xFF));
        uint8_t fy = smoothstep(static_cast<uint8_t>(sy & 0xFF));
        uint8_t fz = smoothstep(static_cast<uint8_t>(sz & 0xFF));

        // 8 cube-corner hashes
        uint8_t v000 = hash(static_cast<uint32_t>(ix),     static_cast<uint32_t>(iy),     static_cast<uint32_t>(iz));
        uint8_t v100 = hash(static_cast<uint32_t>(ix + 1), static_cast<uint32_t>(iy),     static_cast<uint32_t>(iz));
        uint8_t v010 = hash(static_cast<uint32_t>(ix),     static_cast<uint32_t>(iy + 1), static_cast<uint32_t>(iz));
        uint8_t v110 = hash(static_cast<uint32_t>(ix + 1), static_cast<uint32_t>(iy + 1), static_cast<uint32_t>(iz));
        uint8_t v001 = hash(static_cast<uint32_t>(ix),     static_cast<uint32_t>(iy),     static_cast<uint32_t>(iz + 1));
        uint8_t v101 = hash(static_cast<uint32_t>(ix + 1), static_cast<uint32_t>(iy),     static_cast<uint32_t>(iz + 1));
        uint8_t v011 = hash(static_cast<uint32_t>(ix),     static_cast<uint32_t>(iy + 1), static_cast<uint32_t>(iz + 1));
        uint8_t v111 = hash(static_cast<uint32_t>(ix + 1), static_cast<uint32_t>(iy + 1), static_cast<uint32_t>(iz + 1));

        // Trilinear interpolation: lerp along x, then y (two faces), then z.
        uint8_t z0top = lerp8(v000, v100, fx);
        uint8_t z0bot = lerp8(v010, v110, fx);
        uint8_t z0    = lerp8(z0top, z0bot, fy);
        uint8_t z1top = lerp8(v001, v101, fx);
        uint8_t z1bot = lerp8(v011, v111, fx);
        uint8_t z1    = lerp8(z1top, z1bot, fy);
        return lerp8(z0, z1, fz);
    }
};

} // namespace mm
