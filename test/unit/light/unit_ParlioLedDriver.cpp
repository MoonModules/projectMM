// @module ParlioLedDriver
// @also Drivers, Correction

#include "doctest.h"
#include "light/drivers/Correction.h"
#include "light/drivers/ParlioLedDriver.h"
#include "light/layers/Buffer.h"

#include <cstring>

// Host-side half of the Parlio driver: lane slicing (the shared PinList
// semantics), the frame-byte arithmetic (latch pad, 64-byte alignment, RGBW
// growth), and the parse-error/recovery shape. The hardware half (TX unit init,
// DMA transmit) is inert on the host — desktop stubs return false/nullptr — and
// is proven on the P4. The encoder itself is shared with the LCD driver and is
// covered by unit_LcdLedEncoder.cpp, so it isn't re-tested here.
//
// The one behavioural difference from the LCD driver pinned below: Parlio has
// NO exactly-8-pins rule — 1..8 lanes are all valid (it takes the data GPIOs
// directly, no all-lanes-required i80 bus).

namespace {

void wire(mm::ParlioLedDriver& d, mm::Buffer& src, mm::Correction& corr,
          mm::nrOfLightsType lights) {
    // Pins default to UNSET now (the "default only when it cannot do harm" rule —
    // the user solders the strand to its own GPIOs), so a fresh driver idles until
    // configured. These slicing/frame tests exercise the lane logic, not the
    // default value, so the helper supplies the bench 8-pin set unless a case set
    // its own pins first.
    if (d.pins[0] == '\0') std::strcpy(d.pins, "20,21,22,23,24,25,26,27");
    // allocate succeeds exactly when lights > 0 (zero-grid wires an empty buffer
    // on purpose); a masked alloc failure would fail cases downstream.
    REQUIRE(src.allocate(lights, 3) == (lights > 0));
    corr.rebuild(255, mm::LightPreset::GRB);   // 3 out-channels
    d.onBuildControls();
    d.setSourceBuffer(&src);
    d.setCorrection(&corr);
    d.onBuildState();
}

// frameBytes = maxLaneLights × outCh × 24 + 864 latch pad, rounded up to 64.
size_t expectFrame(mm::nrOfLightsType maxLights, uint8_t outCh) {
    if (maxLights == 0) return 0;
    const size_t raw = static_cast<size_t>(maxLights) * outCh * 24 + 800 + 64;
    return (raw + 63) & ~static_cast<size_t>(63);
}

} // namespace

// Three lanes (Parlio accepts any 1..8 count) slice the buffer consecutively;
// the frame is sized by the LONGEST lane.
TEST_CASE("ParlioLedDriver slices lanes and sizes the frame by the longest") {
    mm::ParlioLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "36,37,38");
    std::strcpy(d.ledsPerPin, "50,20,20");
    wire(d, src, corr, 90);

    REQUIRE(d.laneCount() == 3);
    CHECK(d.laneLightCount(0) == 50);
    CHECK(d.laneLightCount(1) == 20);
    CHECK(d.laneLightCount(2) == 20);
    CHECK(d.laneStart(0) == 0);
    CHECK(d.laneStart(1) == 50);
    CHECK(d.laneStart(2) == 70);
    CHECK(d.maxLaneLights() == 50);
    CHECK(d.frameBytes() == expectFrame(50, 3));
}

// Empty ledsPerPin (the default) splits evenly over the 8 lanes — shared PinList
// semantics, same as the RMT/LCD drivers.
TEST_CASE("ParlioLedDriver even split over 8 lanes") {
    mm::ParlioLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 256);         // ledsPerPin empty (default) = even split

    REQUIRE(d.laneCount() == 8);
    CHECK(d.laneLightCount(0) == 32);
    CHECK(d.laneLightCount(7) == 32);
    CHECK(d.maxLaneLights() == 32);
    CHECK(d.frameBytes() == expectFrame(32, 3));
}

// The Parlio-vs-LCD difference: 1..8 pins are ALL valid (no exactly-8 rule).
TEST_CASE("ParlioLedDriver accepts any lane count from 1 to 8") {
    mm::Correction corr;
    corr.rebuild(255, mm::LightPreset::GRB);
    for (const char* pinList : {"36", "36,37", "36,37,38,39,40", "36,37,38,39,40,41,42,43"}) {
        mm::ParlioLedDriver d;
        mm::Buffer src;
        std::strcpy(d.pins, pinList);
        wire(d, src, corr, 64);
        // count the commas+1 to know the expected lane count
        uint8_t expected = 1;
        for (const char* p = pinList; *p; p++) if (*p == ',') expected++;
        CHECK(d.laneCount() == expected);
        CHECK(d.status() == nullptr);   // no error for any 1..8 count
    }
}

// More than 8 pins is rejected (the chip's lane cap), like the other drivers.
TEST_CASE("ParlioLedDriver rejects more than 8 pins") {
    mm::ParlioLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "1,2,3,4,5,6,7,8,9");
    wire(d, src, corr, 64);
    CHECK(d.laneCount() == 0);
    CHECK(d.status() != nullptr);
}

