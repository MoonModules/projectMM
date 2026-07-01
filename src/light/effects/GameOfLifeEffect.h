#pragma once

#include "light/layers/Layer.h"   // layer()->buffer()
#include "light/draw.h"           // draw::pixel/blendPixel/get — setRGB/blendColor/getRGB
#include "light/Palette.h"        // colorFromPalette, Palettes::active
#include "core/math8.h"           // Random8
#include "core/crc.h"             // crc16 — grid fingerprint for stasis detection
#include "platform/platform.h"    // alloc — the heap grid state

#include <cstring>

namespace mm {

// Conway's Game of Life, generalised to 2D and 3D, with selectable rulesets, palette-coloured
// cells that inherit a living neighbour's colour on birth, optional green→red age colouring, a
// dead-cell blur trail that fades toward a configurable background colour, a 1.5 s settle pause on
// each new game, and self-respawn (R-pentomino / glider) when the pattern goes static. A living
// cell survives if its live-neighbour count is in the ruleset's SURVIVE set; a dead cell is born if
// its count is in the BIRTH set. Neighbours are the 8 around a cell in 2D, the 26 in 3D, optionally
// wrapping toroidally. The board fingerprints itself (crc16) at three periods — every 16 gens
// (oscillators), every lcm(h,w)·4 gens (spaceships), every that·6 (cube gliders) — and respawns or
// resets when a fingerprint recurs, dies out, density floors, or at random.
//
// Prior art: MoonLight's GameOfLife (E_MoonModules, MoonModules; Ewoud Wijma 2022 after
// natureofcode ch.7 + DougHaber/nlife-color, Brandon Butler / @Brandon502 2024) — its behaviour is
// reproduced here (rulesets, 2D/3D neighbourhoods, neighbour-colour inheritance, age colouring,
// background blur, 3-CRC stasis, R-pentomino respawn, settle pause), written fresh on projectMM's
// EffectBase + shared primitives (Random8, colorFromPalette, draw::, crc16). Conway's Game of Life
// (John Conway, 1970) is the underlying automaton.
class GameOfLifeEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🌙"; }  // MoonLight origin · MoonModules

    // Rulesets: index → B(orn)/S(urvive) string. Index 0 reads customRuleString. The label is
    // descriptive only; parsing reads the digits around the '/' (see parseRuleset).
    static constexpr const char* kRulesetOptions[] = {
        "Custom B/S",
        "Conway's Game of Life B3/S23",
        "HighLife B36/S23",
        "InverseLife B0123478/S34678",
        "Maze B3/S12345",
        "Mazecentric B3/S1234",
        "DrighLife B367/S23"};
    static constexpr uint8_t kRulesetCount = 7;

    // The B/S string a given ruleset parses (index 0 = custom). Kept separate from the UI label so
    // a preset parses exactly its rule, not the words in its menu entry.
    static constexpr const char* kRulesetStrings[] = {
        nullptr,            // 0: custom → customRuleString
        "B3/S23",           // Conway
        "B36/S23",          // HighLife
        "B0123478/S34678",  // InverseLife
        "B3/S12345",        // Maze
        "B3/S1234",         // Mazecentric
        "B367/S23"};        // DrighLife

    // Defaults match MoonLight's E_MoonModules GameOfLife exactly.
    uint8_t backgroundColorR = 0, backgroundColorG = 0, backgroundColorB = 0;  // bgC {0,0,0}
    uint8_t ruleset    = 1;            // Conway
    char    customRuleString[20] = "B/S";
    uint8_t speed      = 20;           // GameSpeed (FPS), 0..100 (100 = uncapped)
    uint8_t lifeChance = 32;           // startingLifeDensity, 10..90 %
    uint8_t mutation   = 2;            // mutationChance, 0..100 %
    bool    wrap       = true;
    bool    disablePause = false;
    bool    colorByAge = false;
    bool    infinite   = true;
    uint8_t blur       = 128;

    void onBuildControls() override {
        // MoonLight's bgC is a Coord3D 0..255 read as RGB. projectMM has no colour control, so the
        // three components are three uint8s — the native, recognisable shape for an RGB triple here.
        controls_.addUint8("backgroundColorR", backgroundColorR, 0, 255);
        controls_.addUint8("backgroundColorG", backgroundColorG, 0, 255);
        controls_.addUint8("backgroundColorB", backgroundColorB, 0, 255);
        controls_.addSelect("ruleset", ruleset, kRulesetOptions, kRulesetCount);
        controls_.addText("customRuleString", customRuleString, sizeof(customRuleString));
        controls_.addUint8("GameSpeed (FPS)", speed, 0, 100);
        controls_.addUint8("startingLifeDensity", lifeChance, 10, 90);
        controls_.addUint8("mutationChance", mutation, 0, 100);
        controls_.addBool("wrap", wrap);
        controls_.addBool("disablePause", disablePause);
        controls_.addBool("colorByAge", colorByAge);
        controls_.addBool("infinite", infinite);
        controls_.addUint8("blur", blur, 0, 255);
    }

