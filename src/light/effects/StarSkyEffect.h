#pragma once

#include "light/effects/EffectBase.h"
#include "light/layers/Layer.h"   // layer()->buffer()
#include "light/Palette.h"        // colorFromPalette, Palettes::active()
#include "light/draw.h"           // draw::pixel, draw::fade
#include "core/math8.h"           // Random8
#include "platform/platform.h"    // alloc/free — the per-star heap state

namespace mm {

// Star Sky: a field of independently twinkling stars over a 3D grid. A fixed pool of stars
// (sized from the light count) each pick a random cell, a random initial brightness, and a random
// fade direction; every frame the whole buffer dims a little and each star steps its brightness
// toward full (then reverses) or toward zero (then respawns at a fresh random cell). A small per-
// frame chance (random8() < 10) flips a star's direction early, scattering the twinkle so it never
// pulses in sync. Stars are white (b,b,b) unless usePalette, in which case each carries its own
// palette index. The colour drawn each frame is taken from the brightness BEFORE this frame's step
// (MoonLight computes `color` once from the current brightness, then steps, then setRGB(color)).
//
// Prior art: MoonLight's StarSky (E_MoonModules / MoonModules) — the star-pool model (fill-ratio
// sizing, fade-up/fade-down/respawn, the random early-reverse, the optional palette colour) is
// reproduced here, written fresh on projectMM's EffectBase + shared primitives (Random8,
// colorFromPalette, draw::). The per-star arrays live on the heap (platform::alloc), never as inline
// members, so sizeof(StarSkyEffect) stays tiny.
class StarSkyEffect : public EffectBase {
public:
    const char* tags() const override { return "💫"; }  // MoonLight origin
    Dim dimensions() const override { return Dim::D3; }

    // Defaults match MoonLight's StarSky exactly.
    uint8_t speed           = 1;     // fade step per frame (0..42)
    uint8_t star_fill_ratio = 42;    // stars per 10000 lights (the pool-size lever)
    bool    usePalette      = false; // false → white stars; true → per-star palette colour

    void onBuildControls() override {
        controls_.addUint8("speed", speed, 0, 42);
        controls_.addUint8("star_fill_ratio", star_fill_ratio, 0, 255);
        controls_.addBool("usePalette", usePalette);
    }

    // Per-star state on the heap, sized to nb_stars = star_fill_ratio*nrOfLights/10000 + 1. Reallocated
    // whenever the light count or fill ratio changes (so it tracks a live grid/control edit). Off the
    // hot path; never an inline member (a large inline array overflows the registerType<T> probe stack).
    void onBuildState() override {
        const nrOfLightsType count = nrOfLights();
        const size_t wanted = enabled() && count > 0
            ? (static_cast<size_t>(star_fill_ratio) * count) / 10000u + 1u
            : 0u;
        if (wanted != nbStars_ || count != lightCount_) {
            release();
            if (wanted > 0) {
                indexes_    = static_cast<nrOfLightsType*>(platform::alloc(wanted * sizeof(nrOfLightsType)));
                fadeDir_    = static_cast<uint8_t*>(platform::alloc(wanted));
                brightness_ = static_cast<uint8_t*>(platform::alloc(wanted));
                colors_     = static_cast<uint8_t*>(platform::alloc(wanted));
                if (indexes_ && fadeDir_ && brightness_ && colors_) {
                    nbStars_    = wanted;
                    lightCount_ = count;
                    initStars(count);
                } else {
                    release();
                }
            }
        }
        setDynamicBytes(nbStars_ ? nbStars_ * (sizeof(nrOfLightsType) + 3) : 0);
    }

    void teardown() override { release(); setDynamicBytes(0); }
    ~StarSkyEffect() override { release(); }

