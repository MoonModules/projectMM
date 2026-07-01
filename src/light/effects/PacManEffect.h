#pragma once

#include "light/effects/EffectBase.h"
#include "light/layers/Layer.h"   // layer()->buffer()
#include "light/draw.h"           // draw::pixel/fade/blur
#include "core/math8.h"           // Random8 (rng_), integer helpers
#include "platform/platform.h"    // alloc/free — the per-character heap array

#include <cstring>                // strcmp — control-name dispatch

namespace mm {

// PacMan — a 1D LED game animation: a yellow Pac-Man chomps left-to-right along the strip eating a
// trail of orange-yellow power dots while a pack of coloured ghosts chases. Eating a power dot turns
// every ghost blue and reverses the whole pack; the blue ghosts blink white as Pac-Man closes in,
// and when Pac-Man reaches the strip start the ghosts recolour and the dots respawn. A leading
// "wall" of whitish dots marks the furthest Pac-Man has reached, the power dots flash on/off on a
// timing counter, and an optional smear/blur softens the strip. Pure 1D over nrOfLights — the index
// is the LED position; height/depth are not iterated (extrude fills them).
//
// Prior art: MoonLight's PacMan (E_MoonModules / MoonModules) — the game state machine (Pac-Man +
// ghost positions and directions, power-dot layout via the `everyXLeds` fixed-point step, the blue/
// blink timing on aux1TimingCounter, the eat→reverse→respawn cycle, the leading dot wall, the
// blinkDistance-gated white blink) is reproduced exactly here, written fresh on EffectBase + the
// shared draw primitives. The 0xRRGGBB game colours are unpacked into RGB; the strip blur uses the
// unified draw::blur. Runs on any strip length and tick rate; below 16+2·ghosts LEDs it idles.
class PacManEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🌙"; }  // MoonLight origin · MoonModules
    Dim dimensions() const override { return Dim::D1; }

    // Controls — MoonLight's exact defaults and ranges.
    uint8_t speed              = 192;
    uint8_t numPowerDotsControl = 64;   // "#powerdots"
    uint8_t blinkDistance      = 64;    // 20..255
    uint8_t blur               = 0;
    uint8_t numGhosts          = 4;     // 2..8
    bool    dots               = true;
    bool    smearMode          = false; // "smear"
    bool    compact            = false;

    void onBuildControls() override {
        controls_.addUint8("speed", speed);
        controls_.addUint8("#powerdots", numPowerDotsControl);
        controls_.addUint8("blinkDistance", blinkDistance, 20, 255);
        controls_.addUint8("blur", blur);
        controls_.addUint8("#ghosts", numGhosts, 2, 8);
        controls_.addBool("dots", dots);
        controls_.addBool("smear", smearMode);
        controls_.addBool("compact", compact);
    }

    // A change to the power-dot count or ghost count re-sizes the character array, so it must run
    // the onBuildState() sweep that re-allocates and re-initialises the game (the other controls are
    // read live in loop()).
    bool controlChangeTriggersBuildState(const char* name) const override {
        return std::strcmp(name, "#powerdots") == 0 || std::strcmp(name, "#ghosts") == 0;
    }

    // (Re)build the character array for the current strip length / control values. Called by the
    // Scheduler on boot, on a strip-size change, and on a #powerdots/#ghosts change. MoonLight calls
    // initializePacMan() from onSizeChanged + those two onUpdate cases — same trigger set here.
    void onBuildState() override {
        if (enabled() && nrOfLights() > 0) initializePacMan();
        else release();
        setDynamicBytes(nrOfCharacters_ * sizeof(PacManChar));
    }

    void teardown() override { release(); setDynamicBytes(0); }
    ~PacManEffect() override { release(); }