    // Grid state lives on the heap (cells + next-gen + per-cell colour), sized to the light count.
    // Bit-packed alive/dead keeps it small (16K cells = 2KB each plane); colours are one byte each.
    // Off the hot path (cf. Fire's heat_) — never an inline member, so sizeof(GameOfLife) stays tiny
    // (an inline array here caused a P4 stack-overflow bootloop with HueDriver).
    void onBuildState() override {
        const nrOfLightsType count = nrOfLights();
        if (enabled() && count > 0) {
            const size_t planeBytes = (static_cast<size_t>(count) + 7) / 8;
            if (count != cellCount_) {
                release();
                cells_  = static_cast<uint8_t*>(platform::alloc(planeBytes));
                future_ = static_cast<uint8_t*>(platform::alloc(planeBytes));
                colors_ = static_cast<uint8_t*>(platform::alloc(count));
                if (cells_ && future_ && colors_) {
                    cellCount_ = count;
                    planeBytes_ = planeBytes;
                    generation_ = 0;   // force a fresh fill on the next loop
                } else {
                    release();
                }
            }
        } else {
            release();
        }
        setDynamicBytes(cellCount_ ? planeBytes_ * 2 + cellCount_ : 0);
    }

    void teardown() override { release(); setDynamicBytes(0); }
    ~GameOfLifeEffect() override { release(); }

    void onUpdate(const char* name) override {
        if (std::strcmp(name, "ruleset") == 0 || std::strcmp(name, "customRuleString") == 0)
            parseRuleset();
    }

    // --- Test seams: drive the automaton deterministically without a Layer/clock. allocateForTest
    // sizes the grid; setCellForTest seeds a pattern; stepForTest runs one generation; isAliveForTest
    // reads a cell. parseRulesetForTest / birthForTest / surviveForTest expose the B/S parser.
    bool allocateForTest(lengthType w, lengthType h, lengthType d) {
        testW_ = w; testH_ = h; testD_ = d;
        const nrOfLightsType count = static_cast<nrOfLightsType>(w) * h * d;
        const size_t planeBytes = (static_cast<size_t>(count) + 7) / 8;
        release();
        cells_  = static_cast<uint8_t*>(platform::alloc(planeBytes));
        future_ = static_cast<uint8_t*>(platform::alloc(planeBytes));
        colors_ = static_cast<uint8_t*>(platform::alloc(count));
        if (!cells_ || !future_ || !colors_) { release(); return false; }
        cellCount_ = count; planeBytes_ = planeBytes;
        std::memset(cells_, 0, planeBytes); std::memset(future_, 0, planeBytes); std::memset(colors_, 0, count);
        generation_ = 1;   // skip the random-fill path
        return true;
    }
    void setCellForTest(lengthType x, lengthType y, lengthType z, bool on) {
        setBit(cells_, idx(x, y, z, testW_, testH_), on);
    }
    bool isAliveForTest(lengthType x, lengthType y, lengthType z) const {
        return getBit(cells_, idx(x, y, z, testW_, testH_));
    }
    void stepForTest() { parseRuleset(); evolveAutomaton(testW_, testH_, testD_, true); }
    void parseRulesetForTest() { parseRuleset(); }
    bool birthForTest(uint8_t n) const { return birthNumbers_[n]; }
    bool surviveForTest(uint8_t n) const { return surviveNumbers_[n]; }

