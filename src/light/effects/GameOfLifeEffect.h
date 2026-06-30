#pragma once

#include "light/layers/Layer.h"
#include "light/Palette.h"        // colorFromPalette, blend, fadeToBlackBy
#include "core/math8.h"           // Random8
#include "core/crc.h"             // crc16 — grid fingerprint for stasis detection
#include "platform/platform.h"    // alloc — the heap grid state

#include <cstring>

namespace mm {

// Conway's Game of Life, generalised to 2D and 3D, with selectable rulesets, palette-coloured
// cells, age colouring, a dead-cell blur trail, and self-respawn when the pattern goes static.
// A living cell survives if its live-neighbour count is in the ruleset's SURVIVE set; a dead cell
// is born if its count is in the BIRTH set. Neighbours are the 8 around a cell in 2D, the 26 in
// 3D, optionally wrapping toroidally. The board fingerprints itself each generation (crc16); when
// it falls into a short oscillation or dies out, it either respawns an R-pentomino/glider
// (`infinite`) or resets to a fresh random fill.
//
// Prior art: MoonLight's GameOfLife (E_MoonModules, MoonModules) — behaviour reproduced (rulesets,
// 2D/3D neighbourhoods, colour aging, blur, stasis respawn), written fresh on projectMM's
// EffectBase + the shared primitives (Random8, colorFromPalette, fadeToBlackBy, crc16). Conway's
// Game of Life (John Conway, 1970) is the underlying automaton.
class GameOfLifeEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🌙"; }  // MoonLight origin · MoonModules

    // Rulesets: B(orn)/S(urvive) neighbour counts. Custom reads `customRule`.
    static constexpr const char* kRulesetOptions[] = {
        "Custom", "Conway B3/S23", "HighLife B36/S23", "InverseLife B0123478/S01234678",
        "Maze B3/S12345", "Mazectric B3/S1234", "DryLife B37/S23"};
    static constexpr uint8_t kRulesetCount = 7;

    uint8_t  ruleset   = 1;     // index into kRulesetOptions (default Conway)
    char     customRule[20] = "B3/S23";
    uint8_t  speed     = 20;    // generations per second (0 = as fast as the tick)
    uint8_t  density   = 30;    // initial live fraction, 10..90 %
    uint8_t  mutation  = 5;     // % chance a newborn gets a fresh random colour instead of inheriting
    bool     wrap      = true;  // toroidal edges
    bool     colorByAge = false; // green newborn → red as it ages, instead of palette colour
    bool     infinite  = true;  // respawn on stasis instead of full reset
    uint8_t  blur       = 128;  // dead-cell fade toward black per render (0 = snap off, 255 = long trail)

    void onBuildControls() override {
        controls_.addSelect("ruleset", ruleset, kRulesetOptions, kRulesetCount);
        controls_.addText("customRule", customRule, sizeof(customRule));
        controls_.addUint8("speed", speed, 0, 100);
        controls_.addUint8("density", density, 10, 90);
        controls_.addUint8("mutation", mutation, 0, 100);
        controls_.addBool("wrap", wrap);
        controls_.addBool("colorByAge", colorByAge);
        controls_.addBool("infinite", infinite);
        controls_.addUint8("blur", blur, 0, 255);
    }

    // Grid state lives on the heap (cells + next-gen + per-cell colour), sized to the light count.
    // Bit-packed alive/dead keeps it small (16K cells = 2KB each plane); colours are one byte each.
    // Off the hot path (cf. Fire's heat_) — never an inline member, so sizeof(GameOfLife) stays tiny.
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

    // --- Test seams: drive the automaton deterministically without a Layer/clock. allocateForTest
    // sizes the grid; setCellForTest seeds a pattern; stepForTest runs one generation; isAliveForTest
    // reads a cell. parseRulesetForTest exposes the B/S parser. (The render/colour path is exercised
    // by the shared effect-render scenario; these pin the AUTOMATON, which is the hard part.)
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
    void stepForTest() { parseRuleset(); evolve(testW_, testH_, testD_); }
    void parseRulesetForTest() { parseRuleset(); }
    bool birthForTest(uint8_t n) const { return birth_[n]; }
    bool surviveForTest(uint8_t n) const { return survive_[n]; }