    void loop() override {
        const int n = static_cast<int>(nrOfLights());
        if (channelsPerLight() < 1) return;
        // Idle below the playable length (MoonLight: nrOfLights > 16 + 2·numGhosts).
        if (!(n > 16 + 2 * numGhosts) || !character_ || nrOfCharacters_ == 0) return;

        Buffer& buf = layer()->buffer();
        const Coord3D dims{static_cast<lengthType>(n), 1, 1};

        // The furthest Pac-Man has reached gates where the ghosts start blinking white.
        int maxBlinkPos = character_[PACMAN].topPos;
        if (maxBlinkPos < 20) maxBlinkPos = 20;
        const int startBlinkingGhostsLED =
            (n < 64) ? n / 3 : mapRange(blinkDistance, 20, 255, 20, maxBlinkPos);

        // One timing tick per ms (elapsed() advances ~each frame); aux1TimingCounter sequences the
        // blink/flash/move cadence. MoonLight uses millis()>step; elapsed() is the strip-start clock.
        if (elapsed() > step_) { step_ = elapsed(); aux1TimingCounter_++; }

        if (!smearMode) draw::fade(buf, 255);   // fadeToBlackBy(255) — full clear unless smearing

        // Trailing white "dots" behind Pac-Man's furthest reach (every LED, or every other LED).
        if (dots) {
            const int step2 = compact ? 1 : 2;
            for (int i = n - 1; i > character_[PACMAN].topPos; i -= step2)
                setRGB(buf, dims, i, WHITEISH);
        }

        // Lay the power dots out evenly from LED 10 to the end, fixed-point (everyXLeds is 8.8).
        const uint32_t everyXLeds = ((static_cast<uint32_t>(n) - 10U) << 8) / numPowerDots_;
        for (int i = 1; i < numPowerDots_; i++)
            character_[i + numGhosts + 1].pos = 10 + static_cast<int>((i * everyXLeds) >> 8);

        // Flash the power dots on/off every 10 timing ticks (toggle ORANGEYELLOW ↔ off).
        if (aux1TimingCounter_ % 10 == 0) {
            const uint32_t dotColor = (character_[numGhosts + 1].color == ORANGEYELLOW) ? 0x000000 : ORANGEYELLOW;
            for (int i = 0; i < numPowerDots_; i++) character_[i + numGhosts + 1].color = dotColor;
        }

        // While ghosts are blue and Pac-Man is within blink range, blink them white↔blue every 15.
        if (aux1TimingCounter_ % 15 == 0 && character_[1].blue && character_[PACMAN].pos <= startBlinkingGhostsLED) {
            const uint32_t gc = (character_[1].color == BLUE) ? WHITEISH : BLUE;
            for (int i = 1; i <= numGhosts; i++) character_[i].color = gc;
        }

        // Draw the uneaten power dots.
        for (int i = 0; i < numPowerDots_; i++) {
            PacManChar& d = character_[i + numGhosts + 1];
            if (!d.eaten && static_cast<unsigned>(d.pos) < static_cast<unsigned>(n))
                setRGB(buf, dims, d.pos, d.color);
        }

        // Pac-Man eats a power dot it overlaps: reverse the whole pack and turn the ghosts blue.
        for (int j = 0; j < numPowerDots_; j++) {
            PacManChar& dot = character_[j + numGhosts + 1];
            if (character_[PACMAN].pos == dot.pos && !dot.eaten) {
                for (int i = 0; i <= numGhosts; i++) character_[i].direction = false;
                for (int i = 1; i <= numGhosts; i++) { character_[i].color = BLUE; character_[i].blue = true; }
                dot.eaten = true;
                break;
            }
        }

        // Pac-Man reached the start while ghosts are blue: turn the pack back around, restore ghost
        // colours, and (if the leading dot was eaten) respawn all dots + reset the reach marker.
        if (character_[1].blue && character_[PACMAN].pos <= 0) {
            for (int i = 0; i <= numGhosts; i++) character_[i].direction = true;
            for (int i = 1; i <= numGhosts; i++) {
                character_[i].color = ghostColors[(i - 1) % 4];
                character_[i].blue = false;
            }
            if (character_[numGhosts + 1].eaten) {
                for (int i = 0; i < numPowerDots_; i++) character_[i + numGhosts + 1].eaten = false;
                character_[PACMAN].topPos = 0;
            }
        }

        // Advance positions on a speed-derived cadence: faster speed → smaller modulo → more steps.
        const int moveEvery = mapRange(speed, 0, 255, 15, 1);
        const bool updatePositions = (moveEvery > 0) && (aux1TimingCounter_ % moveEvery == 0);
        if (updatePositions) {
            character_[PACMAN].pos += character_[PACMAN].direction ? 1 : -1;
            for (int i = 1; i <= numGhosts; i++)
                character_[i].pos += character_[i].direction ? 1 : -1;
        }

        // Draw Pac-Man and the ghosts.
        if (static_cast<unsigned>(character_[PACMAN].pos) < static_cast<unsigned>(n))
            setRGB(buf, dims, character_[PACMAN].pos, character_[PACMAN].color);
        for (int i = 1; i <= numGhosts; i++)
            if (static_cast<unsigned>(character_[i].pos) < static_cast<unsigned>(n))
                setRGB(buf, dims, character_[i].pos, character_[i].color);

        // Track Pac-Man's furthest reach (the trailing-dot wall + blink gate use it).
        if (character_[PACMAN].topPos < character_[PACMAN].pos) character_[PACMAN].topPos = character_[PACMAN].pos;

        draw::blur(buf, dims, static_cast<uint8_t>(blur >> 1));  // MoonLight: blur2d(blur>>1)
    }

private:
    // One game character — Pac-Man (index PACMAN=0), the ghosts (1..numGhosts), then the power dots.
    // `topPos` is meaningful only for Pac-Man (its furthest reach). `pos` is signed (ghosts spawn at
    // negative positions and walk on). Colours are stored as 0xRRGGBB and unpacked when drawn.
    struct PacManChar {
        int      pos;
        int      topPos;
        uint32_t color;
        bool     direction;
        bool     blue;
        bool     eaten;
    };

