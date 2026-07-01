#pragma once

#include "light/effects/EffectBase.h"
#include "light/layers/Layer.h"   // layer()->buffer()
#include "light/Palette.h"        // colorFromPalette, Palettes::active()
#include "light/draw.h"           // draw::pixel, draw::fade
#include "core/math8.h"           // Random8
#include "platform/platform.h"    // platform::alloc / platform::free (heap star table)

namespace mm {

// StarField: a forward-flying starfield. Each star is a 3D point (x, y, z) drifting toward the
// viewer; every advance z decreases by 1 and the star is re-projected onto the 2D panel with a
// simple pinhole projection (screen position = centre + (coord / z) scaled to half the panel), so
// stars near the camera (small z) splay outward and fly off the edges while distant stars sit near
// the centre — the classic "warp" / hyperspace look. A star that reaches z<=0 or leaves the panel
// respawns at the far plane (z = width). Brightness (and, with usePalette, hue) rides the depth:
// nearer stars are brighter. `blur` fades the previous frame instead of clearing it, so each star
// leaves a motion streak; `speed` throttles how often the field advances.
//
// Per-star math keeps floats (the x/z, y/z perspective division): it runs once per star, not per
// pixel, off the hot pixel loop, and reproducing MoonLight's exact projection preserves the visual
// the user has known for years (fidelity wins here). The centre offset and the projection scale are
// integer `size/2`, exactly as MoonLight computes them (size is int there), so odd-width panels
// project identically.
//
// Prior art: MoonLight's StarField effect (E_MoonModules / MoonModules). The star model
// (x,y,z + colorIndex), the z-=1 advance, the pinhole re-projection, the depth→brightness map, the
// respawn rule (random depth at first seed, far-plane z=width on respawn), and the
// speed/blur/numStars/usePalette controls are reproduced exactly here, written fresh on EffectBase
// + the shared draw/palette primitives. The star table lives on the heap (platform::alloc), sized
// to the control maximum, rather than as a large inline member.
// Author: @Brandon502 (MoonLight), inspired by Daniel Shiffman / Coding Train — https://www.youtube.com/watch?v=17WoOqgXsRM , https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h
class StarFieldEffect : public EffectBase {
public:
    const char* tags() const override { return "💫"; }  // MoonLight origin
    Dim dimensions() const override { return Dim::D2; }  // writes only the z=0 slice

    uint8_t speed      = 20;     // advance rate (0..30); 0 = paused. Throttle is 1000/speed ms.
    uint8_t numStars   = 16;     // active stars (1..255)
    uint8_t blur       = 128;    // per-frame fade-to-black amount (0..255); higher = stronger fade = shorter streaks (draw::fade keep = 255-blur, matching MoonLight's fadeToBlackBy(blur))
    bool    usePalette = false;  // colour stars from the palette instead of greyscale

    void onBuildControls() override {
        controls_.addUint8("speed", speed, 0, 30);
        controls_.addUint8("numStars", numStars, 1, 255);
        controls_.addUint8("blur", blur, 0, 255);
        controls_.addBool("usePalette", usePalette);
    }

    // Star table is sized to the control maximum (255) so a live numStars change never reallocates,
    // and re-seeds whenever the grid changes (the random spawn ranges depend on width/height). The
    // initial seed scatters stars at random depths (z in [0,width)); respawns later fly in from the
    // far plane.
    void onBuildState() override {
        const lengthType w = width();
        const lengthType h = height();
        if (enabled() && w > 0 && h > 0) {
            if (!stars_) {
                stars_ = static_cast<Star*>(platform::alloc(sizeof(Star) * kMaxStars));
            }
            if (stars_ && (w != seedW_ || h != seedH_)) {
                for (uint16_t i = 0; i < kMaxStars; i++) spawn(stars_[i], w, h, /*far=*/false);
                seedW_ = w;
                seedH_ = h;
            }
        } else {
            release();
        }
        setDynamicBytes(stars_ ? sizeof(Star) * kMaxStars : 0);
    }

    void teardown() override {
        release();
        setDynamicBytes(0);
    }

    ~StarFieldEffect() override { release(); }

