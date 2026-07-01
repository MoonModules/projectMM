#pragma once

#include "light/effects/EffectBase.h"
#include "light/layers/Layer.h"   // layer()->buffer()
#include "light/Palette.h"        // colorFromPalette, Palettes::active()
#include "light/draw.h"           // draw::pixel, draw::fade, draw::blur
#include "core/color.h"           // RGB, scale8
#include "core/math8.h"           // Random8
#include "platform/platform.h"    // platform::alloc / platform::free (heap ant table)

#include <cmath>                  // fabsf, roundf (per-ant physics, not per-light)

namespace mm {

// Ant: a 1D effect of ants marching along the strip, each a point with a position (0..1 along the
// strip), a signed velocity, and a "has food" flag. Each ant moves at constant velocity, bounces at
// the two ends, and — with `gatherFood` on — carries a food pixel that flips colour at the far end.
// With `passBy` off, ants that meet collide elastically (the heavier-speed one reverses), resolved
// with a closed-form collision-time solve so two ants exchange momentum at the exact crossing moment
// rather than passing through each other. `antSize` widens each ant to a short body; `blur` softens
// the whole strip. The result is a busy, organic line of dots ricocheting back and forth.
//
// Per-ant math keeps floats (the position/velocity/collision-time physics): it runs once per ant,
// not per pixel, off the hot pixel loop, and reproducing MoonLight's exact integration preserves the
// look the user has known for years (fidelity wins here). Time deltas are taken from elapsed() (ms
// since the effect started); the ants store their last-bump timestamp in the same clock, so the
// `millis() - lastBumpUpdate` differences match MoonLight's exactly.
//
// Prior art: MoonLight's Ant effect (E_MoonModules / MoonModules), a 1D physics toy. The ant model
// (position/velocity/hasFood/lastBumpUpdate), the time-conversion factor, the boundary bounce, the
// food-pixel colouring, the pairwise collision-time solve and momentum exchange, and the
// antSpeed/nrOfAnts/antSize/blur/gatherFood/passBy controls are reproduced exactly here, written
// fresh on EffectBase + the shared draw/palette primitives. The ant table lives on the heap
// (platform::alloc, sized to MAX_ANTS) rather than as a large inline member.
class AntEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🌙"; }  // MoonLight origin · MoonModules
    Dim dimensions() const override { return Dim::D1; }   // runs along the strip (1D)

    uint8_t antSpeed       = 192;  // 0..255 — higher = faster (shorter time-conversion factor)
    uint8_t nrOfAnts       = 16;   // 1..32 — density knob (MAX_ANTS/2); actual count scales with strip length
    uint8_t antSizeControl = 8;    // 1..20 — body length in pixels (+1 with gatherFood)
    uint8_t blur           = 0;    // 0..255 — strip blur amount (applied as blur>>1)
    bool    gatherFood     = true; // ants carry a food pixel that flips colour at the far end
    bool    passBy         = true; // true = ants pass through each other; false = they collide

    void onBuildControls() override {
        controls_.addUint8("antSpeed", antSpeed, 0, 255);
        controls_.addUint8("nrOfAnts", nrOfAnts, 1, 32);
        controls_.addUint8("antSize", antSizeControl, 1, 20);
        controls_.addUint8("blur", blur, 0, 255);
        controls_.addBool("gatherFood", gatherFood);
        controls_.addBool("passBy", passBy);
    }

    // Ant table is a fixed MAX_ANTS array on the heap, seeded once when first allocated (MoonLight's
    // initAnts(), called from setup). A live nrOfAnts / strip-size change never reallocates — only
    // the number of ants actually rendered (numAnts) changes, derived each frame from the strip
    // length, exactly as the source does.
    void onBuildState() override {
        if (enabled() && nrOfLights() > 0) {
            if (!ants_) {
                ants_ = static_cast<Ant*>(platform::alloc(sizeof(Ant) * MAX_ANTS));
                if (ants_) initAnts();
            }
        } else {
            release();
        }
        setDynamicBytes(ants_ ? sizeof(Ant) * MAX_ANTS : 0);
    }

    void teardown() override {
        release();
        setDynamicBytes(0);
    }

    ~AntEffect() override { release(); }

