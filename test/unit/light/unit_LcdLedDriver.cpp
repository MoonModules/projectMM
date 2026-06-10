// @module LcdLedDriver
// @also Drivers, Correction

#include "doctest.h"
#include "light/drivers/Correction.h"
#include "light/drivers/LcdLedDriver.h"
#include "light/layers/Buffer.h"

#include <cstring>

// Host-side half of the LCD driver: lane slicing (the shared PinList
// semantics), the frame-byte arithmetic (latch pad, 64-byte alignment, RGBW
// growth), and the parse-error/recovery shape. The hardware half (bus init,
// DMA transmit) is inert on the host — desktop stubs return false/nullptr —
// and is proven on the S3.

namespace {

void wire(mm::LcdLedDriver& d, mm::Buffer& src, mm::Correction& corr,
          mm::nrOfLightsType lights) {
    // allocate succeeds exactly when lights > 0 (the zero-grid case wires an
    // empty buffer on purpose); a masked alloc failure would fail cases downstream.
    REQUIRE(src.allocate(lights, 3) == (lights > 0));
    corr.rebuild(255, mm::LightPreset::GRB);   // 3 out-channels
    d.onBuildControls();
    d.setSourceBuffer(&src);
    d.setCorrection(&corr);
    d.onBuildState();
}

// frameBytes = maxLaneLights × outCh × 24 + 784 latch pad, rounded up to 64.
size_t expectFrame(mm::nrOfLightsType maxLights, uint8_t outCh) {
    if (maxLights == 0) return 0;
    const size_t raw = static_cast<size_t>(maxLights) * outCh * 24 + 800 + 64;
    return (raw + 63) & ~static_cast<size_t>(63);
}

} // namespace

// Explicit counts slice the buffer consecutively; the frame is sized by the
// LONGEST lane. The bus always has all 8 lanes — unused strands take the
// 0-light remainder and idle LOW.
TEST_CASE("LcdLedDriver slices lanes and sizes the frame by the longest") {
    mm::LcdLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.ledsPerPin, "50,20,20");   // lanes 3..7 share the remainder: 0
    wire(d, src, corr, 90);

    REQUIRE(d.laneCount() == 8);
    CHECK(d.laneLightCount(0) == 50);
    CHECK(d.laneLightCount(1) == 20);
    CHECK(d.laneLightCount(2) == 20);
    CHECK(d.laneLightCount(3) == 0);
    CHECK(d.laneLightCount(7) == 0);
    CHECK(d.laneStart(0) == 0);
    CHECK(d.laneStart(1) == 50);
    CHECK(d.laneStart(2) == 70);
    CHECK(d.maxLaneLights() == 50);
    CHECK(d.frameBytes() == expectFrame(50, 3));
}

// Empty ledsPerPin splits evenly — same PinList semantics the RMT driver uses.
TEST_CASE("LcdLedDriver even split over the default 8 lanes") {
    mm::LcdLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 256);   // default pins: 8 lanes

    REQUIRE(d.laneCount() == 8);
    CHECK(d.laneLightCount(0) == 32);
    CHECK(d.laneLightCount(7) == 32);
    CHECK(d.maxLaneLights() == 32);
    CHECK(d.frameBytes() == expectFrame(32, 3));
}

// An RGB→RGBW preset toggle grows the frame (32 vs 24 slot bytes per light).
TEST_CASE("LcdLedDriver frame grows on RGBW preset") {
    mm::LcdLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.ledsPerPin, "50,50");   // lanes 2..7 idle
    wire(d, src, corr, 100);
    CHECK(d.frameBytes() == expectFrame(50, 3));

    corr.rebuild(255, mm::LightPreset::GRBW);
    d.onCorrectionChanged();
    CHECK(d.frameBytes() == expectFrame(50, 4));
}

// A bad pin list idles the driver with the parse literal in the status; fixing it recovers.
TEST_CASE("LcdLedDriver bad pins → status error → recovery") {
    mm::LcdLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "1,nope");
    wire(d, src, corr, 64);

    CHECK(d.laneCount() == 0);
    CHECK(d.frameBytes() == 0);
    CHECK(d.status() != nullptr);

    std::strcpy(d.pins, "1,2,4,5,6,7,8,9");
    d.onBuildState();
    CHECK(d.laneCount() == 8);
    CHECK(d.status() == nullptr);
}

// IDF's i80 bus rejects partial pin sets, so the driver does too — fewer than
// 8 pins is a config error, not a narrower bus.
TEST_CASE("LcdLedDriver requires exactly 8 pins") {
    mm::LcdLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "1,2,4");
    wire(d, src, corr, 64);

    CHECK(d.laneCount() == 0);
    CHECK(d.frameBytes() == 0);
    REQUIRE(d.status() != nullptr);
    CHECK(std::strcmp(d.status(), "LCD bus needs exactly 8 pins") == 0);
}

// A 0×0×0 grid is a clean idle: zero counts, zero frame (no pad for an empty frame), no crash.
TEST_CASE("LcdLedDriver tolerates a zero-light buffer") {
    mm::LcdLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 0);

    CHECK(d.laneCount() == 8);       // pins parse fine
    CHECK(d.maxLaneLights() == 0);
    CHECK(d.frameBytes() == 0);
    d.loop();                        // must be a no-op, not a crash
    CHECK(true);
}

// setup/teardown cycles leave no residue (status clean, ASAN-checked heap).
TEST_CASE("LcdLedDriver setup/teardown is repeatable") {
    mm::LcdLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    src.allocate(64, 3);
    corr.rebuild(255, mm::LightPreset::GRB);
    d.onBuildControls();
    for (int cycle = 0; cycle < 4; cycle++) {
        d.setup();
        d.setSourceBuffer(&src);
        d.setCorrection(&corr);
        d.onBuildState();
        REQUIRE(d.laneCount() == 8);
        d.teardown();
        CHECK(d.status() == nullptr);
    }
}

// loopbackRxPin is bound always, visible only while loopbackTest is on.
TEST_CASE("LcdLedDriver loopbackRxPin tracks the loopbackTest toggle") {
    mm::LcdLedDriver d;
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