    // Fixed game colours (0xRRGGBB), verbatim from MoonLight's PacMan.
    static constexpr uint8_t  PACMAN       = 0;
    static constexpr uint32_t ORANGEYELLOW = 0xFFCC00;
    static constexpr uint32_t PURPLEISH    = 0xB000B0;
    static constexpr uint32_t ORANGEISH    = 0xFF8800;
    static constexpr uint32_t WHITEISH     = 0x999999;
    static constexpr uint32_t YELLOW       = 0xFFFF00;
    static constexpr uint32_t BLUE         = 0x0000FF;
    static constexpr uint32_t RED          = 0xFF0000;
    static constexpr uint32_t CYAN         = 0x00FFFF;
    // The four base ghost colours, cycled by (ghostIndex-1)%4.
    static constexpr uint32_t ghostColors[4] = {RED, PURPLEISH, CYAN, ORANGEISH};

    PacManChar* character_      = nullptr;
    size_t      nrOfCharacters_ = 0;
    int         numPowerDots_   = 0;
    uint8_t     aux1TimingCounter_ = 0;
    uint32_t    step_           = 0;

    void release() {
        if (character_) { platform::free(character_); character_ = nullptr; }
        nrOfCharacters_ = 0;
        numPowerDots_   = 0;
    }

    // Allocate and seed the character array. MoonLight: numPowerDots = clamp(nrOfLights/10) to
    // [1, numPowerDotsControl]; the array holds numGhosts + numPowerDots + 1 entries. Pac-Man starts
    // yellow at 0 moving right; ghosts spawn at staggered negative positions; the leading power dot
    // sits at the far end. (initializePacMan)
    void initializePacMan() {
        const int n = static_cast<int>(nrOfLights());
        numPowerDots_ = MAXi(1, MINi(n / 10, numPowerDotsControl));
        const size_t want = static_cast<size_t>(numGhosts) + static_cast<size_t>(numPowerDots_) + 1;

        if (want != nrOfCharacters_) {
            release();
            character_ = static_cast<PacManChar*>(platform::alloc(want * sizeof(PacManChar)));
            if (!character_) { numPowerDots_ = 0; return; }
            nrOfCharacters_ = want;
        }
        // numPowerDots_ may have shrunk without the total size changing (e.g. strip got shorter but
        // ghost count grew to compensate) — recompute it regardless of whether we reallocated.
        numPowerDots_ = MAXi(1, MINi(n / 10, numPowerDotsControl));

        for (size_t i = 0; i < nrOfCharacters_; i++) character_[i] = PacManChar{0, 0, 0, true, false, false};

        if (nrOfCharacters_ > 0) {
            character_[PACMAN].color = YELLOW;
            character_[PACMAN].pos = 0;
            character_[PACMAN].topPos = 0;
            character_[PACMAN].direction = true;
            character_[PACMAN].blue = false;
        }
        const int ghostMax = MINi(numGhosts, static_cast<int>(nrOfCharacters_));
        for (int i = 1; i <= ghostMax; i++) {
            character_[i].color = ghostColors[(i - 1) % 4];
            character_[i].pos = -2 * (i + 1);
            character_[i].direction = true;
            character_[i].blue = false;
        }
        for (int i = 0; i < numPowerDots_; i++)
            if (static_cast<size_t>(i + numGhosts + 1) < nrOfCharacters_) {
                character_[i + numGhosts + 1].color = ORANGEYELLOW;
                character_[i + numGhosts + 1].eaten = false;
            }
        if (static_cast<size_t>(numGhosts + 1) < nrOfCharacters_)
            character_[numGhosts + 1].pos = n - 1;
    }

    // setRGB(idx, 0xRRGGBB) → a clipped 1D pixel write. Unpacks the packed colour into RGB.
    static void setRGB(Buffer& buf, Coord3D dims, int idx, uint32_t c) {
        draw::pixel(buf, dims, {static_cast<lengthType>(idx), 0, 0},
                    RGB{static_cast<uint8_t>((c >> 16) & 0xFF),
                        static_cast<uint8_t>((c >> 8) & 0xFF),
                        static_cast<uint8_t>(c & 0xFF)});
    }

    static int MAXi(int a, int b) { return a > b ? a : b; }
    static int MINi(int a, int b) { return a < b ? a : b; }

    // FastLED ::map for the speed/blink-distance rescales: linear (x-inLo)·(outHi-outLo)/(inHi-inLo)
    // + outLo, guarding a zero input span. FastLED's map doesn't clamp, and MoonLight relies on
    // inputs staying in range, so this matches its behaviour. // RECONSTRUCTED: FastLED's map() body.
    static int mapRange(int x, int inLo, int inHi, int outLo, int outHi) {
        const int den = inHi - inLo;
        if (den == 0) return outLo;
        return (x - inLo) * (outHi - outLo) / den + outLo;
    }
};

} // namespace mm