    void loop() override {
        if (!cells_ || !future_ || !colors_ || cellCount_ == 0) return;
        const lengthType w = width(), h = height(), d = depth();
        const uint8_t cpl = channelsPerLight();
        if (w == 0 || h == 0 || d == 0 || cpl == 0) return;

        parseRuleset();

        // generation 0 = "between games": wait out the settle/respawn delay, then start fresh and
        // show the initial fill before the first step (MoonLight: gen==0 && step<millis()).
        if (generation_ == 0) {
            if (now() < step_) { renderInitial(w, h, d); return; }
            startNewGame(w, h, d);
            renderInitial(w, h, d);
            return;
        }

        Buffer& buf = layer()->buffer();
        const Coord3D dims{w, h, d};
        const RGB bg{backgroundColorR, backgroundColorG, backgroundColorB};

        // blur>220 (&& !colorByAge) keeps a faded background instead of fully clearing dead cells:
        // raise a floor and pull blur back under 220 for this frame.
        int fadedBackground = 0;
        uint8_t frameBlur = blur;
        if (blur > 220 && !colorByAge) {
            fadedBackground = bg.r + bg.g + bg.b + 20 + (blur - 220);
            frameBlur = static_cast<uint8_t>(blur - (blur - 220));  // == 220
        }
        const bool blurDead = step_ > now() && !fadedBackground;  // still in the settle pause

        // Redraw pass: paints the just-placed fill, ages paused cells, blurs dead cells while paused.
        if (generation_ <= 1 || blurDead) {
            for (lengthType z = 0; z < d; z++)
                for (lengthType y = 0; y < h; y++)
                    for (lengthType x = 0; x < w; x++) {
                        const nrOfLightsType i = idx(x, y, z, w, h);
                        const Coord3D p{x, y, z};
                        const bool alive = getBit(cells_, i);
                        const bool recolor = alive && generation_ == 1 && colors_[i] == 0 && !rng_.below(16);
                        if (alive && recolor) {
                            colors_[i] = rng_.below(1, 255);
                            draw::pixel(buf, dims, p, liveColor(colors_[i]));
                        } else if (alive && colorByAge && generation_ == 0) {
                            draw::blendPixel(buf, dims, p, RGB{255, 0, 0}, 248);  // age while paused
                        } else if (alive && colors_[i] != 0) {
                            draw::pixel(buf, dims, p, liveColor(colors_[i]));
                        } else if (!alive && blurDead) {
                            draw::blendPixel(buf, dims, p, bg, frameBlur);   // blur dead while paused
                        } else if (!alive && generation_ == 1) {
                            draw::blendPixel(buf, dims, p, bg, 248);         // fade dead on new game
                        }
                    }
        }

        // Speed throttle: 100 runs uncapped; otherwise advance only once 1000/speed ms have passed.
        if (!speed || step_ > now() || (speed != 100 && now() - step_ < 1000u / speed)) return;

        evolveAutomaton(w, h, d, false, &buf, dims, bg, frameBlur, fadedBackground);
    }

private:
    uint8_t* cells_  = nullptr;   // bit-packed alive/dead, current generation
    uint8_t* future_ = nullptr;   // bit-packed, next generation (swapped in)
    uint8_t* colors_ = nullptr;   // palette index (or 0 = dead) per cell
    nrOfLightsType cellCount_ = 0;
    size_t   planeBytes_ = 0;

    uint32_t generation_ = 0;
    uint32_t step_ = 0;           // ms timestamp gating the next step / settle pause
    Random8  rng_{0x6C0FFEE5u};

    bool     birthNumbers_[9]   = {};   // birthNumbers_[n]   = a dead cell with n live neighbours is born
    bool     surviveNumbers_[9] = {};   // surviveNumbers_[n] = a live cell with n live neighbours survives

    // Three stasis fingerprints sampled at three periods, plus the solo-glider flag.
    uint16_t oscillatorCRC_ = 0, spaceshipCRC_ = 0, cubeGliderCRC_ = 0;
    uint16_t gliderLength_ = 0, cubeGliderLength_ = 0;
    bool     soloGlider_ = false;

    lengthType testW_ = 0, testH_ = 0, testD_ = 0;  // test-seam grid dims (see allocateForTest)

    uint32_t now() const { return elapsed(); }

    void release() {
        if (cells_)  { platform::free(cells_);  cells_  = nullptr; }
        if (future_) { platform::free(future_); future_ = nullptr; }
        if (colors_) { platform::free(colors_); colors_ = nullptr; }
        cellCount_ = 0; planeBytes_ = 0;
    }

    // --- bit-packed cell access ---
    static bool getBit(const uint8_t* plane, nrOfLightsType i) { return (plane[i >> 3] >> (i & 7)) & 1; }
    static void setBit(uint8_t* plane, nrOfLightsType i, bool on) {
        const uint8_t m = static_cast<uint8_t>(1u << (i & 7));
        if (on) plane[i >> 3] |= m; else plane[i >> 3] = static_cast<uint8_t>(plane[i >> 3] & ~m);
    }
    static nrOfLightsType idx(lengthType x, lengthType y, lengthType z, lengthType w, lengthType h) {
        return static_cast<nrOfLightsType>((static_cast<size_t>(z) * h + y) * w + x);
    }