    void loop() override {
        if (!ants_) return;

        const int nLights = static_cast<int>(nrOfLights());
        if (nLights <= 0 || channelsPerLight() < 3) return;

        Buffer& buf = layer()->buffer();
        // 1D: the strip is a single row of nrOfLights pixels (convention: dims {nrOfLights,1,1}).
        const Coord3D dims{static_cast<lengthType>(nLights), 1, 1};

        const unsigned long now = elapsed();

        // Count of ants actually rendered: 1 + (nrOfLights * nrOfAnts / 4096), capped at MAX_ANTS.
        unsigned numAnts = MINu(1u + ((static_cast<unsigned>(nLights) * nrOfAnts) >> 12), MAX_ANTS);
        const bool passByEff = passBy || gatherFood;
        const unsigned antSize = antSizeControl + (gatherFood ? 1u : 0u);
        // Time-conversion factor: slower antSpeed → larger factor → slower motion. scale8 from color.h.
        const float timeConversionFactor = float(scale8(8, static_cast<uint8_t>(255 - antSpeed)) + 1) * 20000.0f;

        // Clear the strip every frame (MoonLight: fadeToBlackBy(255)).
        draw::fade(buf, 255);

        for (unsigned i = 0; i < numAnts; i++) {
            Ant& ai = ants_[i];

            float timeSinceLastUpdate = float(now - ai.lastBumpUpdate) / timeConversionFactor;
            float newPosition = ai.position + ai.velocity * timeSinceLastUpdate;

            // Sanity respawn if the integration ran away (position far outside [0,1]).
            if (newPosition < -0.5f || newPosition > 1.5f) {
                newPosition = ai.position = randUnit();
                ai.lastBumpUpdate = now;
            }

            // Boundary bounce: at the strip ends, reverse (with food handling when gatherFood).
            if (newPosition <= 0.0f && ai.velocity < 0.0f)      handleBoundary(ai, newPosition, gatherFood, /*atStart=*/true,  now);
            else if (newPosition >= 1.0f && ai.velocity > 0.0f) handleBoundary(ai, newPosition, gatherFood, /*atStart=*/false, now);

            // Pairwise elastic collisions (only when ants don't pass each other).
            if (!passByEff) {
                for (unsigned j = i + 1; j < numAnts; j++) {
                    Ant& aj = ants_[j];
                    if (fabsf(aj.velocity - ai.velocity) < 0.001f) continue;  // parallel: never meet
                    // Time deltas are UNSIGNED-long differences, exactly as MoonLight does them:
                    // when aj was bumped after ai (aj.lastBumpUpdate < ai.lastBumpUpdate) the subtraction
                    // wraps to a huge value, which pushes collisionTime past the `< timeSinceJ` guard and
                    // suppresses the collision. Reproducing that wrap (not a signed delta) keeps the exact
                    // collision-firing pattern users have seen for years — fidelity over "fixing" the wrap.
                    const float timeOffset = float(aj.lastBumpUpdate - ai.lastBumpUpdate);
                    const float collisionTime = (timeConversionFactor * (ai.position - aj.position) + ai.velocity * timeOffset)
                                              / (aj.velocity - ai.velocity);
                    const float timeSinceJ = float(now - aj.lastBumpUpdate);
                    if (collisionTime > MIN_COLLISION_TIME_MS && collisionTime < timeSinceJ) {
                        const float adjustedTime = (collisionTime + float(aj.lastBumpUpdate - ai.lastBumpUpdate))
                                                 / timeConversionFactor;
                        ai.position += ai.velocity * adjustedTime;
                        aj.position = ai.position;
                        const unsigned long collisionMoment = (unsigned long)(collisionTime + 0.5f) + aj.lastBumpUpdate;
                        ai.lastBumpUpdate = collisionMoment;
                        aj.lastBumpUpdate = collisionMoment;
                        // The faster ant reverses (momentum exchange).
                        if (fabsf(ai.velocity) > fabsf(aj.velocity)) ai.velocity = -ai.velocity;
                        else                                         aj.velocity = -aj.velocity;
                        newPosition = ai.position + ai.velocity * float(now - ai.lastBumpUpdate) / timeConversionFactor;
                    }
                }
            }

            newPosition = constrainf(newPosition, 0.0f, 1.0f);
            const unsigned pixelPosition = static_cast<unsigned>(roundf(newPosition * float(nLights - 1)));
            const RGB antColor = getAntColor(static_cast<int>(i), static_cast<int>(numAnts));

            for (unsigned pixelOffset = 0; pixelOffset < antSize; pixelOffset++) {
                const unsigned currentPixel = pixelPosition + pixelOffset;
                if (currentPixel >= static_cast<unsigned>(nLights)) break;
                renderAntPixel(buf, dims, static_cast<int>(currentPixel), static_cast<int>(pixelOffset),
                               static_cast<int>(antSize), ai, antColor, gatherFood);
            }

            ai.lastBumpUpdate = now;
            ai.position = newPosition;
        }

        // Soften the strip (MoonLight: blur1d(blur>>1)).
        draw::blur(buf, dims, static_cast<uint8_t>(blur >> 1));
    }

private:
    struct Ant {
        unsigned long lastBumpUpdate;
        bool  hasFood;
        float velocity;
        float position;
    };

