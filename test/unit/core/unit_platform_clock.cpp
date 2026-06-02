// @module platform

// Pins the platform test-clock seam used by every time-dependent unit test.
// Production code never calls setTestNowMs; this contract is the only thing
// keeping the animation tests (LavaLamp, Metaballs, Checkerboard, Spiral) fast
// and deterministic instead of wall-clock-bound and flaky on a loaded CI box.

#include "doctest.h"
#include "platform/platform.h"

// setTestNowMs freezes platform::millis() to the given value; passing 0 restores
// the real clock so subsequent test cases see fresh time.
TEST_CASE("platform::setTestNowMs freezes and restores millis()") {
    mm::platform::setTestNowMs(12345);
    CHECK(mm::platform::millis() == 12345);
    mm::platform::setTestNowMs(67890);
    CHECK(mm::platform::millis() == 67890);

    mm::platform::setTestNowMs(0);
    // Real clock returns a non-zero value (process has been alive for some time).
    // Two consecutive reads must not equal the override values either.
    uint32_t a = mm::platform::millis();
    uint32_t b = mm::platform::millis();
    CHECK(a != 12345);
    CHECK(b != 67890);
    CHECK(a > 0);
}