    // A live cell's colour: green when colorByAge (it ages toward red), else its palette colour.
    RGB liveColor(uint8_t colorIndex) const {
        return colorByAge ? RGB{0, 255, 0} : colorFromPalette(*Palettes::active(), colorIndex);
    }

    // Parse "B#/S#" into the birth/survive sets. Digits 0..8 before the '/' are birth counts, after
    // are survive counts — no 'B'/'S' letters required, so a user typing "36/23" works. Matches
    // MoonLight: index into the slash, classify each digit by side.
    void parseRuleset() {
        const char* r = (ruleset == 0) ? customRuleString
                                       : (ruleset < kRulesetCount ? kRulesetStrings[ruleset] : kRulesetStrings[1]);
        std::memset(birthNumbers_, 0, sizeof(birthNumbers_));
        std::memset(surviveNumbers_, 0, sizeof(surviveNumbers_));
        if (!r) return;
        const char* slash = std::strchr(r, '/');
        const long slashIndex = slash ? (slash - r) : -1;
        for (const char* p = r; *p; p++) {
            const int num = *p - '0';
            if (num >= 0 && num < 9) {
                if (slashIndex >= 0 && (p - r) < slashIndex) birthNumbers_[num] = true;
                else                                          surviveNumbers_[num] = true;
            }
        }
    }

    // Integer gcd/lcm — gliderLength = lcm(h,w)*4 (the spaceship sampling period).
    static uint32_t gcd(uint32_t a, uint32_t b) { while (b) { const uint32_t t = a % b; a = b; b = t; } return a; }
    static uint32_t lcm(uint32_t a, uint32_t b) { if (!a || !b) return 0; return a / gcd(a, b) * b; }

    // Begin a new game: random fill at lifeChance density, seed the three CRCs from the fill, set the
    // settle pause (1.5 s unless disablePause), reset glider state. MoonLight: startNewGameOfLife.
    void startNewGame(lengthType w, lengthType h, lengthType d) {
        generation_ = 1;
        step_ = disablePause ? now() : now() + 1500;

        std::memset(cells_, 0, planeBytes_);
        std::memset(colors_, 0, cellCount_);

        Buffer& buf = layer()->buffer();
        const Coord3D dims{w, h, d};
        for (lengthType z = 0; z < d; z++)
            for (lengthType y = 0; y < h; y++)
                for (lengthType x = 0; x < w; x++) {
                    if (rng_.below(100) < lifeChance) {
                        const nrOfLightsType i = idx(x, y, z, w, h);
                        setBit(cells_, i, true);
                        colors_[i] = rng_.below(1, 255);  // never 0 (0 = dead marker)
                        draw::pixel(buf, dims, {x, y, z}, liveColor(colors_[i]));
                    }
                }
        std::memcpy(future_, cells_, planeBytes_);

        soloGlider_ = false;
        const uint16_t crc = crc16(cells_, planeBytes_);
        oscillatorCRC_ = spaceshipCRC_ = cubeGliderCRC_ = crc;
        gliderLength_ = static_cast<uint16_t>(lcm(static_cast<uint32_t>(h), static_cast<uint32_t>(w)) * 4);
        cubeGliderLength_ = static_cast<uint16_t>(gliderLength_ * 6);  // rectangular-cuboid case left as-is
    }

    // Repaint every live cell on a fresh fill — the "show the start" frame between games while the
    // settle timer runs. (MoonLight relies on the redraw loop; here the cells/colours are already
    // set by startNewGame, so painting them is a straight pass.)
    void renderInitial(lengthType w, lengthType h, lengthType d) {
        Buffer& buf = layer()->buffer();
        const Coord3D dims{w, h, d};
        for (lengthType z = 0; z < d; z++)
            for (lengthType y = 0; y < h; y++)
                for (lengthType x = 0; x < w; x++) {
                    const nrOfLightsType i = idx(x, y, z, w, h);
                    if (getBit(cells_, i) && colors_[i] != 0)
                        draw::pixel(buf, dims, {x, y, z}, liveColor(colors_[i]));
                }
    }

