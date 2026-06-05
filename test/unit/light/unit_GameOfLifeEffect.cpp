// @module GameOfLifeEffect

#include "doctest.h"
#include "light/effects/GameOfLifeEffect.h"
#include "light/layouts/GridLayout.h"
#include "platform/platform.h"

// Build a Layer with a w×h grid and a GameOfLifeEffect child, run onBuildState
// so the cell grids allocate. Returns by configuring the passed-in objects.
static void build(mm::Layouts& layouts, mm::GridLayout& grid, mm::Layer& layer,
                  mm::GameOfLifeEffect& gol, mm::lengthType w, mm::lengthType h) {
    grid.width = w;
    grid.height = h;
    grid.depth = 1;
    layouts.addChild(&grid);
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.addChild(&gol);
    layer.onBuildState();
}

// Two cell grids of width × height bytes each.
TEST_CASE("GameOfLifeEffect allocates two cell grids when enabled") {
    mm::Layouts layouts; mm::GridLayout grid; mm::Layer layer; mm::GameOfLifeEffect gol;
    build(layouts, grid, layer, gol, 16, 16);
    CHECK(gol.dynamicBytes() == 16 * 16 * 2);
}

// Disabling releases both grids (dynamicBytes drops to 0) via the parent lifecycle.
TEST_CASE("GameOfLifeEffect frees grids when disabled") {
    mm::Layouts layouts; mm::GridLayout grid; mm::Layer layer; mm::GameOfLifeEffect gol;
    build(layouts, grid, layer, gol, 8, 8);
    CHECK(gol.dynamicBytes() > 0);

    gol.setEnabled(false);
    layer.onBuildState();
    CHECK(gol.dynamicBytes() == 0);
}

// A blinker (horizontal 3-in-a-row) oscillates with period 2 under B3/S23:
// it becomes a vertical 3-in-a-row, then back. Pins both birth (B3) and
// survival (S23) on a known pattern.
TEST_CASE("GameOfLifeEffect blinker oscillates period-2 (B3/S23)") {
    mm::Layouts layouts; mm::GridLayout grid; mm::Layer layer; mm::GameOfLifeEffect gol;
    build(layouts, grid, layer, gol, 5, 5);

    // Horizontal blinker centred at (2,2): (1,2)(2,2)(3,2)
    gol.clearGrid();
    gol.setCell(1, 2, true);
    gol.setCell(2, 2, true);
    gol.setCell(3, 2, true);
    CHECK(gol.liveCount() == 3);

    gol.stepOnce();  // → vertical: (2,1)(2,2)(2,3)
    CHECK(gol.liveCount() == 3);
    CHECK(gol.getCell(2, 1));
    CHECK(gol.getCell(2, 2));
    CHECK(gol.getCell(2, 3));
    CHECK_FALSE(gol.getCell(1, 2));
    CHECK_FALSE(gol.getCell(3, 2));

    gol.stepOnce();  // → back to horizontal
    CHECK(gol.getCell(1, 2));
    CHECK(gol.getCell(2, 2));
    CHECK(gol.getCell(3, 2));
}

// A 2×2 block is a still-life: every live cell has 3 neighbours (S3), no dead
// cell has exactly 3 (no B3), so stepOnce leaves it unchanged.
TEST_CASE("GameOfLifeEffect block is a still-life") {
    mm::Layouts layouts; mm::GridLayout grid; mm::Layer layer; mm::GameOfLifeEffect gol;
    build(layouts, grid, layer, gol, 6, 6);

    gol.clearGrid();
    gol.setCell(2, 2, true);
    gol.setCell(3, 2, true);
    gol.setCell(2, 3, true);
    gol.setCell(3, 3, true);

    mm::nrOfLightsType alive = 0;
    mm::nrOfLightsType changed = gol.stepOnce(&alive);
    CHECK(changed == 0);
    CHECK(alive == 4);
    CHECK(gol.getCell(2, 2));
    CHECK(gol.getCell(3, 3));
}

// A lone cell dies (underpopulation: 0 neighbours, not S2/S3) → extinction.
TEST_CASE("GameOfLifeEffect lone cell dies") {
    mm::Layouts layouts; mm::GridLayout grid; mm::Layer layer; mm::GameOfLifeEffect gol;
    build(layouts, grid, layer, gol, 6, 6);

    gol.clearGrid();
    gol.setCell(3, 3, true);
    mm::nrOfLightsType alive = 99;
    gol.stepOnce(&alive);
    CHECK(alive == 0);
}