    void loop() override {
        if (!cells_ || cellCount_ == 0) return;
        const lengthType w = width(), h = height(), d = depth();
        const uint8_t cpl = channelsPerLight();
        if (w == 0 || h == 0 || d == 0 || cpl == 0) return;

        parseRuleset();

        if (generation_ == 0) randomFill(w, h, d);

        // Advance a generation only when the speed interval has elapsed; otherwise just re-render
        // (the blur trail keeps animating between generations).
        const uint32_t now = elapsed();
        const uint32_t stepMs = speed ? (1000u / speed) : 0;
        if (generation_ <= 1 || now - lastStepMs_ >= stepMs) {
            lastStepMs_ = now;
            evolve(w, h, d);
            checkStasis(w, h, d);
            generation_++;
        }
        render(w, h, d, cpl);
    }

private:
    uint8_t* cells_  = nullptr;   // bit-packed alive/dead, current generation
    uint8_t* future_ = nullptr;   // bit-packed, next generation (swapped in)
    uint8_t* colors_ = nullptr;   // palette index (or age) per cell
    nrOfLightsType cellCount_ = 0;
    size_t   planeBytes_ = 0;
    uint32_t generation_ = 0;
    uint32_t lastStepMs_ = 0;
    Random8  rng_{0x6C0FFEE5u};

    bool     birth_[27]   = {};   // birth_[n]   = a dead cell with n live neighbours is born
    bool     survive_[27] = {};   // survive_[n] = a live cell with n live neighbours survives
    uint16_t lastCrc_ = 0;        // previous generation's fingerprint
    uint16_t prevCrc_ = 0;        // the one before — catches period-2 oscillators
    lengthType testW_ = 0, testH_ = 0, testD_ = 0;  // test-seam grid dims (see allocateForTest)

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

    // Parse "B#/S#" (e.g. "B36/S23") into the birth/survive sets. A preset string is parsed the
    // same way as the custom one — one code path. Digits after B are birth counts, after S survive.
    void parseRuleset() {
        const char* r = (ruleset == 0) ? customRule : kRulesetOptions[ruleset];
        // Presets are stored as "Name B.../S..."; find the 'B'. Custom is just "B.../S...".
        const char* b = std::strchr(r, 'B');
        std::memset(birth_, 0, sizeof(birth_));
        std::memset(survive_, 0, sizeof(survive_));
        if (!b) { birth_[3] = survive_[2] = survive_[3] = true; return; }  // fall back to Conway
        bool* set = birth_;
        for (const char* p = b + 1; *p; p++) {
            if (*p == 'S' || *p == 's') { set = survive_; continue; }
            if (*p >= '0' && *p <= '9') { const uint8_t n = static_cast<uint8_t>(*p - '0'); if (n < 27) set[n] = true; }
        }
    }

    void randomFill(lengthType w, lengthType h, lengthType d) {
        std::memset(cells_, 0, planeBytes_);
        for (nrOfLightsType i = 0; i < cellCount_; i++) {
            const bool alive = rng_.below(100) < density;
            setBit(cells_, i, alive);
            colors_[i] = alive ? rng_.next8() : 0;   // a random palette index per live cell
        }
        (void)w; (void)h; (void)d;
        generation_ = 1;
        lastCrc_ = prevCrc_ = 0;
    }

