#pragma once

#include "light/layers/Layer.h"
#include "core/color.h"
#include "platform/platform.h"

#include <cstring>

namespace mm {

// Conway's Game of Life on the XY plane (B3/S23). Two cell grids (cur/nxt) hold
// one byte per cell (0 dead, 1 alive); the step reads cur and writes nxt, then
// swaps. On extinction (no live cells) or stasis (no cell changed) the grid
// re-seeds from the PRNG so the effect never stops.
//
// Scope is deliberately the minimal classic rule — MoonLight's E_MoonModules
// GameOfLife adds rulesets, palette colouring, blur, mutation and pentomino
// seeding; those are out by design (concrete first). The simulation step is
// decoupled from colouring (one render line), so a future ruleset control or
// palette swap is a localised change. Prior art: MoonLight (Ewoud Wijma 2022,
// Brandon Butler 2024) and projectMM v1's GameOfLifeEffect.
class GameOfLifeEffect : public EffectBase {
public:
    const char* tags() const override { return "🔬🌙"; }  // cellular automaton · MoonLight / v1 lineage
    // Iterates y and x only; Layer::extrude fills z on 3D layers. The cell grids
    // cover only the z=0 plane (w*h), not the full 3D buffer.
    Dim dimensions() const override { return Dim::D2; }

    uint8_t seed = 42;
    bool wraparound = false;
    uint8_t hue = 160;
    uint8_t bpm = 60;  // generation rate; ≈ bpm/8 generations per second

    void onBuildControls() override {
        controls_.addUint8("seed", seed, 0, 255);
        controls_.addBool("wraparound", wraparound);
        controls_.addUint8("hue", hue, 0, 255);
        controls_.addUint8("bpm", bpm, 1, 255);
    }

    void onBuildState() override {
        nrOfLightsType count = static_cast<nrOfLightsType>(width()) * height();
        if (enabled() && count > 0) {
            if (count != cellCount_) {
                releaseGrids();
                cur_ = static_cast<uint8_t*>(platform::alloc(count));
                nxt_ = static_cast<uint8_t*>(platform::alloc(count));
                if (cur_ && nxt_) {
                    cellCount_ = count;
                    reseed();  // fresh state for the new dimensions
                } else {
                    releaseGrids();  // partial alloc → keep nothing
                }
            }
        } else {
            releaseGrids();
        }
        // Two grids: report both so the UI's per-effect heap figure is honest.
        setDynamicBytes(static_cast<size_t>(cellCount_) * 2);
    }

    void teardown() override {
        releaseGrids();
        setDynamicBytes(0);
    }

    ~GameOfLifeEffect() override {
        releaseGrids();
    }

    void loop() override {
        if (!cur_ || !nxt_) return;

        lengthType w = width();
        lengthType h = height();
        if (w <= 0 || h <= 0) return;

        // 1. Time-gate the generation rate so bpm controls speed independent of
        //    frame rate. Accumulate dt*bpm (ms·bpm) and spend whole "beats";
        //    one beat = one generation. bpm/8 ≈ generations per second (bpm 8 →
        //    1/s, 60 → ~7.5/s, 255 → ~32/s). Numerator-only accumulator, divide
        //    at the read site — same shape as CheckerboardEffect.
        uint32_t now = elapsed();
        uint32_t dt = now - lastElapsed_;
        lastElapsed_ = now;
        stepAccum_ += static_cast<uint64_t>(dt) * bpm;
        constexpr uint64_t kMsPerBeat = 8000;  // 8000 ms·bpm == one generation
        // Cap catch-up so a long stall (e.g. tab hidden) can't run thousands of
        // generations in one frame.
        uint8_t budget = 4;
        while (stepAccum_ >= kMsPerBeat && budget-- > 0) {
            stepAccum_ -= kMsPerBeat;
            advance();
        }

        // 2. Render the current grid EVERY frame. The Layer clears the buffer
        //    before each effect runs, so skipping the paint on non-step frames
        //    would leave the buffer black — the grid only advances on beats, but
        //    it must be drawn on every frame to stay visible between them.
        uint8_t* buf = buffer();
        uint8_t cpl = channelsPerLight();
        for (lengthType y = 0; y < h; y++) {
            for (lengthType x = 0; x < w; x++) {
                size_t off = static_cast<size_t>(idx(x, y, w)) * cpl;
                if (cur_[idx(x, y, w)]) {
                    RGB c = hsvToRgb(static_cast<uint8_t>(hue + x * 3 + y * 5), 200, 255);
                    if (cpl >= 1) buf[off + 0] = c.r;
                    if (cpl >= 2) buf[off + 1] = c.g;
                    if (cpl >= 3) buf[off + 2] = c.b;
                } else {
                    if (cpl >= 1) buf[off + 0] = 0;
                    if (cpl >= 2) buf[off + 1] = 0;
                    if (cpl >= 3) buf[off + 2] = 0;
                }
            }
        }
    }

