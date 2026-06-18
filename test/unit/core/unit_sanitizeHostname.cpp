// @module SystemModule
// @also NetworkModule

// Pins mm::sanitizeHostname (core/Control.h), the RFC-1123 coercion that keeps the
// device name a valid DNS/mDNS hostname. deviceName is the single network identity —
// the mDNS <name>.local, the SoftAP SSID, and the DHCP hostname all derive from it —
// so an invalid name (spaces, punctuation) would break mDNS registration and the
// installer's clickable .local link. These tests lock the rule: keep [A-Za-z0-9-],
// collapse any run of other chars to a single '-', trim leading/trailing '-',
// idempotent on an already-valid name, empty stays empty (caller supplies the MAC
// fallback).

#include "doctest.h"
#include "core/Control.h"

#include <cstring>
#include <string>

namespace {
// Sanitize a copy and return it, so the cases read as input → expected.
std::string sane(const char* in) {
    char buf[64];
    std::strncpy(buf, in, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    mm::sanitizeHostname(buf);
    return std::string(buf);
}
} // namespace

TEST_CASE("sanitizeHostname leaves a valid hostname unchanged (idempotent)") {
    CHECK(sane("MM-A1B2") == "MM-A1B2");
    CHECK(sane("living-room-1") == "living-room-1");
    CHECK(sane("ESP32") == "ESP32");
}

TEST_CASE("sanitizeHostname replaces spaces with a single dash") {
    CHECK(sane("My Living Room") == "My-Living-Room");
    CHECK(sane("a  b") == "a-b");        // a run of spaces collapses to ONE dash
}

TEST_CASE("sanitizeHostname strips punctuation and other invalid chars") {
    CHECK(sane("My Room!") == "My-Room");
    CHECK(sane("a.b.c") == "a-b-c");      // dots are not valid in a single label
    CHECK(sane("café") == "caf");         // non-ASCII dropped (UTF-8 bytes → one dash run, then trimmed)
    CHECK(sane("foo_bar") == "foo-bar");  // underscore is not a legal hostname char
}

TEST_CASE("sanitizeHostname trims leading and trailing dashes / invalid runs") {
    CHECK(sane(" leading") == "leading");
    CHECK(sane("trailing ") == "trailing");
    CHECK(sane("-keep-") == "keep");
    CHECK(sane("!!mid!!") == "mid");
}

TEST_CASE("sanitizeHostname yields empty for all-invalid input (caller falls back)") {
    CHECK(sane("") == "");
    CHECK(sane("   ") == "");
    CHECK(sane("!@#$") == "");
}