    void loop() override {
        if (!indexes_ || !fadeDir_ || !brightness_ || !colors_ || nbStars_ == 0) return;
        const lengthType w = width(), h = height(), d = depthDim();
        if (w <= 0 || h <= 0 || channelsPerLight() < 3) return;
        const nrOfLightsType count = nrOfLights();
        if (count == 0) return;

        Buffer& buf = layer()->buffer();
        const Coord3D dims{w, h, d};

        draw::fade(buf, 50);

        for (size_t i = 0; i < nbStars_; i++) {
            const nrOfLightsType index = indexes_[i];
            // Decode the linear index back to a grid cell (index < nrOfLights → always in bounds).
            const lengthType x = static_cast<lengthType>(index % w);
            const lengthType y = static_cast<lengthType>((index / w) % h);
            const lengthType z = static_cast<lengthType>(index / (static_cast<size_t>(w) * h));
            const Coord3D p{x, y, z};

            // Colour is computed ONCE from the CURRENT (pre-step) brightness, then the brightness is
            // stepped, then the pre-step colour is drawn — matching MoonLight's
            //   color = usePalette ? ColorFromPalette(pal, colors[i], brightness[i]) : CRGB(b,b,b);
            //   brightness[i] += speed; setRGB(pos, color);
            const uint8_t b = brightness_[i];
            const RGB color = usePalette
                ? colorFromPalette(*Palettes::active(), colors_[i], b)
                : RGB{b, b, b};

            if (fadeDir_[i]) {
                // Fading up toward full brightness.
                const uint16_t nb = static_cast<uint16_t>(b) + speed;
                brightness_[i] = nb > 255 ? 255 : static_cast<uint8_t>(nb);
                draw::pixel(buf, dims, p, color);
                if (brightness_[i] == 255) fadeDir_[i] = 0;
                if (rng_.next8() < 10) fadeDir_[i] = 0;
            } else {
                // Fading down toward black; respawn at a fresh cell when it reaches zero.
                brightness_[i] = b > speed ? static_cast<uint8_t>(b - speed) : 0;
                draw::pixel(buf, dims, p, color);
                if (brightness_[i] == 0) {
                    indexes_[i] = randomIndex(count);
                    fadeDir_[i] = 1;
                }
                if (rng_.next8() < 10) fadeDir_[i] = 1;
            }
        }
    }

private:
    nrOfLightsType* indexes_    = nullptr;  // linear cell index per star (< nrOfLights)
    uint8_t*  fadeDir_    = nullptr;  // 0 = fading down, 1 = fading up
    uint8_t*  brightness_ = nullptr;  // current 0..255 brightness per star
    uint8_t*  colors_     = nullptr;  // per-star palette index (used only when usePalette)
    size_t         nbStars_    = 0;
    nrOfLightsType lightCount_ = 0;
    Random8   rng_{0x57A55C1Eu};

    lengthType depthDim() const { return depth() > 0 ? depth() : 1; }

    // A uniform cell pick over 0..count-1. nrOfLightsType is uint32_t on PSRAM builds (>65535 lights),
    // so a single next16() draw can't reach the top of a large grid — compose a full-width draw from
    // two 16-bit draws when the index type is wider than 16 bits, then take it modulo count. On the
    // no-PSRAM (uint16_t) build this collapses to the plain next16() pick.
    nrOfLightsType randomIndex(nrOfLightsType count) {
        if (count == 0) return 0;
        if constexpr (sizeof(nrOfLightsType) > sizeof(uint16_t)) {
            const uint32_t draw = (static_cast<uint32_t>(rng_.next16()) << 16) | rng_.next16();
            return static_cast<nrOfLightsType>(draw % count);
        } else {
            return static_cast<nrOfLightsType>(rng_.next16() % count);
        }
    }

    void release() {
        if (indexes_)    { platform::free(indexes_);    indexes_    = nullptr; }
        if (fadeDir_)    { platform::free(fadeDir_);    fadeDir_    = nullptr; }
        if (brightness_) { platform::free(brightness_); brightness_ = nullptr; }
        if (colors_)     { platform::free(colors_);     colors_     = nullptr; }
        nbStars_ = 0; lightCount_ = 0;
    }

    // Seed every star: random cell, random fade direction, random mid brightness, random colour.
    void initStars(nrOfLightsType count) {
        for (size_t i = 0; i < nbStars_; i++) {
            indexes_[i]    = randomIndex(count);
            fadeDir_[i]    = rng_.below(2);          // 0 or 1   (random8(2))
            brightness_[i] = rng_.below(1, 254);     // 1..253   (random8(1,254))
            colors_[i]     = rng_.next8();
        }
    }
};

} // namespace mm
