#pragma once

#include "light/effects/EffectBase.h"
#include "light/layers/Layer.h"   // layer()->buffer()
#include "light/Palette.h"        // colorFromPalette, Palettes::active()
#include "light/draw.h"           // draw::pixel, draw::blendPixel
#include "core/math8.h"           // Random8
#include "platform/platform.h"    // platform::alloc/free, platform::millis

namespace mm {

// Tetrix: each column drops a "brick" of light that falls under a per-column speed, lands on a
// growing stack at the bottom, and once the stack fills the column the whole column blanks back
// to black and the cycle restarts — the falling-block "Tetris" look applied per LED column. Each
// column runs an independent little state machine (idle → start-delay → fall → stack → blank),
// so the columns desync into a shimmering rain of stacking bricks. With `oneColor` every brick in
// a column shares one slowly-advancing palette index; otherwise each new brick picks a random one.
//
// Prior art: MoonLight's Tetrix (E_MoonModules / MoonModules), descended from the WLED "Tetrix"
// effect (Aircoookie / blazoncek). The per-column physics (mapped fall speed = grid-height·FRAMETIME
// / map(speed,1,255,40000,250), the `pos`/`stack`/`brick` integers, the millis()+2000 start/blank
// delays, and the step-machine values 0/1/2/>2) and the colour rules are reproduced from the
// MoonLight spec, written fresh on EffectBase + the shared draw primitives. One drop per X column;
// safe at any grid size.
// Author: Andrew Tuline (WLED-SR) — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h
class TetrixEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🌙"; }  // MoonLight origin · MoonModules
    Dim dimensions() const override { return Dim::D2; }   // writes only the z=0 slice; iterates x and y

    // Controls — MoonLight's exact defaults. `speedControl` is the UI "speed" (0 = random per brick).
    uint8_t speedControl = 0;     // 0..255; 0 → each brick gets a random fall speed
    uint8_t widthControl = 0;     // 0..255; 0 → random brick height, else derived from this
    bool    oneColor     = false; // all bricks in a column share one slowly-advancing colour

    void onBuildControls() override {
        controls_.addUint8("speed", speedControl, 0, 255);
        controls_.addUint8("width", widthControl, 0, 255);
        controls_.addBool("oneColor", oneColor);
    }

    // Per-column falling-brick state (MoonLight's Tetris struct). `step` doubles as the state
    // machine value (0 idle, 1 start-roll, 2 falling) AND as a future-millis timestamp once it
    // holds millis()+2000 (the start delay and the post-fill blank delay) — those are all > 2.
    struct Tetris {
        float    pos   = 0.0f;  // current head position of the falling brick (in LED rows)
        float    speed = 0.0f;  // fall speed in rows per frame
        uint8_t  col   = 0;     // palette index for this column's brick(s)
        uint16_t brick = 0;     // brick height in LEDs
        uint16_t stack = 0;     // current stacked height at the bottom
        uint32_t step  = 0;     // state machine / timestamp (see above)
    };

    void onBuildState() override {
        // One drop per X column. Reallocate only when the column count changes.
        const nrOfLightsType cols = (enabled() && width() > 0)
                                        ? static_cast<nrOfLightsType>(width()) : 0;
        if (cols != nrOfDrops_) {
            releaseDrops();
            if (cols > 0) {
                drops_ = static_cast<Tetris*>(platform::alloc(cols * sizeof(Tetris)));
                if (drops_) nrOfDrops_ = cols;
            }
        }
        if (drops_) {
            // MoonLight's onSizeChanged init: every column starts idle (stack=0), with a 2 s start
            // delay (step = millis()+2000), and oneColor columns seeded to palette index 0.
            const uint32_t now = platform::millis();
            for (nrOfLightsType i = 0; i < nrOfDrops_; i++) {
                drops_[i] = Tetris{};
                drops_[i].stack = 0;
                drops_[i].step  = now + 2000;
                if (oneColor) drops_[i].col = 0;
            }
        }
        setDynamicBytes(static_cast<size_t>(nrOfDrops_) * sizeof(Tetris));
    }

    void teardown() override {
        releaseDrops();
        setDynamicBytes(0);
    }

    ~TetrixEffect() override { releaseDrops(); }

