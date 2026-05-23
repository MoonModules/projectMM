#pragma once

#include "light/Layer.h"
#include "core/color.h"
#include "platform/platform.h"

#include <cstring>

namespace mm {

class ParticlesEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🦅"; }  // MoonLight origin · David Jupijn / Rising Step
    // Iterates y and x only; Layer::extrude fills z on 3D layers. The trail
    // buffer is sized to the z=0 plane (w*h*cpl), not the full 3D buffer.
    Dim dimensions() const override { return Dim::D2; }

    static constexpr uint8_t MAX_PARTICLES = 64;

    uint8_t count = 32;
    uint8_t speed = 80;
    uint8_t fade = 240;
    uint8_t hue_shift = 0;

    void onBuildControls() override {
        controls_.addUint8("count", count, 1, 255);
        controls_.addUint8("speed", speed, 1, 255);
        controls_.addUint8("fade", fade, 1, 255);
        controls_.addUint8("hue_shift", hue_shift, 0, 255);
    }

    void onAllocateMemory() override {
        // D2 effect: trail buffer covers only the z=0 plane (w*h*cpl). Extrude
        // fills z on 3D layers. Avoids allocating depth× more heap than needed.
        uint8_t cpl = channelsPerLight();
        size_t needed = static_cast<size_t>(width()) * height() * cpl;
        if (enabled() && needed > 0) {
            if (needed != trailBytes_) {
                releaseTrail();
                trail_ = static_cast<uint8_t*>(platform::alloc(needed));
                if (trail_) {
                    std::memset(trail_, 0, needed);
                    trailBytes_ = needed;
                }
            }
            initParticles();
        } else {
            releaseTrail();
        }
        setDynamicBytes(trailBytes_);
    }

    void teardown() override {
        releaseTrail();
        setDynamicBytes(0);
    }

    ~ParticlesEffect() override {
        releaseTrail();
    }

    void loop() override {
        if (!trail_) return;

        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();
        uint8_t* buf = buffer();

        // 1. Fade the persistent trail buffer
        for (size_t i = 0; i < trailBytes_; i++) {
            trail_[i] = scale8(trail_[i], fade);
        }

        // 2. Update and draw particles
        int16_t maxXfp = static_cast<int16_t>((w - 1) << 4);
        int16_t maxYfp = static_cast<int16_t>((h - 1) << 4);
        for (uint8_t i = 0; i < count && i < MAX_PARTICLES; i++) {
            auto& p = particles_[i];

            int16_t sx = static_cast<int16_t>((static_cast<int32_t>(p.vx) * speed) >> 6);
            int16_t sy = static_cast<int16_t>((static_cast<int32_t>(p.vy) * speed) >> 6);
            p.x = static_cast<int16_t>(p.x + sx);
            p.y = static_cast<int16_t>(p.y + sy);

            if (p.x < 0)       { p.x = 0;       p.vx = static_cast<int8_t>(-p.vx); }
            if (p.x > maxXfp)  { p.x = maxXfp;  p.vx = static_cast<int8_t>(-p.vx); }
            if (p.y < 0)       { p.y = 0;       p.vy = static_cast<int8_t>(-p.vy); }
            if (p.y > maxYfp)  { p.y = maxYfp;  p.vy = static_cast<int8_t>(-p.vy); }

            lengthType px = static_cast<lengthType>(p.x >> 4);
            lengthType py = static_cast<lengthType>(p.y >> 4);
            if (px >= 0 && px < w && py >= 0 && py < h) {
                RGB c = hsvToRgb(static_cast<uint8_t>(p.hue + hue_shift), 255, 255);
                size_t off = (static_cast<size_t>(py) * w + px) * cpl;
                if (cpl >= 1) trail_[off + 0] = c.r;
                if (cpl >= 2) trail_[off + 1] = c.g;
                if (cpl >= 3) trail_[off + 2] = c.b;
            }
        }

        // 3. Copy persistent trail buffer to layer buffer (layer cleared it)
        std::memcpy(buf, trail_, trailBytes_);
    }

private:
    struct Particle {
        int16_t x;       // 12.4 fixed-point pixel position
        int16_t y;
        int8_t vx;
        int8_t vy;
        uint8_t hue;
        uint8_t pad;
    };

    Particle particles_[MAX_PARTICLES] = {};
    bool initialized_ = false;
    uint8_t* trail_ = nullptr;
    size_t trailBytes_ = 0;
    uint32_t rngState_ = 0xBADF00Du;

    uint8_t rand8() {
        rngState_ = rngState_ * 1103515245u + 12345u;
        return static_cast<uint8_t>((rngState_ >> 16) & 0xFF);
    }

    void initParticles() {
        lengthType w = width();
        lengthType h = height();
        if (w <= 0 || h <= 0) return;
        if (initialized_) return;
        for (uint8_t i = 0; i < MAX_PARTICLES; i++) {
            particles_[i].x = static_cast<int16_t>((static_cast<uint16_t>(rand8()) * w) >> 4);
            particles_[i].y = static_cast<int16_t>((static_cast<uint16_t>(rand8()) * h) >> 4);
            particles_[i].vx = static_cast<int8_t>(rand8()) >> 1;
            particles_[i].vy = static_cast<int8_t>(rand8()) >> 1;
            if (particles_[i].vx == 0) particles_[i].vx = 1;
            if (particles_[i].vy == 0) particles_[i].vy = 1;
            particles_[i].hue = rand8();
            particles_[i].pad = 0;
        }
        initialized_ = true;
    }

    void releaseTrail() {
        if (trail_) {
            platform::free(trail_);
            trail_ = nullptr;
        }
        trailBytes_ = 0;
    }
};

} // namespace mm
