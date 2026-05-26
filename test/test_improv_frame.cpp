// Unit tests for src/core/ImprovFrame.h — the byte-at-a-time framing layer
// for Improv-WiFi serial. The parser drives the ESP32 UART task at
// src/platform/esp32/platform_esp32.cpp::improvTask; isolating it here lets
// us cover the framing without an MCU + serial cable in the loop.
//
// The Improv RPC payload semantics (the GET_DEVICE_INFO / WIFI_SETTINGS /
// etc. command shapes) live in the upstream improv-wifi/sdk-cpp library and
// are not re-tested here — the boundary between framing and RPC is the
// whole reason for the split.

#include "doctest.h"
#include "core/ImprovFrame.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace mm;

// Drive a full byte sequence through the parser. Returns the result of the
// final feed() — earlier bytes are asserted to be NeedMore so a regression
// surfaces immediately.
static ImprovFeedResult feedAll(ImprovFrameParser& p, const uint8_t* data, size_t len) {
    REQUIRE(len > 0);
    for (size_t i = 0; i + 1 < len; i++) {
        CHECK(p.feed(data[i]) == ImprovFeedResult::NeedMore);
    }
    return p.feed(data[len - 1]);
}

TEST_CASE("improvChecksum is sum mod 256") {
    const uint8_t a[] = {1, 2, 3, 4};
    CHECK(improvChecksum(a, 4) == 10);

    const uint8_t b[] = {0xFF, 0x01};
    CHECK(improvChecksum(b, 2) == 0x00);

    CHECK(improvChecksum(nullptr, 0) == 0);
}

TEST_CASE("buildImprovFrame writes magic + version + type + length + payload + checksum") {
    const uint8_t payload[] = {0xA, 0xB, 0xC};
    uint8_t out[32];
    size_t n = buildImprovFrame(ImprovFrameType::Rpc, payload, 3, out, sizeof(out));
    REQUIRE(n == 6 + 1 + 1 + 1 + 3 + 1);

    CHECK(std::memcmp(out, "IMPROV", 6) == 0);
    CHECK(out[6] == kImprovSerialVersion);
    CHECK(out[7] == static_cast<uint8_t>(ImprovFrameType::Rpc));
    CHECK(out[8] == 3);
    CHECK(out[9] == 0xA);
    CHECK(out[10] == 0xB);
    CHECK(out[11] == 0xC);
    // Checksum = sum(0..n-2) mod 256.
    CHECK(out[12] == improvChecksum(out, n - 1));
}

TEST_CASE("buildImprovFrame refuses oversize payload") {
    uint8_t payload[200] = {};   // > kImprovMaxPayload (128)
    uint8_t out[300];
    CHECK(buildImprovFrame(ImprovFrameType::Rpc, payload, sizeof(payload),
                           out, sizeof(out)) == 0);
}

TEST_CASE("buildImprovFrame refuses too-small output buffer") {
    const uint8_t payload[] = {1, 2, 3};
    uint8_t out[5];   // need 12, have 5
    CHECK(buildImprovFrame(ImprovFrameType::Rpc, payload, 3, out, sizeof(out)) == 0);
}

TEST_CASE("buildImprovFrame handles zero-length payload") {
    uint8_t out[16];
    size_t n = buildImprovFrame(ImprovFrameType::CurrentState, nullptr, 0, out, sizeof(out));
    REQUIRE(n == 6 + 1 + 1 + 1 + 1);   // magic + version + type + length + checksum
    CHECK(out[8] == 0);
    CHECK(out[9] == improvChecksum(out, n - 1));
}

TEST_CASE("ImprovFrameParser accepts a well-formed frame") {
    const uint8_t payload[] = {0x42};
    uint8_t frame[16];
    size_t n = buildImprovFrame(ImprovFrameType::Rpc, payload, 1, frame, sizeof(frame));
    REQUIRE(n > 0);

    ImprovFrameParser p;
    CHECK(feedAll(p, frame, n) == ImprovFeedResult::FrameReady);
    CHECK(p.lastType() == static_cast<uint8_t>(ImprovFrameType::Rpc));
    CHECK(p.lastPayloadLen() == 1);
    CHECK(p.lastPayload()[0] == 0x42);
}

