// @module ArtNetReceiveEffect
// @also ArtNetSendDriver

#include "doctest.h"
#include "light/ArtNetPacket.h"
#include "light/effects/ArtNetReceiveEffect.h"
#include "light/layouts/GridLayout.h"
#include "platform/platform.h"

#include <cstring>

// These tests pin the receive side of the shared OpDmx wire format: parser
// accept/reject, universe→buffer placement and clamping (via the public
// applyDmx test surface — no sockets needed), the staging-buffer lifecycle
// (sized off the hot path, never reallocated by loop, freed on teardown), and
// one real localhost UDP round-trip that exercises the platform bind/recvFrom
// path end to end on desktop CI.

namespace {

// The standard rig from the effect tests (unit_NoiseEffect.cpp shape): a grid
// layout + layer with the effect as child. 16×16 RGB = 768 bytes = universes
// {0: bytes 0..509, 1: bytes 510..767} at universe_start 0.
struct Rig {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    mm::ArtNetReceiveEffect fx;

    explicit Rig(mm::lengthType w = 16, mm::lengthType h = 16) {
        grid.width = w;
        grid.height = h;
        grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.addChild(&fx);
        fx.onBuildControls();
        layer.onBuildState();
    }
};

} // namespace

// --- wire format (shared header) ---------------------------------------------

// A packet built by the sender's builder parses back to the same universe and payload — the two sides can't drift.
TEST_CASE("ArtNet OpDmx build→parse round-trip") {
    uint8_t payload[6] = {1, 2, 3, 4, 5, 6};
    uint8_t pkt[mm::ARTNET_HEADER_SIZE + 6];
    const size_t len = mm::buildArtDmxPacket(pkt, 259, 7, payload, 6);

    uint16_t universe = 0, dataLen = 0;
    const uint8_t* data = nullptr;
    REQUIRE(mm::parseArtDmxPacket(pkt, len, universe, data, dataLen));
    CHECK(universe == 259);
    CHECK(dataLen == 6);
    CHECK(std::memcmp(data, payload, 6) == 0);
}

// Bad magic, non-OpDmx opcodes, truncated headers, and lying length fields are all rejected — the receiver drops them.
TEST_CASE("ArtNet OpDmx parse rejects malformed packets") {
    uint8_t payload[3] = {9, 9, 9};
    uint8_t pkt[mm::ARTNET_HEADER_SIZE + 3];
    const size_t len = mm::buildArtDmxPacket(pkt, 0, 0, payload, 3);

    uint16_t universe = 0, dataLen = 0;
    const uint8_t* data = nullptr;

    // Bad magic.
    uint8_t bad[mm::ARTNET_HEADER_SIZE + 3];
    std::memcpy(bad, pkt, len);
    bad[0] = 'X';
    CHECK_FALSE(mm::parseArtDmxPacket(bad, len, universe, data, dataLen));

    // Wrong opcode (OpPoll 0x2000 instead of OpDmx 0x5000).
    std::memcpy(bad, pkt, len);
    bad[9] = 0x20;
    CHECK_FALSE(mm::parseArtDmxPacket(bad, len, universe, data, dataLen));

    // Truncated header.
    CHECK_FALSE(mm::parseArtDmxPacket(pkt, mm::ARTNET_HEADER_SIZE - 1, universe, data, dataLen));

    // Length field claims more data than the datagram carries.
    std::memcpy(bad, pkt, len);
    bad[16] = 0x01; bad[17] = 0xFE;   // declares 510, datagram has 3
    CHECK_FALSE(mm::parseArtDmxPacket(bad, len, universe, data, dataLen));
}

// --- universe placement (applyDmx, no sockets) --------------------------------

// Universe universe_start lands at byte 0; the next universe lands at byte 510 — the same split the sender uses.
TEST_CASE("ArtNetReceiveEffect places universes at consecutive 510-byte offsets") {
    Rig r;
    uint8_t u0[510], u1[258];
    std::memset(u0, 0xAA, sizeof(u0));
    std::memset(u1, 0xBB, sizeof(u1));

    r.fx.applyDmx(0, u0, sizeof(u0));
    r.fx.applyDmx(1, u1, sizeof(u1));
    r.layer.loop();   // staging → layer buffer

    const uint8_t* buf = r.layer.buffer().data();
    CHECK(buf[0] == 0xAA);
    CHECK(buf[509] == 0xAA);
    CHECK(buf[510] == 0xBB);
    CHECK(buf[767] == 0xBB);
}