    // Place an R-pentomino (1/5 chance a glider), up to 100 attempts avoiding overlap; bounds and the
    // z-plane pick match MoonLight's placePentomino. Writes both future_ and the buffer.
    void placePentomino(lengthType w, lengthType h, lengthType d, Buffer* buf, Coord3D dims) {
        // R-pentomino offsets; pattern[0][1] becomes 3 for the glider variant.
        uint8_t pattern[5][2] = {{1, 0}, {0, 1}, {1, 1}, {2, 1}, {2, 2}};
        if (!rng_.below(5)) pattern[0][1] = 3;
        const uint8_t colorIndex = rng_.next8();
        const RGB color = colorFromPalette(*Palettes::active(), colorIndex);

        // random8(1, size-N) needs size>N; guard tiny grids (degenerate axes collapse to 0).
        const uint8_t xHi = w > 3 ? static_cast<uint8_t>(w > 258 ? 255 : w - 3) : 1;
        const uint8_t yHi = h > 5 ? static_cast<uint8_t>(h > 260 ? 255 : h - 5) : 1;

        for (int attempts = 0; attempts < 100; attempts++) {
            const lengthType x = static_cast<lengthType>(xHi > 1 ? rng_.below(1, xHi) : 0);
            const lengthType y = static_cast<lengthType>(yHi > 1 ? rng_.below(1, yHi) : 0);
            const lengthType z = static_cast<lengthType>(d > 1 ? rng_.below(2) * (d - 1) : 0);
            bool canPlace = true;
            for (int i = 0; i < 5; i++) {
                const lengthType nx = static_cast<lengthType>(x + pattern[i][0]);
                const lengthType ny = static_cast<lengthType>(y + pattern[i][1]);
                if (nx >= w || ny >= h) continue;
                if (getBit(future_, idx(nx, ny, z, w, h))) { canPlace = false; break; }
            }
            if (canPlace || attempts == 99) {
                for (int i = 0; i < 5; i++) {
                    const lengthType nx = static_cast<lengthType>(x + pattern[i][0]);
                    const lengthType ny = static_cast<lengthType>(y + pattern[i][1]);
                    if (nx >= w || ny >= h) continue;
                    const nrOfLightsType i2 = idx(nx, ny, z, w, h);
                    setBit(future_, i2, true);
                    // Record the cell's colour index so later neighbour-colour inheritance sees a
                    // live (non-zero marker) colour for these injected cells, not 0 (dead). Drawn
                    // green under colorByAge, but colors_ still carries the palette index it ages from.
                    colors_[i2] = colorIndex;
                    if (buf) draw::pixel(*buf, dims, {nx, ny, z}, colorByAge ? RGB{0, 255, 0} : color);
                }
                return;
            }
        }
    }