    void loop() override {
        if (!drops_) return;

        const lengthType w = width();
        const lengthType h = height();
        if (w <= 0 || h <= 0 || channelsPerLight() < 1) return;

        Buffer& buf = layer()->buffer();
        const Coord3D dims{w, h, depthDim()};

        const uint32_t now = platform::millis();
        const RGB black{0, 0, 0};

        // Process exactly the live column count (never the allocated max — robust to a shrink
        // before onBuildState reruns).
        const nrOfLightsType nrOfDrops = (static_cast<nrOfLightsType>(w) < nrOfDrops_)
                                             ? static_cast<nrOfLightsType>(w) : nrOfDrops_;

        for (nrOfLightsType x = 0; x < nrOfDrops; x++) {
            Tetris& d = drops_[x];

            if (d.step == 0) {
                // Idle → spawn a new brick. speed input is the control (or random when 0).
                const uint8_t in = speedControl ? speedControl : rng_.below(1, 255);
                // FastLED map(in, 1, 255, 40000, 250) — descending; the result (250..40000) needs a
                // wide type (it does NOT fit in a byte). Fall speed = grid-height·FRAMETIME / mapped.
                const long mapped = mapRange(in, 1, 255, 40000, 250);
                d.speed = (mapped > 0) ? (static_cast<float>(h) * FRAMETIME) / static_cast<float>(mapped)
                                       : 1.0f;
                d.pos   = static_cast<float>(h);             // start above the top, fall downward
                if (!oneColor) d.col = static_cast<uint8_t>(rng_.below(0, 15) << 4);
                d.step  = 1;
                // Brick height: from the width control (if set) or random 1..4, scaled up on tall grids.
                d.brick = static_cast<uint16_t>((widthControl ? ((widthControl >> 5) + 1)
                                                              : rng_.below(1, 5))
                                                * (1 + (h >> 6)));
            } else if (d.step == 1) {
                // Start-roll: ~75% chance each frame to begin falling (random8() >> 6 is 0 ~1/4 of the time).
                if (rng_.next8() >> 6) d.step = 2;
            } else if (d.step == 2) {
                // Falling: descend until the brick head reaches the top of the stack.
                if (d.pos > static_cast<float>(d.stack)) {
                    d.pos -= d.speed;
                    if (d.pos < static_cast<float>(d.stack)) d.pos = static_cast<float>(d.stack);
                    // Render the brick: rows [pos, pos+brick) lit in the column colour, above it black.
                    for (lengthType i = static_cast<lengthType>(d.pos); i < h; i++) {
                        const RGB c = (i < static_cast<lengthType>(d.pos) + static_cast<lengthType>(d.brick))
                                          ? colorFromPalette(*Palettes::active(), d.col)
                                          : black;
                        draw::pixel(buf, dims, {static_cast<lengthType>(x),
                                                static_cast<lengthType>(h - 1 - i), 0}, c);
                    }
                } else {
                    // Landed: grow the stack by the brick height. If the column is full, start the
                    // blank delay (step = millis()+2000); otherwise idle for the next brick.
                    d.step = 0;
                    d.stack = static_cast<uint16_t>(d.stack + d.brick);
                    if (d.stack >= static_cast<uint16_t>(h)) d.step = now + 2000;
                }
            } else {
                // step > 2: the column is full and waiting to blank. While the delay is in the
                // future, fade the whole column toward black; once it elapses, reset the column.
                // The compare is the wrap-safe signed-difference form ((int32_t)(step - now) > 0)
                // rather than raw `step > now`, so it stays correct across the 32-bit millis()
                // wrap (~49 days) — the modular difference fits a signed range for the 2 s delay.
                d.brick = 0;
                if (static_cast<int32_t>(d.step - now) > 0) {
                    for (lengthType i = 0; i < h; i++)
                        draw::blendPixel(buf, dims, {static_cast<lengthType>(x), i, 0}, black, 25);
                } else {
                    d.stack = 0;
                    d.step  = 0;
                    if (oneColor) d.col = static_cast<uint8_t>(d.col + 8);
                }
            }
        }
    }

private:
    // FRAMETIME at MoonLight's 40 FPS rate (1000/40 = 25 ms): the per-frame step the fall speed
    // is calibrated against, so the descent rate is grid-rate-independent.
    static constexpr int FRAMETIME = 1000 / 40;

    // FastLED's integer ::map(x, inMin, inMax, outMin, outMax) — used here with a descending out
    // range (40000 → 250), so the result falls as the speed control rises. Guards inMax == inMin.
    static long mapRange(long x, long inMin, long inMax, long outMin, long outMax) {
        const long den = inMax - inMin;
        if (den == 0) return outMin;
        return (x - inMin) * (outMax - outMin) / den + outMin;
    }

    // The grid depth accessor needs the >0 guard for the dims z extent; the width/oneColor control
    // members are named *Control so they don't shadow the inherited width()/depth() accessors.
    lengthType depthDim() const { return depth() > 0 ? depth() : 1; }

    void releaseDrops() {
        if (drops_) { platform::free(drops_); drops_ = nullptr; }
        nrOfDrops_ = 0;
    }

    Tetris*        drops_     = nullptr;
    nrOfLightsType nrOfDrops_ = 0;
    Random8        rng_;
};

} // namespace mm