TEST_CASE("ImprovFrameParser handles a zero-length payload frame") {
    uint8_t frame[16];
    size_t n = buildImprovFrame(ImprovFrameType::CurrentState, nullptr, 0, frame, sizeof(frame));
    REQUIRE(n > 0);

    ImprovFrameParser p;
    CHECK(feedAll(p, frame, n) == ImprovFeedResult::FrameReady);
    CHECK(p.lastType() == static_cast<uint8_t>(ImprovFrameType::CurrentState));
    CHECK(p.lastPayloadLen() == 0);
}

TEST_CASE("ImprovFrameParser detects bad checksum") {
    const uint8_t payload[] = {1, 2, 3};
    uint8_t frame[16];
    size_t n = buildImprovFrame(ImprovFrameType::Rpc, payload, 3, frame, sizeof(frame));
    REQUIRE(n > 0);
    frame[n - 1] ^= 0xFF;   // corrupt the checksum

    ImprovFrameParser p;
    CHECK(feedAll(p, frame, n) == ImprovFeedResult::BadChecksum);
}

TEST_CASE("ImprovFrameParser rejects oversize length byte") {
    // Hand-crafted: valid magic + version + type + length=200 (> kImprovMaxPayload=128).
    uint8_t frame[] = {'I','M','P','R','O','V', kImprovSerialVersion,
                       static_cast<uint8_t>(ImprovFrameType::Rpc), 200};
    ImprovFrameParser p;
    // The length byte itself triggers OversizePayload — earlier bytes are NeedMore.
    for (size_t i = 0; i < sizeof(frame) - 1; i++) {
        CHECK(p.feed(frame[i]) == ImprovFeedResult::NeedMore);
    }
    CHECK(p.feed(frame[sizeof(frame) - 1]) == ImprovFeedResult::OversizePayload);
}

TEST_CASE("ImprovFrameParser resyncs after garbage bytes") {
    // Leading garbage that does NOT match 'I' is skipped; the parser stays in Magic0.
    const uint8_t garbage[] = {0x00, 0xAA, 0xFF, 'X', 'Y', 'Z'};
    ImprovFrameParser p;
    for (uint8_t b : garbage) CHECK(p.feed(b) == ImprovFeedResult::NeedMore);

    // Then a valid frame.
    const uint8_t payload[] = {0x55};
    uint8_t frame[16];
    size_t n = buildImprovFrame(ImprovFrameType::Rpc, payload, 1, frame, sizeof(frame));
    REQUIRE(n > 0);
    CHECK(feedAll(p, frame, n) == ImprovFeedResult::FrameReady);
    CHECK(p.lastPayloadLen() == 1);
    CHECK(p.lastPayload()[0] == 0x55);
}

TEST_CASE("ImprovFrameParser resyncs on aborted magic — stray 'I' restarts the search") {
    // "I" then non-'M' — should reset to Magic0 *and* re-test if that byte is 'I'.
    // Sequence: I, I, M, P, R, O, V, version, type, len=0, checksum.
    ImprovFrameParser p;
    CHECK(p.feed('I') == ImprovFeedResult::NeedMore);   // Magic0 → Magic1
    CHECK(p.feed('I') == ImprovFeedResult::NeedMore);   // not 'M' → reset, but the byte is 'I' so → Magic1
    CHECK(p.feed('M') == ImprovFeedResult::NeedMore);
    CHECK(p.feed('P') == ImprovFeedResult::NeedMore);
    CHECK(p.feed('R') == ImprovFeedResult::NeedMore);
    CHECK(p.feed('O') == ImprovFeedResult::NeedMore);
    CHECK(p.feed('V') == ImprovFeedResult::NeedMore);
    CHECK(p.feed(kImprovSerialVersion) == ImprovFeedResult::NeedMore);
    CHECK(p.feed(static_cast<uint8_t>(ImprovFrameType::CurrentState)) == ImprovFeedResult::NeedMore);
    CHECK(p.feed(0) == ImprovFeedResult::NeedMore);   // length = 0
    // Checksum of the header so far. Reconstruct: I+I+M+P+R+O+V+version+type+len,
    // but the parser only summed the *accepted* header bytes after the resync (i.e.
    // the second 'I' onwards). Build that sum here to assert correctness.
    const uint8_t accepted[] = {'I','M','P','R','O','V', kImprovSerialVersion,
                                static_cast<uint8_t>(ImprovFrameType::CurrentState), 0};
    CHECK(p.feed(improvChecksum(accepted, sizeof(accepted))) == ImprovFeedResult::FrameReady);
}

