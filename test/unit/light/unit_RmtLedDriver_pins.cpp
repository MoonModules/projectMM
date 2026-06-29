// @module RmtLedDriver
// @also Drivers, Correction

#include "doctest.h"
#include "light/drivers/RmtLedDriver.h"
#include "light/drivers/Correction.h"
#include "light/layers/Buffer.h"

#include <cstring>

// These tests pin the MULTI-PIN surface: the `pins` / `ledsPerPin` text-control
// parsing (shared free functions in PinList.h, used by RmtLedDriver and LcdLedDriver
// precedent) and the slice arithmetic down to per-pin symbol offsets. All pure
// host logic — the RMT peripheral is never touched; on desktop the channel init
// is inert but parsing and slicing must behave identically, which is exactly
// what lets CI guard them.

namespace {

void wire(mm::RmtLedDriver& d, mm::Buffer& src, mm::Correction& corr,
          mm::nrOfLightsType lights) {
    REQUIRE(src.allocate(lights, 3));   // a masked alloc failure would fail cases downstream
    corr.rebuild(255, mm::LightPreset::GRB);   // 3 out-channels
    d.onBuildControls();
    d.setSourceBuffer(&src);
    d.setCorrection(&corr);
    d.onBuildState();
}

} // namespace

// --- parsePinList -----------------------------------------------------------

// "18,17,16" parses to three pins in list order — the order defines the buffer slices.
TEST_CASE("parsePinList accepts a comma-separated list, in order") {
    uint16_t pins[8] = {};
    uint8_t n = 0;
    CHECK(mm::parsePinList("18,17,16", pins, 8, n) == nullptr);
    REQUIRE(n == 3);
    CHECK(pins[0] == 18);
    CHECK(pins[1] == 17);
    CHECK(pins[2] == 16);
}

// A single pin (the default "18") and spaces around tokens are both fine.
TEST_CASE("parsePinList accepts a single pin and spaces around tokens") {
    uint16_t pins[8] = {};
    uint8_t n = 0;
    CHECK(mm::parsePinList("18", pins, 8, n) == nullptr);
    REQUIRE(n == 1);
    CHECK(pins[0] == 18);

    CHECK(mm::parsePinList(" 18, 17 ", pins, 8, n) == nullptr);
    REQUIRE(n == 2);
    CHECK(pins[1] == 17);
}

TEST_CASE("parsePinList rejects bad input with a static error message") {
    uint16_t pins[8] = {};
    uint8_t n = 0;
    // Bad token, trailing comma (empty token), and the empty string are all
    // invalid — the driver idles with the message in its status field.
    CHECK(mm::parsePinList("18,x", pins, 8, n) != nullptr);
    CHECK(mm::parsePinList("18,", pins, 8, n) != nullptr);
    CHECK(mm::parsePinList("", pins, 8, n) != nullptr);
}

// maxPins is the chip's RMT TX-channel cap: 5 pins fail an S3-sized 4, fit a classic 8.
TEST_CASE("parsePinList enforces maxPins (the chip's TX-channel cap)") {
    uint16_t pins[8] = {};
    uint8_t n = 0;
    // 5 pins through an S3-sized cap of 4 → rejected.
    CHECK(mm::parsePinList("1,2,3,4,5", pins, 4, n) != nullptr);
    // The same list fits the classic-ESP32 cap of 8.
    CHECK(mm::parsePinList("1,2,3,4,5", pins, 8, n) == nullptr);
    CHECK(n == 5);
}

// The same GPIO twice would double-drive one strand — rejected at parse time.
TEST_CASE("parsePinList rejects duplicate pins") {
    uint16_t pins[8] = {};
    uint8_t n = 0;
    CHECK(mm::parsePinList("18,17,18", pins, 8, n) != nullptr);
}

// --- assignCounts -----------------------------------------------------------

// Explicit "100,100,50" maps one count to each pin by position.
TEST_CASE("assignCounts takes explicit per-pin counts") {
    mm::nrOfLightsType counts[8] = {};
    CHECK(mm::assignCounts("100,100,50", 3, 250, counts) == nullptr);
    CHECK(counts[0] == 100);
    CHECK(counts[1] == 100);
    CHECK(counts[2] == 50);
}

// A short list assigns what it names; unlisted pins share the remaining lights evenly.
TEST_CASE("assignCounts splits the remainder evenly over unlisted pins") {
    // 3 pins, only the first has an explicit count: the remaining 150 lights
    // split evenly over the remaining 2 pins.
    mm::nrOfLightsType counts[8] = {};
    CHECK(mm::assignCounts("100", 3, 250, counts) == nullptr);
    CHECK(counts[0] == 100);
    CHECK(counts[1] == 75);
    CHECK(counts[2] == 75);
}

TEST_CASE("assignCounts with an empty list splits evenly, last pin takes the rounding remainder") {
    mm::nrOfLightsType counts[8] = {};
    CHECK(mm::assignCounts("", 3, 100, counts) == nullptr);
    CHECK(counts[0] == 33);
    CHECK(counts[1] == 33);
    CHECK(counts[2] == 34);
}