    // --- Test helpers (deterministic stepping without rendering) -------------
    // Mirror the v1 effect's test surface so the rule can be pinned directly.
    void setCell(lengthType x, lengthType y, bool alive) {
        if (cur_ && x >= 0 && y >= 0 && x < width() && y < height())
            cur_[idx(x, y, width())] = alive ? 1 : 0;
    }
    bool getCell(lengthType x, lengthType y) const {
        if (!cur_ || x < 0 || y < 0 || x >= width() || y >= height()) return false;
        return cur_[idx(x, y, width())] != 0;
    }
    nrOfLightsType liveCount() const {
        nrOfLightsType n = 0;
        for (nrOfLightsType i = 0; i < cellCount_; i++) n += (cur_[i] != 0);
        return n;
    }
    void clearGrid() {
        if (cur_) std::memset(cur_, 0, cellCount_);
    }
    // Advance one B3/S23 generation (no re-seed, no render); returns the number
    // of cells that changed and, if `aliveOut` is given, writes the live count.
    // loop() calls this then re-seeds on extinction/stasis. Public so tests can
    // step a known pattern deterministically.
    nrOfLightsType stepOnce(nrOfLightsType* aliveOut = nullptr) {
        if (!cur_ || !nxt_) { if (aliveOut) *aliveOut = 0; return 0; }
        lengthType w = width();
        lengthType h = height();
        nrOfLightsType alive = 0, changed = 0;
        for (lengthType y = 0; y < h; y++) {
            for (lengthType x = 0; x < w; x++) {
                uint8_t n = neighbors(x, y, w, h);
                uint8_t self = cur_[idx(x, y, w)];
                // Birth on exactly 3 neighbours; survival on 2 or 3.
                uint8_t next = (n == 3 || (self && n == 2)) ? 1 : 0;
                nxt_[idx(x, y, w)] = next;
                if (next) alive++;
                if (next != self) changed++;
            }
        }
        uint8_t* tmp = cur_; cur_ = nxt_; nxt_ = tmp;  // swap: cur = new gen
        if (aliveOut) *aliveOut = alive;
        return changed;
    }

private:
    // One production generation plus the liveliness logic. A random soup always
    // decays to sparse still-lifes + a few blinkers (changed never hits 0, so a
    // plain stasis check won't fire) — so we re-seed when the colony goes
    // extinct, thins below a density floor, or stops growing for a while. That
    // keeps gliders and chaos coming instead of a frozen field. (MoonLight does
    // the richer version with pentomino injection + CRC cycle detection; this
    // is the minimal equivalent — see the spec's Extending note.)
    void advance() {
        nrOfLightsType alive = 0;
        stepOnce(&alive);
        generation_++;

        nrOfLightsType floor = cellCount_ / 32;  // ~3% of the grid
        if (alive <= floor) {
            reseed();
            return;
        }
        // Stagnation: if the live count barely moves over a window, the colony
        // has settled into still-lifes + oscillators. Re-seed to revive it.
        uint16_t delta = (alive > lastAlive_)
            ? static_cast<uint16_t>(alive - lastAlive_)
            : static_cast<uint16_t>(lastAlive_ - alive);
        if (delta <= (cellCount_ / 256 + 1)) {  // <0.4% change this generation
            if (++stagnantGens_ >= 32) reseed();
        } else {
            stagnantGens_ = 0;
        }
        lastAlive_ = alive;
    }

    uint8_t* cur_ = nullptr;
    uint8_t* nxt_ = nullptr;
    nrOfLightsType cellCount_ = 0;
    uint32_t rngState_ = 0;

    uint8_t rand8() {
        rngState_ = rngState_ * 1103515245u + 12345u;
        return static_cast<uint8_t>((rngState_ >> 16) & 0xFF);
    }

    // Row-major cell index (z=0 plane only).
    static nrOfLightsType idx(lengthType x, lengthType y, lengthType w) {
        return static_cast<nrOfLightsType>(y) * w + x;
    }

    // Count of the 8 Moore neighbours that are alive. Edges either wrap or are
    // treated as dead, per the wraparound control.
    uint8_t neighbors(lengthType x, lengthType y, lengthType w, lengthType h) const {
        uint8_t n = 0;
        for (int8_t dy = -1; dy <= 1; dy++) {
            for (int8_t dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0) continue;
                lengthType nx = static_cast<lengthType>(x + dx);
                lengthType ny = static_cast<lengthType>(y + dy);
                if (wraparound) {
                    if (nx < 0) nx = static_cast<lengthType>(w - 1);
                    else if (nx >= w) nx = 0;
                    if (ny < 0) ny = static_cast<lengthType>(h - 1);
                    else if (ny >= h) ny = 0;
                } else if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
                    continue;  // out-of-bounds counts as dead
                }
                n = static_cast<uint8_t>(n + (cur_[idx(nx, ny, w)] != 0));
            }
        }
        return n;
    }

    // Random initial state. The very first seeding (per grid) starts the PRNG
    // from the `seed` control so a given seed gives a reproducible opening;
    // later re-seeds continue the same stream so each revival differs — without
    // that, every reseed would replay the identical soup and the effect would
    // loop forever. ~31% alive: dense enough to evolve, sparse enough to avoid
    // instant gridlock.
    void reseed() {
        if (!cur_) return;
        if (!seeded_) {
            rngState_ = 0x9E3779B9u ^ (static_cast<uint32_t>(seed) * 2654435761u);
            seeded_ = true;
        }
        for (nrOfLightsType i = 0; i < cellCount_; i++) {
            cur_[i] = (rand8() < 80) ? 1 : 0;  // 80/256 ≈ 31%
        }
        lastAlive_ = 0;
        stagnantGens_ = 0;
    }

    void releaseGrids() {
        if (cur_) { platform::free(cur_); cur_ = nullptr; }
        if (nxt_) { platform::free(nxt_); nxt_ = nullptr; }
        cellCount_ = 0;
        seeded_ = false;  // a fresh grid re-derives the seed
    }

    // Generation-pacing + liveliness state.
    uint32_t lastElapsed_ = 0;
    uint64_t stepAccum_ = 0;       // accumulated dt*bpm (ms·bpm)
    bool seeded_ = false;          // first reseed derives from `seed`
    uint32_t generation_ = 0;
    nrOfLightsType lastAlive_ = 0;
    uint16_t stagnantGens_ = 0;
};

} // namespace mm