    // Count live neighbours of (x,y,z): the 8 around it in 2D (d==1), the 26 in 3D. `wrap` makes
    // the edges toroidal; otherwise off-grid neighbours count as dead.
    uint8_t liveNeighbours(lengthType x, lengthType y, lengthType z,
                           lengthType w, lengthType h, lengthType d) const {
        uint8_t n = 0;
        const lengthType zlo = d > 1 ? -1 : 0, zhi = d > 1 ? 1 : 0;
        for (lengthType dz = zlo; dz <= zhi; dz++)
            for (lengthType dy = -1; dy <= 1; dy++)
                for (lengthType dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0 && dz == 0) continue;
                    lengthType nx = x + dx, ny = y + dy, nz = z + dz;
                    if (wrap) {
                        nx = static_cast<lengthType>((nx + w) % w);
                        ny = static_cast<lengthType>((ny + h) % h);
                        if (d > 1) nz = static_cast<lengthType>((nz + d) % d);
                    } else if (nx < 0 || ny < 0 || nz < 0 || nx >= w || ny >= h || nz >= d) {
                        continue;
                    }
                    if (getBit(cells_, idx(nx, ny, nz, w, h))) n++;
                }
        return n;
    }

    // Build the next generation in future_, then swap. Newborns inherit a neighbour's colour (with
    // a `mutation`% chance of a fresh one); survivors keep theirs; dead cells keep their colour byte
    // for the blur trail to fade.
    void evolve(lengthType w, lengthType h, lengthType d) {
        for (lengthType z = 0; z < d; z++)
            for (lengthType y = 0; y < h; y++)
                for (lengthType x = 0; x < w; x++) {
                    const nrOfLightsType i = idx(x, y, z, w, h);
                    const uint8_t n = liveNeighbours(x, y, z, w, h, d);
                    const bool alive = getBit(cells_, i);
                    bool next;
                    if (alive) next = survive_[n];
                    else       next = birth_[n];
                    setBit(future_, i, next);
                    if (next && !alive) {
                        // Birth: inherit a colour (mutate sometimes). Cheap "inherit": reuse this
                        // cell's own last colour if it has one, else a fresh random index.
                        colors_[i] = (rng_.below(100) < mutation || colors_[i] == 0)
                                     ? rng_.next8() : colors_[i];
                    }
                }
        std::memcpy(cells_, future_, planeBytes_);
    }

    // Fingerprint the grid; if it matches either of the last two generations the pattern is static
    // or a short oscillator → respawn (infinite) or reset.
    void checkStasis(lengthType w, lengthType h, lengthType d) {
        const uint16_t crc = crc16(cells_, planeBytes_);
        const bool stuck = (crc == lastCrc_ || crc == prevCrc_);
        prevCrc_ = lastCrc_;
        lastCrc_ = crc;
        if (!stuck) return;
        if (infinite) respawn(w, h, d);
        else generation_ = 0;   // full random refill next loop
    }

    // Drop an R-pentomino (a famously long-lived methuselah) at a random spot to re-energise a
    // stalled board without wiping it.
    void respawn(lengthType w, lengthType h, lengthType d) {
        const lengthType cx = static_cast<lengthType>(rng_.below(static_cast<uint8_t>(w > 255 ? 255 : w)));
        const lengthType cy = static_cast<lengthType>(rng_.below(static_cast<uint8_t>(h > 255 ? 255 : h)));
        const lengthType cz = d > 1 ? static_cast<lengthType>(rng_.below(static_cast<uint8_t>(d))) : 0;
        // R-pentomino:  .XX
        //               XX.
        //               .X.
        static constexpr int8_t cells[5][2] = {{0, 0}, {1, 0}, {-1, 1}, {0, 1}, {0, 2}};
        for (auto& c : cells) {
            const lengthType x = static_cast<lengthType>((cx + c[0] + w) % w);
            const lengthType y = static_cast<lengthType>((cy + c[1] + h) % h);
            const nrOfLightsType i = idx(x, y, cz, w, h);
            setBit(cells_, i, true);
            colors_[i] = rng_.next8();
        }
    }

    // Draw the board: live cells get their palette colour (or an age ramp); dead cells fade toward
    // black by `blur` so a dying pattern leaves a brief trail rather than snapping off.
    void render(lengthType w, lengthType h, lengthType d, uint8_t cpl) {
        uint8_t* buf = buffer();
        for (lengthType z = 0; z < d; z++)
            for (lengthType y = 0; y < h; y++)
                for (lengthType x = 0; x < w; x++) {
                    const nrOfLightsType i = idx(x, y, z, w, h);
                    const size_t off = static_cast<size_t>(i) * cpl;
                    if (getBit(cells_, i)) {
                        RGB c = colorByAge
                            ? RGB{0, 255, 0}   // newborn/living green (age→red is a future refinement)
                            : colorFromPalette(*Palettes::active(), colors_[i]);
                        if (cpl >= 1) buf[off + 0] = c.r;
                        if (cpl >= 2) buf[off + 1] = c.g;
                        if (cpl >= 3) buf[off + 2] = c.b;
                    } else {
                        // Dead: fade the existing buffer contents toward black (the blur trail).
                        if (cpl >= 3) {
                            RGB c{buf[off + 0], buf[off + 1], buf[off + 2]};
                            fadeToBlackBy(c, static_cast<uint8_t>(255 - blur));
                            buf[off + 0] = c.r; buf[off + 1] = c.g; buf[off + 2] = c.b;
                        } else if (cpl >= 1) {
                            buf[off + 0] = scale8(buf[off + 0], blur);
                        }
                    }
                }
    }
};

} // namespace mm