TEST_CASE("ImprovFrameParser resyncs on bad version when bad byte is 'I'") {
    // Specific regression for the State::Version resync branch added in the
    // fix-pack: when the byte after MagicV isn't kImprovSerialVersion but
    // happens to be the magic-start 'I', the parser should re-enter the
    // magic search at Magic1 rather than discarding the 'I' and losing the
    // start of a new frame that arrives right after a corrupted header.
    ImprovFrameParser p;
    // Feed a half-frame followed by a bad version byte that happens to be 'I',
    // then the rest of a fresh well-formed frame starting at that 'I'.
    CHECK(p.feed('I') == ImprovFeedResult::NeedMore);    // Magic0 → Magic1
    CHECK(p.feed('M') == ImprovFeedResult::NeedMore);
    CHECK(p.feed('P') == ImprovFeedResult::NeedMore);
    CHECK(p.feed('R') == ImprovFeedResult::NeedMore);
    CHECK(p.feed('O') == ImprovFeedResult::NeedMore);
    CHECK(p.feed('V') == ImprovFeedResult::NeedMore);    // → Version state
    CHECK(p.feed('I') == ImprovFeedResult::NeedMore);    // bad version, but 'I' → Magic1
    // Now finish a well-formed CurrentState frame from this re-entered 'I':
    CHECK(p.feed('M') == ImprovFeedResult::NeedMore);
    CHECK(p.feed('P') == ImprovFeedResult::NeedMore);
    CHECK(p.feed('R') == ImprovFeedResult::NeedMore);
    CHECK(p.feed('O') == ImprovFeedResult::NeedMore);
    CHECK(p.feed('V') == ImprovFeedResult::NeedMore);
    CHECK(p.feed(kImprovSerialVersion) == ImprovFeedResult::NeedMore);
    CHECK(p.feed(static_cast<uint8_t>(ImprovFrameType::CurrentState)) == ImprovFeedResult::NeedMore);
    CHECK(p.feed(0) == ImprovFeedResult::NeedMore);
    const uint8_t accepted[] = {'I','M','P','R','O','V', kImprovSerialVersion,
                                static_cast<uint8_t>(ImprovFrameType::CurrentState), 0};
    CHECK(p.feed(improvChecksum(accepted, sizeof(accepted))) == ImprovFeedResult::FrameReady);
}

TEST_CASE("ImprovFrameParser round-trips builder output for every frame type") {
    const std::vector<ImprovFrameType> types = {
        ImprovFrameType::CurrentState,
        ImprovFrameType::ErrorState,
        ImprovFrameType::Rpc,
        ImprovFrameType::RpcResponse,
    };
    const uint8_t payload[] = {'h','e','l','l','o'};
    for (auto t : types) {
        uint8_t frame[32];
        size_t n = buildImprovFrame(t, payload, sizeof(payload), frame, sizeof(frame));
        REQUIRE(n > 0);

        ImprovFrameParser p;
        REQUIRE(feedAll(p, frame, n) == ImprovFeedResult::FrameReady);
        CHECK(p.lastType() == static_cast<uint8_t>(t));
        CHECK(p.lastPayloadLen() == sizeof(payload));
        CHECK(std::memcmp(p.lastPayload(), payload, sizeof(payload)) == 0);
    }
}

TEST_CASE("ImprovFrameParser handles back-to-back frames on the same instance") {
    const uint8_t pa[] = {0x11};
    const uint8_t pb[] = {0x22, 0x33};
    uint8_t fa[16], fb[16];
    size_t na = buildImprovFrame(ImprovFrameType::Rpc, pa, 1, fa, sizeof(fa));
    size_t nb = buildImprovFrame(ImprovFrameType::RpcResponse, pb, 2, fb, sizeof(fb));
    REQUIRE(na > 0);
    REQUIRE(nb > 0);

    ImprovFrameParser p;
    CHECK(feedAll(p, fa, na) == ImprovFeedResult::FrameReady);
    CHECK(p.lastType() == static_cast<uint8_t>(ImprovFrameType::Rpc));
    CHECK(p.lastPayload()[0] == 0x11);

    // Parser must return to Magic0 after FrameReady so a second frame parses cleanly.
    CHECK(feedAll(p, fb, nb) == ImprovFeedResult::FrameReady);
    CHECK(p.lastType() == static_cast<uint8_t>(ImprovFrameType::RpcResponse));
    CHECK(p.lastPayloadLen() == 2);
    CHECK(p.lastPayload()[0] == 0x22);
    CHECK(p.lastPayload()[1] == 0x33);
}