    // One generation: count neighbours (collecting up to 9 neighbour colours for inheritance), apply
    // the rules into future_, paint each cell, then run the 3-CRC stasis + respawn / reset logic.
    // `testMode` skips rendering and the timing/respawn rendering side-effects (test seam path);
    // buf/dims/bg/frameBlur/fadedBackground are only read off the test path.
    void evolveAutomaton(lengthType w, lengthType h, lengthType d, bool testMode,
                         Buffer* buf = nullptr, Coord3D dims = {}, RGB bg = {},
                         uint8_t frameBlur = 0, int fadedBackground = 0) {
        int aliveCount = 0, deadCount = 0;
        const int zAxis = (d > 1) ? 1 : 0;
        const bool disableWrap = !wrap || soloGlider_ || generation_ % 1500 == 0 || zAxis;

        for (lengthType x = 0; x < w; x++)
            for (lengthType y = 0; y < h; y++)
                for (lengthType z = 0; z < d; z++) {
                    const nrOfLightsType cIndex = idx(x, y, z, w, h);
                    const bool cellValue = getBit(cells_, cIndex);
                    if (cellValue) aliveCount++; else deadCount++;

                    uint8_t neighbors = 0, colorCount = 0;
                    uint8_t nColors[9];
                    for (int i = -1; i <= 1; i++)
                        for (int j = -1; j <= 1; j++)
                            for (int k = -zAxis; k <= zAxis; k++) {
                                if (i == 0 && j == 0 && k == 0) continue;
                                lengthType nx = static_cast<lengthType>(x + i);
                                lengthType ny = static_cast<lengthType>(y + j);
                                lengthType nz = static_cast<lengthType>(z + k);
                                if (nx < 0 || ny < 0 || nz < 0 || nx >= w || ny >= h || nz >= d) {
                                    if (disableWrap) continue;
                                    nx = static_cast<lengthType>((nx + w) % w);
                                    ny = static_cast<lengthType>((ny + h) % h);
                                    nz = static_cast<lengthType>((nz + d) % d);
                                }
                                const nrOfLightsType nIndex = idx(nx, ny, nz, w, h);
                                if (getBit(cells_, nIndex)) {
                                    neighbors++;
                                    if (cellValue || colorByAge) continue;  // colour not needed
                                    if (colors_[nIndex] == 0) continue;      // dead-marker colour
                                    nColors[colorCount % 9] = colors_[nIndex];
                                    colorCount++;
                                }
                            }

                    const Coord3D p{x, y, z};
                    // B/S rulesets are single-digit (0..8, classic Conway notation), so the tables
                    // are sized 9. In 3D the 3×3×3 neighbourhood yields up to 26 neighbours; a count
                    // ≥9 is in no single-digit ruleset, so it reads as "not a birth/survive count"
                    // (the cell dies / stays dead) — clamp the lookup to avoid the OOB table read.
                    const bool survives = neighbors < 9 && surviveNumbers_[neighbors];
                    const bool born     = neighbors < 9 && birthNumbers_[neighbors];
                    if (cellValue && !survives) {
                        // Loneliness / overpopulation: dies, blur toward background.
                        setBit(future_, cIndex, false);
                        if (!testMode && buf) draw::blendPixel(*buf, dims, p, bg, frameBlur);
                    } else if (!cellValue && born) {
                        // Reproduction: inherit a living neighbour's colour, mutate sometimes.
                        setBit(future_, cIndex, true);
                        uint8_t colorIndex = (colorCount > 0) ? nColors[rng_.below(colorCount)] : rng_.next8();
                        if (rng_.below(100) < mutation) colorIndex = rng_.next8();
                        colors_[cIndex] = colorIndex;
                        if (!testMode && buf) draw::pixel(*buf, dims, p, liveColor(colorIndex));
                    } else {
                        // Unchanged cell: dead → blur (honour the faded-background floor); live →
                        // age toward red, or repaint its palette colour.
                        if (!cellValue) {
                            setBit(future_, cIndex, false);
                            if (!testMode && buf) {
                                if (fadedBackground) {
                                    const RGB val = draw::get(*buf, dims, p);
                                    if (fadedBackground < val.r + val.g + val.b)
                                        draw::blendPixel(*buf, dims, p, bg, frameBlur);
                                } else {
                                    draw::blendPixel(*buf, dims, p, bg, frameBlur);
                                }
                            }
                        } else {
                            setBit(future_, cIndex, true);
                            if (!testMode && buf) {
                                if (colorByAge) draw::blendPixel(*buf, dims, p, RGB{255, 0, 0}, 248);
                                else            draw::pixel(*buf, dims, p, liveColor(colors_[cIndex]));
                            }
                        }
                    }
                }

        soloGlider_ = (aliveCount == 5);
        std::memcpy(cells_, future_, planeBytes_);

        // Test seam runs the pure automaton only: a deterministic block/blinker must not be perturbed
        // by the stasis/respawn machinery (which would fire on its low density and fixed RNG).
        if (testMode) return;

        const uint16_t crc = crc16(cells_, planeBytes_);

        bool repetition = false;
        if (!aliveCount || crc == oscillatorCRC_ || crc == spaceshipCRC_ || crc == cubeGliderCRC_)
            repetition = true;

        // Respawn triggers: stasis, a 1/50 random nudge, or density floor under 5% (integer form of
        // float(alive)/(alive+dead) < 0.05 → alive*20 < alive+dead).
        const int total = aliveCount + deadCount;
        const bool densityFloor = total > 0 && aliveCount * 20 < total;
        if ((repetition && infinite) || (infinite && !rng_.below(50)) || (infinite && densityFloor)) {
            placePentomino(w, h, d, testMode ? nullptr : buf, dims);
            std::memcpy(cells_, future_, planeBytes_);
            repetition = false;
        }
        if (repetition) {
            generation_ = 0;
            step_ = disablePause ? now() : now() + 1000;
            return;
        }

        // Periodic CRC sampling: oscillators every 16 gens, spaceships every gliderLength, cube
        // gliders every cubeGliderLength.
        if (generation_ % 16 == 0) oscillatorCRC_ = crc;
        if (gliderLength_ && generation_ % gliderLength_ == 0) spaceshipCRC_ = crc;
        if (cubeGliderLength_ && generation_ % cubeGliderLength_ == 0) cubeGliderCRC_ = crc;
        generation_++;
        step_ = now();
    }
};

} // namespace mm
