// @module HueDriver

// Pins HueDriver's host-testable core: the changed-only diff, the RGB→HSV colour body it PUTs,
// and the parse that keeps only colour-capable, reachable lights. Live bridge I/O (httpRequest,
// pairing) needs a real bridge — that's the bench; here the seams run with no socket.

#include "doctest.h"
#include "light/drivers/HueDriver.h"

#include <cstdlib>
#include <cstring>

TEST_CASE("HueDriver: a coloured pixel becomes an on/bri/hue/sat state body") {
    mm::HueDriver hue;
    char body[80] = {};
    CHECK(hue.wouldPushForTest(0, 255, 0, 0, body, sizeof(body)));   // pure red
    CHECK(std::strstr(body, "\"on\":true") != nullptr);
    CHECK(std::strstr(body, "\"bri\":254") != nullptr);             // max value
    CHECK(std::strstr(body, "\"hue\":") != nullptr);
    CHECK(std::strstr(body, "\"sat\":254") != nullptr);            // fully saturated
}

TEST_CASE("HueDriver: a black pixel becomes on:false") {
    mm::HueDriver hue;
    char body[80] = {};
    CHECK(hue.wouldPushForTest(1, 0, 0, 0, body, sizeof(body)));
    CHECK(std::strstr(body, "\"on\":false") != nullptr);
    CHECK(std::strstr(body, "\"hue\"") == nullptr);   // off → no colour fields
}

TEST_CASE("HueDriver: RGB→HSV maps the primaries to the right Hue wheel positions") {
    uint16_t h; uint8_t s, v;
    mm::HueDriver::rgbToHsvForTest(255, 0, 0, h, s, v);   // red → hue ~0
    CHECK(v == 254); CHECK(s == 254);
    CHECK((h < 2000 || h > 63000));                       // near the 0/65535 wrap
    mm::HueDriver::rgbToHsvForTest(0, 255, 0, h, s, v);   // green → ~1/3 of the wheel
    CHECK(h > 65535 / 3 - 3000); CHECK(h < 65535 / 3 + 3000);
    mm::HueDriver::rgbToHsvForTest(0, 0, 255, h, s, v);   // blue → ~2/3
    CHECK(h > 65535 * 2 / 3 - 3000); CHECK(h < 65535 * 2 / 3 + 3000);
    mm::HueDriver::rgbToHsvForTest(128, 128, 128, h, s, v); // grey → sat 0
    CHECK(s == 0);
}

TEST_CASE("HueDriver: unchanged colour is not resent, a changed one is") {
    mm::HueDriver hue;
    char body[80] = {};
    CHECK(hue.wouldPushForTest(2, 10, 20, 30, body, sizeof(body)));        // first → yes
    CHECK_FALSE(hue.wouldPushForTest(2, 10, 20, 30, body, sizeof(body)));  // same → skip
    CHECK(hue.wouldPushForTest(2, 10, 20, 31, body, sizeof(body)));        // changed → yes
}

TEST_CASE("HueDriver: parseLights keeps only colour-capable, reachable lights") {
    mm::HueDriver hue;
    // id 5 colour + reachable (keep); id 7 dimmable-only white (drop); id 10 on/off plug (drop);
    // id 8 colour but UNREACHABLE (drop). The shapes the real bridge returns.
    const char* json =
        "{\"5\":{\"state\":{\"on\":false,\"bri\":77,\"hue\":8595,\"sat\":121,\"reachable\":true},\"name\":\"Bureau lamp\"},"
        "\"7\":{\"state\":{\"on\":true,\"bri\":40,\"reachable\":true},\"name\":\"Gang lamp\"},"
        "\"10\":{\"state\":{\"on\":true,\"reachable\":true},\"name\":\"Bureau\"},"
        "\"8\":{\"state\":{\"on\":false,\"bri\":1,\"hue\":0,\"sat\":0,\"reachable\":false},\"name\":\"Nachtkastje\"}}";
    hue.parseLightsForTest(json);
    REQUIRE(hue.lightCountForTest() == 1);     // only id 5 qualifies
    CHECK(hue.hueIdForTest(0) == 5);
    CHECK(hue.colourCountForTest() == 1);
}