// The layer clears its buffer every tick; staging holds the last frame, so the lights don't strobe black between packets.
TEST_CASE("ArtNetReceiveEffect holds the last frame across ticks without new packets") {
    Rig r;
    uint8_t u0[3] = {10, 20, 30};
    r.fx.applyDmx(0, u0, sizeof(u0));

    r.layer.loop();
    r.layer.loop();   // no new packet — must still show the last frame
    const uint8_t* buf = r.layer.buffer().data();
    CHECK(buf[0] == 10);
    CHECK(buf[1] == 20);
    CHECK(buf[2] == 30);
}

// Universes below universe_start are ignored; universes relative to a non-zero start land at offset 0.
TEST_CASE("ArtNetReceiveEffect respects universe_start") {
    Rig r;
    r.fx.universeStart = 5;
    uint8_t below[3] = {1, 1, 1};
    uint8_t at[3] = {7, 8, 9};

    r.fx.applyDmx(4, below, sizeof(below));   // below start — ignored
    r.fx.applyDmx(5, at, sizeof(at));         // at start — offset 0
    r.layer.loop();

    const uint8_t* buf = r.layer.buffer().data();
    CHECK(buf[0] == 7);
    CHECK(buf[1] == 8);
    CHECK(buf[2] == 9);
}

// A payload overrunning the buffer end is clamped; a universe entirely beyond the buffer is ignored.
TEST_CASE("ArtNetReceiveEffect clamps payloads to the buffer") {
    Rig r;   // 768 bytes
    uint8_t u1[510];
    std::memset(u1, 0xCC, sizeof(u1));

    r.fx.applyDmx(1, u1, sizeof(u1));   // offset 510 + 510 bytes > 768 → clamp to 258
    r.fx.applyDmx(2, u1, sizeof(u1));   // offset 1020 — entirely beyond → ignored
    r.layer.loop();

    const uint8_t* buf = r.layer.buffer().data();
    CHECK(buf[510] == 0xCC);
    CHECK(buf[767] == 0xCC);
}

// A 0×0×0 grid accepts packets as a clean no-op — degraded, not crashed.
TEST_CASE("ArtNetReceiveEffect tolerates a zero-light grid") {
    Rig r(0, 0);
    uint8_t u0[3] = {1, 2, 3};
    r.fx.applyDmx(0, u0, sizeof(u0));
    r.layer.loop();
    CHECK(true);   // reaching here without a crash is the assertion
}

// --- staging lifecycle ---------------------------------------------------------

// Staging is sized in onBuildState (off the hot path), loop() never reallocates it, teardown frees it.
TEST_CASE("ArtNetReceiveEffect staging buffer lifecycle") {
    Rig r;
    REQUIRE(r.fx.stagingData() != nullptr);
    CHECK(r.fx.stagingBytes() == r.layer.buffer().bytes());

    const uint8_t* before = r.fx.stagingData();
    r.layer.loop();
    CHECK(r.fx.stagingData() == before);   // no realloc in the hot path

    r.fx.teardown();
    CHECK(r.fx.stagingData() == nullptr);
    CHECK(r.fx.stagingBytes() == 0);
}

// --- localhost round-trip (real UDP through the platform bind/recvFrom path) ---

// A real packet sent over localhost UDP lands in the layer buffer — the end-to-end proof of the platform receive path.
TEST_CASE("ArtNetReceiveEffect receives over localhost UDP") {
    Rig r;
    // Not 6454: a live ArtNet tool on the dev machine must not collide with CI.
    r.fx.port = 16454;
    r.fx.setup();
    REQUIRE(r.fx.status() == nullptr);   // bind succeeded

    mm::platform::UdpSocket tx;
    REQUIRE(tx.open());
    REQUIRE(tx.connect("127.0.0.1", 16454));
    uint8_t payload[3] = {42, 43, 44};
    uint8_t pkt[mm::ARTNET_HEADER_SIZE + 3];
    const size_t len = mm::buildArtDmxPacket(pkt, 0, 0, payload, 3);
    REQUIRE(tx.sendTo(pkt, len));

    // UDP on loopback is reliable but asynchronous — poll the frame loop with a
    // bounded retry (≤100 ms) so CI stays deterministic.
    bool landed = false;
    for (int i = 0; i < 100 && !landed; i++) {
        r.layer.loop();
        const uint8_t* buf = r.layer.buffer().data();
        landed = buf[0] == 42 && buf[1] == 43 && buf[2] == 44;
        if (!landed) mm::platform::delayMs(1);
    }
    CHECK(landed);

    tx.close();
    r.fx.teardown();
}
