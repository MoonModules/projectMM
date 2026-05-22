#pragma once

#include "light/Layer.h"
#include "core/color.h"

namespace mm {

// Soft-glowing particles rendered as a metaball field. Particles move with
// independent velocities and bounce off the edges. Field summation gives a
// chaotic, organic blob look — like metaballs with more freedom.
class GlowParticlesEffect : public EffectBase {
public:
    static constexpr uint8_t MAX_PARTICLES = 8;

    uint8_t count = 5;
    uint8_t speed = 60;
    uint8_t radius = 24;
    uint8_t hue_shift = 0;

    void onBuildControls() override {
        controls_.addUint8("count", count, 1, 255);
        controls_.addUint8("speed", speed, 1, 255);
        controls_.addUint8("radius", radius, 4, 255);
        controls_.addUint8("hue_shift", hue_shift, 0, 255);
    }

    void loop() override {
        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();
        if (w <= 0 || h <= 0) return;

        if (!initialized_) initParticles(w, h);

        uint32_t now = elapsed();
        uint32_t dt = now - lastElapsed_;
        lastElapsed_ = now;

        int16_t maxXfp = static_cast<int16_t>((w - 1) << 4);
        int16_t maxYfp = static_cast<int16_t>((h - 1) << 4);

        for (uint8_t i = 0; i < count && i < MAX_PARTICLES; i++) {
            auto& p = particles_[i];
            int16_t sx = static_cast<int16_t>((static_cast<int32_t>(p.vx) * speed * static_cast<int32_t>(dt)) >> 12);
            int16_t sy = static_cast<int16_t>((static_cast<int32_t>(p.vy) * speed * static_cast<int32_t>(dt)) >> 12);
            p.x = static_cast<int16_t>(p.x + sx);
            p.y = static_cast<int16_t>(p.y + sy);
            if (p.x < 0)       { p.x = 0;       p.vx = static_cast<int8_t>(-p.vx); }
            if (p.x > maxXfp)  { p.x = maxXfp;  p.vx = static_cast<int8_t>(-p.vx); }
            if (p.y < 0)       { p.y = 0;       p.vy = static_cast<int8_t>(-p.vy); }
            if (p.y > maxYfp)  { p.y = maxYfp;  p.vy = static_cast<int8_t>(-p.vy); }
        }

        int16_t bx[MAX_PARTICLES] = {};
        int16_t by[MAX_PARTICLES] = {};
        for (uint8_t i = 0; i < count && i < MAX_PARTICLES; i++) {
            bx[i] = static_cast<int16_t>(particles_[i].x >> 4);
            by[i] = static_cast<int16_t>(particles_[i].y >> 4);
        }
        int32_t r2 = static_cast<int32_t>(radius) * radius;

        for (lengthType y = 0; y < h; y++) {
            uint8_t* row = buf + static_cast<size_t>(y) * static_cast<size_t>(w) * cpl;
            for (lengthType x = 0; x < w; x++) {
                uint32_t field = 0;
                for (uint8_t i = 0; i < count && i < MAX_PARTICLES; i++) {
                    int32_t dx = static_cast<int32_t>(x) - bx[i];
                    int32_t dy = static_cast<int32_t>(y) - by[i];
                    int32_t d2 = dx * dx + dy * dy + 1;
                    field += static_cast<uint32_t>((r2 * 64) / d2);
                }
                uint8_t bright = field > 255 ? 255 : static_cast<uint8_t>(field);
                uint8_t hue = static_cast<uint8_t>((field >> 1) + hue_shift);
                RGB c = hsvToRgb(hue, 240, bright);
                if (cpl >= 1) row[0] = c.r;
                if (cpl >= 2) row[1] = c.g;
                if (cpl >= 3) row[2] = c.b;
                row += cpl;
            }
        }
    }

private:
    struct Particle {
        int16_t x;   // 12.4 fixed-point pixel position
        int16_t y;
        int8_t vx;
        int8_t vy;
        uint8_t hue;
        uint8_t pad;
    };

    Particle particles_[MAX_PARTICLES] = {};
    bool initialized_ = false;
    uint32_t lastElapsed_ = 0;
    uint32_t rngState_ = 0xACE1BEEFu;

    uint8_t rand8() {
        rngState_ = rngState_ * 1103515245u + 12345u;
        return static_cast<uint8_t>((rngState_ >> 16) & 0xFF);
    }

    void initParticles(lengthType w, lengthType h) {
        for (uint8_t i = 0; i < MAX_PARTICLES; i++) {
            particles_[i].x = static_cast<int16_t>((static_cast<uint16_t>(rand8()) * w) >> 4);
            particles_[i].y = static_cast<int16_t>((static_cast<uint16_t>(rand8()) * h) >> 4);
            int8_t vx = static_cast<int8_t>((rand8() >> 1) - 32);
            int8_t vy = static_cast<int8_t>((rand8() >> 1) - 32);
            if (vx == 0) vx = 1;
            if (vy == 0) vy = 1;
            particles_[i].vx = vx;
            particles_[i].vy = vy;
            particles_[i].hue = rand8();
            particles_[i].pad = 0;
        }
        initialized_ = true;
    }
};

} // namespace mm