    static constexpr unsigned MAX_ANTS = 32;
    static constexpr float MIN_COLLISION_TIME_MS = 2.0f;
    static constexpr float VELOCITY_MIN = 2.0f;
    static constexpr float VELOCITY_MAX = 10.0f;

    // CRGB colour constants the food logic compares against (MoonLight: CRGB::White / Yellow / Gray).
    static constexpr RGB White{255, 255, 255};
    static constexpr RGB Yellow{255, 255, 0};
    static constexpr RGB Gray{128, 128, 128};

    static bool eq(RGB a, RGB b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

    // The food pixel's colour, given the ant's colour against the (always Black) background. The
    // background is Black here, so neither equality holds: an ant whose colour happens to be White
    // gets a Yellow food pixel; every other ant gets a White food pixel.
    static RGB getFoodColor(RGB antColor, RGB backgroundColor) {
        if (eq(antColor, White)) return eq(backgroundColor, Yellow) ? Gray : Yellow;
        return eq(backgroundColor, White) ? Yellow : White;
    }

    // Reflect an ant at a strip end. With food gathering it reverses direction and toggles hasFood
    // (picks up at the start, drops off at the far end); without it, it simply clamps to the end.
    static void handleBoundary(Ant& ant, float& position, bool gatherFood, bool atStart, unsigned long currentTime) {
        if (gatherFood) {
            position = atStart ? 0.0f : 1.0f;
            ant.velocity = -ant.velocity;
            ant.lastBumpUpdate = currentTime;
            ant.position = position;
            ant.hasFood = atStart;
        } else {
            position = atStart ? 1.0f : 0.0f;
            ant.lastBumpUpdate = currentTime;
            ant.position = position;
        }
    }

    // Each ant's colour: evenly spaced across the palette by index (MoonLight always samples the
    // palette here; the source's usePalette flag is unused, so there is no greyscale path).
    static RGB getAntColor(int antIndex, int numAnts) {
        return colorFromPalette(*Palettes::active(), static_cast<uint8_t>(antIndex * 255 / numAnts));
    }

    // Draw one pixel of an ant's body. The leading edge (front of travel) carries the food pixel when
    // the ant has food: moving backward, that's offset 0; moving forward, it's the last offset.
    static void renderAntPixel(Buffer& buf, Coord3D dims, int pixelIndex, int pixelOffset, int antSize,
                               const Ant& ant, RGB antColor, bool gatherFood) {
        const bool isMovingBackward = (ant.velocity < 0.0f);
        const bool isFoodPixel = gatherFood && ant.hasFood
            && ((isMovingBackward && pixelOffset == 0) || (!isMovingBackward && pixelOffset == antSize - 1));
        const RGB c = isFoodPixel ? getFoodColor(antColor, RGB{0, 0, 0}) : antColor;
        draw::pixel(buf, dims, {static_cast<lengthType>(pixelIndex), 0, 0}, c);
    }

    // Seed all MAX_ANTS with random velocities and positions; one randomly chosen ant starts reversed
    // (the "confused ant"). Called once when the table is first allocated.
    void initAnts() {
        const int confusedAntIndex = rng_.below(static_cast<uint8_t>(nrOfAnts));
        const unsigned long now = elapsed();
        for (unsigned i = 0; i < MAX_ANTS; i++) {
            ants_[i].lastBumpUpdate = now;
            const float velocity = VELOCITY_MIN + (VELOCITY_MAX - VELOCITY_MIN) * float(rand16Range(1000, 5000)) / 5000.0f;
            ants_[i].velocity = (static_cast<int>(i) == confusedAntIndex) ? -velocity : velocity;
            ants_[i].position = randUnit();
            ants_[i].hasFood = false;
        }
    }

    // FastLED random16(min, max): a uniform integer in [min, max).
    uint16_t rand16Range(uint16_t lo, uint16_t hi) {
        return hi > lo ? static_cast<uint16_t>(lo + rng_.next16() % (hi - lo)) : lo;
    }

    // A random position in [0,1): MoonLight's random16(0,10000)/10000.0f.
    float randUnit() { return float(rand16Range(0, 10000)) / 10000.0f; }

    static unsigned MINu(unsigned a, unsigned b) { return a < b ? a : b; }
    static float constrainf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

    void release() {
        if (ants_) {
            platform::free(ants_);
            ants_ = nullptr;
        }
    }

    Ant* ants_ = nullptr;
    Random8 rng_;
};

} // namespace mm