TEST_CASE("assignCounts clamps so the sum never exceeds the buffer") {
    mm::nrOfLightsType counts[8] = {};
    // Explicit counts overrun the 250-light buffer: second pin clamps to what's left.
    CHECK(mm::assignCounts("200,200", 2, 250, counts) == nullptr);
    CHECK(counts[0] == 200);
    CHECK(counts[1] == 50);
    // A single count larger than the whole buffer clamps to the buffer.
    CHECK(mm::assignCounts("300", 1, 250, counts) == nullptr);
    CHECK(counts[0] == 250);
}

TEST_CASE("assignCounts handles a zero-light buffer (0×0×0 grid) as all-zero") {
    mm::nrOfLightsType counts[8] = {0xFF, 0xFF, 0xFF};
    CHECK(mm::assignCounts("", 3, 0, counts) == nullptr);
    CHECK(counts[0] == 0);
    CHECK(counts[1] == 0);
    CHECK(counts[2] == 0);
}

TEST_CASE("assignCounts rejects a bad token") {
    mm::nrOfLightsType counts[8] = {};
    CHECK(mm::assignCounts("100,x", 2, 250, counts) != nullptr);
}

TEST_CASE("assignCounts ignores extra counts beyond the pin list") {
    // Robust to any input: a stale longer ledsPerPin after pins shrank is not an
    // error — the extra entries just don't apply.
    mm::nrOfLightsType counts[8] = {};
    CHECK(mm::assignCounts("10,20,30", 2, 100, counts) == nullptr);
    CHECK(counts[0] == 10);
    CHECK(counts[1] == 20);
}

// --- driver-level slicing ----------------------------------------------------

TEST_CASE("RmtLedDriver slices the buffer across pins (even split)") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "18,17,16");
    wire(d, src, corr, 90);

    REQUIRE(d.pinCount() == 3);
    CHECK(d.pinLightCount(0) == 30);
    CHECK(d.pinLightCount(1) == 30);
    CHECK(d.pinLightCount(2) == 30);
    // Slice i starts at sumBefore(i) × outCh × 8 words.
    CHECK(d.pinSymbolOffsetWords(0) == 0);
    CHECK(d.pinSymbolOffsetWords(1) == static_cast<size_t>(30) * 3 * 8);
    CHECK(d.pinSymbolOffsetWords(2) == static_cast<size_t>(60) * 3 * 8);
}

TEST_CASE("RmtLedDriver slices the buffer per ledsPerPin") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "18,17,16");
    std::strcpy(d.ledsPerPin, "50,20,20");
    wire(d, src, corr, 90);

    REQUIRE(d.pinCount() == 3);
    CHECK(d.pinLightCount(0) == 50);
    CHECK(d.pinLightCount(1) == 20);
    CHECK(d.pinLightCount(2) == 20);
    CHECK(d.pinSymbolOffsetWords(1) == static_cast<size_t>(50) * 3 * 8);
    CHECK(d.pinSymbolOffsetWords(2) == static_cast<size_t>(70) * 3 * 8);
}

TEST_CASE("RmtLedDriver idles with a status error on a bad pin list") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "18,nope");
    wire(d, src, corr, 64);

    CHECK(d.pinCount() == 0);            // no pins → loop() emits nothing
    CHECK(d.status() != nullptr);        // the parse error is surfaced, not silent
    // Recovery: fixing the control and rebuilding clears the error.
    std::strcpy(d.pins, "18");
    d.onBuildState();
    CHECK(d.pinCount() == 1);
    CHECK(d.status() == nullptr);
}

TEST_CASE("RmtLedDriver with the empty default pins idles cleanly (no pin assumed)") {
    // Pins now default UNSET (the "default only when it cannot do harm" rule — the
    // strand is user-soldered). A fresh, unconfigured driver must idle, not grab a
    // GPIO: zero pins, a status note, and a crash-safe no-op loop().
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    REQUIRE(d.pins[0] == '\0');           // the empty default, not a bench guess
    wire(d, src, corr, 64);               // wire() leaves pins as-is (empty)

    CHECK(d.pinCount() == 0);             // nothing claimed
    CHECK(d.status() != nullptr);         // "set pins" surfaced, not silent
    d.loop();                             // must be a no-op, not a crash
    // Setting pins later brings it live (the user-configures-then-runs flow).
    std::strcpy(d.pins, "18");
    d.onBuildState();
    CHECK(d.pinCount() == 1);
    CHECK(d.status() == nullptr);
}

TEST_CASE("RmtLedDriver re-slices when the source buffer changes") {
    // setSourceBuffer must recompute counts (the Drivers container re-passes the
    // buffer on every buildState) — a grid resize updates the even split.
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "18,17");
    wire(d, src, corr, 100);
    CHECK(d.pinLightCount(0) == 50);

    src.allocate(200, 3);
    d.setSourceBuffer(&src);
    d.onBuildState();
    CHECK(d.pinLightCount(0) == 100);
    CHECK(d.pinLightCount(1) == 100);
}