// Wraparound: a blinker on the right edge stays a valid 3-cell pattern because
// neighbours wrap, rather than losing cells to a hard edge.
TEST_CASE("GameOfLifeEffect wraparound wraps edges") {
    mm::Layouts layouts; mm::GridLayout grid; mm::Layer layer; mm::GameOfLifeEffect gol;
    build(layouts, grid, layer, gol, 5, 5);
    gol.wraparound = true;

    // Horizontal blinker straddling the right edge: (3,2)(4,2)(0,2 via wrap)
    gol.clearGrid();
    gol.setCell(4, 2, true);
    gol.setCell(0, 2, true);
    gol.setCell(3, 2, true);
    gol.stepOnce();
    CHECK(gol.liveCount() == 3);  // survives as a wrapped vertical blinker
}

// Reallocation on dimension change: grids resize, byte count tracks new w×h.
TEST_CASE("GameOfLifeEffect reallocates on dimension change") {
    mm::Layouts layouts; mm::GridLayout grid; mm::Layer layer; mm::GameOfLifeEffect gol;
    build(layouts, grid, layer, gol, 8, 8);
    CHECK(gol.dynamicBytes() == 8 * 8 * 2);

    grid.width = 16;
    grid.height = 8;
    layer.onBuildState();
    CHECK(gol.dynamicBytes() == 16 * 8 * 2);
}

// Must not crash on a zero-size grid (no allocation, loop is a no-op).
TEST_CASE("GameOfLifeEffect survives 0x0 grid") {
    mm::Layouts layouts; mm::GridLayout grid; mm::Layer layer; mm::GameOfLifeEffect gol;
    build(layouts, grid, layer, gol, 0, 0);
    CHECK(gol.dynamicBytes() == 0);
    layer.loop();  // no crash
    CHECK(gol.liveCount() == 0);
}

// bpm time-gates the generation rate: a low bpm advances fewer generations per
// unit time than a high bpm over the same elapsed window. Drives time via the
// desktop millis() test seam (Layer reads platform::millis in loop()).
TEST_CASE("GameOfLifeEffect bpm controls generation rate") {
    auto changesIn1s = [](uint8_t bpm) {
        mm::Layouts layouts; mm::GridLayout grid; mm::Layer layer; mm::GameOfLifeEffect gol;
        build(layouts, grid, layer, gol, 32, 32);
        gol.bpm = bpm;
        uint32_t t = 1000;
        mm::platform::setTestNowMs(t);
        layer.loop();  // first frame: initial render, no step yet
        int changes = 0;
        for (int f = 0; f < 25; f++) {       // 25 × 40ms = 1 simulated second
            t += 40;
            mm::platform::setTestNowMs(t);
            mm::nrOfLightsType before = gol.liveCount();
            layer.loop();
            if (gol.liveCount() != before) changes++;
        }
        mm::platform::setTestNowMs(0);       // restore real clock
        return changes;
    };
    // Fast bpm steps every frame; slow bpm steps only a few times in the second.
    CHECK(changesIn1s(255) > changesIn1s(8));
}

// Regression: the Layer clears the buffer before every effect frame, so the
// grid must be re-painted on EVERY frame, not just on the (rarer) beats where a
// generation advances. A bpm gate that skipped the paint left non-step frames
// black — visible as "a flash now and then" at low bpm. Drive several frames at
// a slow bpm (most are non-step) and require the buffer stays lit on all of them.
TEST_CASE("GameOfLifeEffect renders every frame between generations") {
    mm::Layouts layouts; mm::GridLayout grid; mm::Layer layer; mm::GameOfLifeEffect gol;
    build(layouts, grid, layer, gol, 32, 32);
    gol.bpm = 8;  // ~1 generation/sec: most 40ms frames do NOT step
    uint32_t t = 1000;

    for (int f = 0; f < 10; f++) {
        t += 40;
        mm::platform::setTestNowMs(t);
        layer.loop();
        auto& buf = layer.buffer();
        bool lit = false;
        for (size_t i = 0; i < buf.bytes(); i++) {
            if (buf.data()[i]) { lit = true; break; }
        }
        CHECK(lit);  // never black between beats
    }
    mm::platform::setTestNowMs(0);  // restore real clock
}