// An RGB→RGBW preset toggle grows the frame (32 vs 24 slot bytes per light).
TEST_CASE("ParlioLedDriver frame grows on RGBW preset") {
    mm::ParlioLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.ledsPerPin, "50,50");
    wire(d, src, corr, 100);
    CHECK(d.frameBytes() == expectFrame(50, 3));

    corr.rebuild(255, mm::LightPreset::GRBW);
    d.onCorrectionChanged();
    CHECK(d.frameBytes() == expectFrame(50, 4));
}

// A bad pin list idles the driver with the parse literal in the status; fixing it recovers.
TEST_CASE("ParlioLedDriver bad pins → status error → recovery") {
    mm::ParlioLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "36,nope");
    wire(d, src, corr, 64);

    CHECK(d.laneCount() == 0);
    CHECK(d.frameBytes() == 0);
    CHECK(d.status() != nullptr);

    std::strcpy(d.pins, "36,37");
    d.onBuildState();
    CHECK(d.laneCount() == 2);
    CHECK(d.status() == nullptr);
}

// Pins now default UNSET (the "default only when it cannot do harm" rule — the
// strand is user-soldered). A fresh, unconfigured driver idles, never grabbing a
// GPIO. (wire() back-fills empty pins for the slicing cases, so this one wires
// the buffer directly to keep pins empty.)
TEST_CASE("ParlioLedDriver with the empty default pins idles cleanly") {
    mm::ParlioLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    REQUIRE(d.pins[0] == '\0');           // the empty default, not a bench guess
    REQUIRE(src.allocate(64, 3));
    corr.rebuild(255, mm::LightPreset::GRB);
    d.onBuildControls();
    d.setSourceBuffer(&src);
    d.setCorrection(&corr);
    d.onBuildState();

    CHECK(d.laneCount() == 0);            // no lanes claimed
    CHECK(d.frameBytes() == 0);
    CHECK(d.status() != nullptr);         // "set pins" surfaced, not silent
    d.loop();                             // must be a no-op, not a crash
}

// A 0×0×0 grid is a clean idle: zero counts, zero frame, no crash.
TEST_CASE("ParlioLedDriver tolerates a zero-light buffer") {
    mm::ParlioLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 0);

    CHECK(d.laneCount() == 8);       // the default 8 pins parse fine
    CHECK(d.maxLaneLights() == 0);
    CHECK(d.frameBytes() == 0);
    d.loop();                        // must be a no-op, not a crash
    CHECK(true);
}

// loop() is crash-safe across single-pin / multi-pin / pre-init configs (the
// transmit path is gated out on the host; this pins the reachable contract).
TEST_CASE("ParlioLedDriver loop is crash-safe for every pin configuration") {
    mm::Correction corr;
    corr.rebuild(255, mm::LightPreset::GRB);

    SUBCASE("single pin, populated grid") {
        mm::ParlioLedDriver d; mm::Buffer src;
        std::strcpy(d.pins, "36");
        wire(d, src, corr, 64);
        d.loop();
    }
    SUBCASE("multi-pin even split") {
        mm::ParlioLedDriver d; mm::Buffer src;
        std::strcpy(d.pins, "36,37,38");
        wire(d, src, corr, 90);
        REQUIRE(d.laneCount() == 3);
        d.loop();
    }
    SUBCASE("loop before any buffer is wired") {
        mm::ParlioLedDriver d;
        d.onBuildControls();
        d.loop();
    }
    CHECK(true);
}

// setup/teardown cycles leave no residue (status clean, ASAN-checked heap).
TEST_CASE("ParlioLedDriver setup/teardown is repeatable") {
    mm::ParlioLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    src.allocate(64, 3);
    corr.rebuild(255, mm::LightPreset::GRB);
    std::strcpy(d.pins, "20,21,22,23,24,25,26,27");   // pins now default UNSET
    d.onBuildControls();
    for (int cycle = 0; cycle < 4; cycle++) {
        d.setup();
        d.setSourceBuffer(&src);
        d.setCorrection(&corr);
        d.onBuildState();
        REQUIRE(d.laneCount() == 8);   // the 8 pins set above
        d.teardown();
        CHECK(d.status() == nullptr);
    }
}

// loopbackRxPin is bound always, visible only while loopbackTest is on.
TEST_CASE("ParlioLedDriver loopbackRxPin tracks the loopbackTest toggle") {
    mm::ParlioLedDriver d;
    d.onBuildControls();
    bool found = false;
    for (uint8_t i = 0; i < d.controls().count(); i++) {
        if (std::strcmp(d.controls()[i].name, "loopbackRxPin") == 0) {
            found = true;
            CHECK(d.controls()[i].hidden == true);   // test mode off by default
        }
    }
    CHECK(found);
}

// loopbackTxPin (optional lane-0 TX override) is bound always, hidden until the
// test is on — same conditional-control contract as loopbackRxPin. The override's
// lane-0 substitution is hardware-only (parlioLanes==0 on desktop); the visibility
// contract is host-testable here.
TEST_CASE("ParlioLedDriver loopbackTxPin tracks the loopbackTest toggle") {
    mm::ParlioLedDriver d;
    d.onBuildControls();
    bool found = false;
    for (uint8_t i = 0; i < d.controls().count(); i++) {
        if (std::strcmp(d.controls()[i].name, "loopbackTxPin") == 0) {
            found = true;
            CHECK(d.controls()[i].hidden == true);   // test mode off by default
        }
    }
    CHECK(found);
}