// --- source-buffer window (start / count) ------------------------------------
//
// Every driver reads the SAME shared buffer and outputs a contiguous slice
// [start, start+count) of it (count 0 = to the end). Two drivers on disjoint
// windows is how the onboard status LED (window [0,1)) coexists with the main
// strip (window [1, N)) without either stealing the other's lights. The slice
// arithmetic lives on DriverBase (windowSlice/setWindow), pinned here through
// RmtLedDriver because it is the host-runnable concrete driver.

TEST_CASE("RmtLedDriver window: ledsPerPin distributes over the window, not the whole buffer") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "18,17");
    d.setWindow(/*start=*/10, /*count=*/40);   // this driver owns lights [10,50)
    wire(d, src, corr, 100);                    // 100-light shared buffer

    REQUIRE(d.pinCount() == 2);
    // The even split is over the 40-light WINDOW, not the 100-light buffer.
    CHECK(d.pinLightCount(0) == 20);
    CHECK(d.pinLightCount(1) == 20);
    CHECK(d.windowStart() == 10);
}

TEST_CASE("RmtLedDriver window: count 0 means the rest of the buffer from start") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "18");
    d.setWindow(/*start=*/1, /*count=*/0);   // from light 1 to the end
    wire(d, src, corr, 65);                   // onboard LED took light 0; 64 remain

    REQUIRE(d.pinCount() == 1);
    CHECK(d.pinLightCount(0) == 64);          // 65 - 1 = 64
}

TEST_CASE("RmtLedDriver window: a size-1 window at 0 is the onboard-LED case") {
    // The pairing that drove this feature: one driver renders ONLY light 0 (an
    // onboard status LED), a second renders the strip from light 1 on. Here we
    // pin the first half — exactly one light, regardless of buffer size.
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "48");
    d.setWindow(/*start=*/0, /*count=*/1);
    wire(d, src, corr, 64);

    REQUIRE(d.pinCount() == 1);
    CHECK(d.pinLightCount(0) == 1);           // just the onboard LED
}

TEST_CASE("RmtLedDriver window: a start past the buffer end yields an empty slice") {
    // Robust to any input: a window beyond the current buffer (e.g. the grid
    // shrank) must clamp to zero lights, not read out of bounds.
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "18");
    d.setWindow(/*start=*/200, /*count=*/10);
    wire(d, src, corr, 64);

    CHECK(d.pinLightCount(0) == 0);           // nothing to drive; loop() is a no-op
    d.loop();                                 // must not crash / overrun
}

// --- loop() robustness -------------------------------------------------------
//
// loop()'s transmit-all/wait-all concurrency body is gated out on the desktop
// (platform::rmtTxChannels == 0 → it returns at the top), exactly as
// LcdLedDriver::loop() is. So the host can pin only the reachable contract:
// loop() must never crash or overrun for any pin configuration, grid size, or
// uninitialised state. The concurrency path itself (parallel transmit, longest-
// strand cost) is proven on hardware by the real-frame loopback self-test —
// the platform boundary keeps it out of CI, which is by design.

// loop() is a safe no-op across single-pin, multi-pin and zero-grid configs.
TEST_CASE("RmtLedDriver loop is crash-safe for every pin configuration") {
    mm::Correction corr;
    corr.rebuild(255, mm::LightPreset::GRB);

    SUBCASE("single pin, populated grid") {
        mm::RmtLedDriver d; mm::Buffer src;
        std::strcpy(d.pins, "18");
        wire(d, src, corr, 64);
        d.loop();                       // host: inert; must not crash/overrun
    }
    SUBCASE("multi-pin even split") {
        mm::RmtLedDriver d; mm::Buffer src;
        std::strcpy(d.pins, "18,17,16");
        wire(d, src, corr, 90);
        REQUIRE(d.pinCount() == 3);
        d.loop();
    }
    SUBCASE("zero-light grid — counts and offsets stay zero") {
        // A 0-light buffer allocates nothing (allocate() returns false by
        // design), so wire it by hand rather than through the success-asserting
        // helper — the point is that loop() tolerates the empty buffer.
        mm::RmtLedDriver d; mm::Buffer src;
        std::strcpy(d.pins, "18,17");
        CHECK_FALSE(src.allocate(0, 3));
        d.onBuildControls();
        d.setSourceBuffer(&src);
        d.setCorrection(&corr);
        d.onBuildState();
        d.loop();                       // 0×0×0 must be a clean no-op
    }
    SUBCASE("loop before any buffer is wired") {
        mm::RmtLedDriver d;
        d.onBuildControls();
        d.loop();                       // uninitialised: the guards must hold
    }
    CHECK(true);                        // reached here ⇒ no crash in any subcase
}