    void loop() override {
        if (!stars_) return;

        const lengthType w = width();
        const lengthType h = height();
        if (w <= 0 || h <= 0 || channelsPerLight() < 3) return;

        // Throttle: pause when speed==0, else advance at most once per 1000/speed ms.
        if (speed == 0) return;
        const uint32_t now = elapsed();
        if (now - step_ < 1000u / speed) return;

        Buffer& buf = layer()->buffer();
        const Coord3D dims{w, h, depthDim()};

        // Motion streaks: fade the previous frame rather than clearing it.
        layer()->fadeToBlackBy(blur);

        const int sizeX = w;
        const int sizeY = h;
        // Integer centre/scale, exactly as MoonLight (size is int there): size.x/2, size.y/2.
        const int halfX = sizeX / 2;
        const int halfY = sizeY / 2;

        const uint8_t n = numStars;  // 1..255, never exceeds kMaxStars
        for (uint8_t i = 0; i < n; i++) {
            Star& s = stars_[i];

            // Pinhole projection: fmap(coord/z, 0,1, 0, half) == half * (coord/z) (in_min=0,in_max=1).
            // z<=0 maps to MoonLight's +inf (off-grid, not drawn); guard the divide and treat as such.
            bool inBounds = false;
            int sx = 0, sy = 0;
            if (s.z > 0.0f) {
                const float invZ = 1.0f / s.z;
                sx = halfX + static_cast<int>(halfX * (s.x * invZ));
                sy = halfY + static_cast<int>(halfY * (s.y * invZ));
                inBounds = (sx >= 0 && sx < sizeX && sy >= 0 && sy < sizeY);
            }

            if (inBounds) {
                RGB col;
                if (usePalette) {
                    // Nearer (smaller z) = brighter: depth 0..sizeX maps brightness 255..150.
                    const uint8_t bri = static_cast<uint8_t>(imap(static_cast<int>(s.z), 0, sizeX, 255, 150));
                    col = colorFromPalette(*Palettes::active(), s.colorIndex, bri);
                } else {
                    // Greyscale: base intensity from colorIndex (120..255), scaled by depth (7..10)/10.
                    int color = imap(s.colorIndex, 0, 255, 120, 255);
                    const int brightness = imap(static_cast<int>(s.z), 0, sizeX, 7, 10);
                    color = static_cast<int>(color * (brightness / 10.0f));
                    if (color < 0) color = 0;
                    if (color > 255) color = 255;
                    const uint8_t c = static_cast<uint8_t>(color);
                    col = RGB{c, c, c};
                }
                draw::pixel(buf, dims, {static_cast<lengthType>(sx), static_cast<lengthType>(sy), 0}, col);
            }

            // Advance toward the viewer; respawn at the far plane when it passes the camera or flies
            // off-panel (MoonLight: z = size.x on respawn).
            s.z -= 1.0f;
            if (s.z <= 0.0f || !inBounds) spawn(s, w, h, /*far=*/true);
        }

        step_ = now;
    }

private:
    struct Star {
        float x, y, z;
        uint8_t colorIndex;
    };
    static constexpr uint16_t kMaxStars = 255;  // the numStars control maximum

    // Standard integer map (FastLED ::map), guarded against a zero input span.
    static int imap(int v, int inLo, int inHi, int outLo, int outHi) {
        const int den = inHi - inLo;
        if (den == 0) return outLo;
        return (v - inLo) * (outHi - outLo) / den + outLo;
    }

    // Spawn a star at a random x/y far position with a fresh colour index. `far` selects the depth:
    //   far=false  → initial seed: z in [0, w)  (MoonLight init: z = random(size.x))
    //   far=true   → respawn:      z = w        (MoonLight respawn: z = size.x)
    void spawn(Star& s, lengthType w, lengthType h, bool far) {
        s.x = static_cast<float>(randRange(-w, w));
        s.y = static_cast<float>(randRange(-h, h));
        s.z = far ? static_cast<float>(w)
                  : static_cast<float>(w > 0 ? rng_.next16() % w : 0);
        s.colorIndex = rng_.next8();
    }

    // A uniform integer in [lo, hi) (half-open, like FastLED's random(lo, hi)).
    int randRange(int lo, int hi) {
        if (hi <= lo) return lo;
        const uint32_t span = static_cast<uint32_t>(hi - lo);
        return lo + static_cast<int>(rng_.next16() % span);
    }

    lengthType depthDim() const { return depth() > 0 ? depth() : 1; }

    void release() {
        if (stars_) {
            platform::free(stars_);
            stars_ = nullptr;
        }
        seedW_ = 0;
        seedH_ = 0;
    }

    Star* stars_ = nullptr;
    lengthType seedW_ = 0;
    lengthType seedH_ = 0;
    uint32_t step_ = 0;
    Random8 rng_;
};

} // namespace mm