// @module NetworkSendDriver

#include "doctest.h"
#include "light/drivers/NetworkSendDriver.h"

#include <cstring>

// The built packet contains the exact header layout the Art-Net spec mandates: ID, OpCode, version, sequence, physical, universe, length, data.
TEST_CASE("ArtNet packet header format") {
    uint8_t data[3] = {255, 0, 128};
    uint8_t packet[mm::ARTNET_HEADER_SIZE + 3];

    size_t len = mm::buildArtDmxPacket(packet, 0, 42, data, 3);

    CHECK(len == mm::ARTNET_HEADER_SIZE + 3);

    // "Art-Net\0" at offset 0
    CHECK(std::memcmp(packet, "Art-Net", 8) == 0);

    // OpCode: 0x5000 little-endian at offset 8
    CHECK(packet[8] == 0x00);
    CHECK(packet[9] == 0x50);

    // Protocol version: 14 big-endian at offset 10
    CHECK(packet[10] == 0x00);
    CHECK(packet[11] == 0x0e);

    // Sequence at offset 12
    CHECK(packet[12] == 42);

    // Physical at offset 13
    CHECK(packet[13] == 0);

    // Universe: 0 little-endian at offset 14
    CHECK(packet[14] == 0x00);
    CHECK(packet[15] == 0x00);

    // Length: 3 big-endian at offset 16
    CHECK(packet[16] == 0x00);
    CHECK(packet[17] == 0x03);

    // Data at offset 18
    CHECK(packet[18] == 255);
    CHECK(packet[19] == 0);
    CHECK(packet[20] == 128);
}

// Universe 259 (0x0103) is encoded little-endian (low byte first), matching the Art-Net wire format.
TEST_CASE("ArtNet packet with non-zero universe") {
    uint8_t data[6] = {1, 2, 3, 4, 5, 6};
    uint8_t packet[mm::ARTNET_HEADER_SIZE + 6];

    mm::buildArtDmxPacket(packet, 259, 0, data, 6);

    // Universe 259 = 0x0103, little-endian
    CHECK(packet[14] == 0x03);
    CHECK(packet[15] == 0x01);
}

// 256 RGB lights (768 bytes) split across exactly 2 universes (510 + 258), matching the 510-channel-per-universe cap.
TEST_CASE("ArtNet universe splitting for 256 RGB lights") {
    // 256 RGB lights = 768 bytes = 2 universes (510 + 258)
    constexpr size_t maxPerUniverse = mm::MAX_CHANNELS_PER_UNIVERSE;
    constexpr size_t totalBytes = 256 * 3;

    size_t universeCount = 0;
    size_t sent = 0;
    while (sent < totalBytes) {
        size_t chunk = totalBytes - sent;
        if (chunk > maxPerUniverse) chunk = maxPerUniverse;
        sent += chunk;
        universeCount++;
    }

    CHECK(universeCount == 2);
    CHECK(sent == totalBytes);
}

// The data-length field is encoded big-endian (high byte first), unlike the universe field — matching the Art-Net spec.
TEST_CASE("ArtNet packet length field is big-endian") {
    uint8_t data[510];
    std::memset(data, 0, sizeof(data));
    uint8_t packet[mm::ARTNET_HEADER_SIZE + 510];

    mm::buildArtDmxPacket(packet, 0, 0, data, 510);

    // 510 = 0x01FE, big-endian
    CHECK(packet[16] == 0x01);
    CHECK(packet[17] == 0xFE);
}

// The built E1.31 packet carries the exact ACN layout strict sACN receivers (and tools like xLights) validate: identifier, the three flags+length fields, CID, source name, priority, universe, property count, start code.
TEST_CASE("E1.31 packet header format") {
    uint8_t cid[mm::E131_CID_LENGTH];
    for (uint8_t i = 0; i < mm::E131_CID_LENGTH; i++) cid[i] = static_cast<uint8_t>(0xC0 + i);
    uint8_t data[3] = {255, 0, 128};
    uint8_t pkt[mm::E131_HEADER_SIZE + 3];

    const size_t len = mm::buildE131Packet(pkt, 0x0103, 42, cid, data, 3);
    REQUIRE(len == mm::E131_HEADER_SIZE + 3);   // totalLen = 129

    // Root layer: preamble, ACN identifier, flags+length (0x7000 | 129-16), vector, CID.
    CHECK(pkt[0] == 0x00); CHECK(pkt[1] == 0x10);
    CHECK(std::memcmp(pkt + 4, "ASC-E1.17\0\0\0", 12) == 0);
    CHECK(pkt[16] == 0x70); CHECK(pkt[17] == 113);
    CHECK(pkt[21] == 0x04);
    CHECK(std::memcmp(pkt + 22, cid, mm::E131_CID_LENGTH) == 0);

    // Framing layer: flags+length (129-38=91), vector, source name, priority 100,
    // sequence, universe big-endian.
    CHECK(pkt[38] == 0x70); CHECK(pkt[39] == 91);
    CHECK(pkt[43] == 0x02);
    CHECK(std::strcmp(reinterpret_cast<const char*>(pkt + 44), "projectMM") == 0);
    CHECK(pkt[108] == 100);
    CHECK(pkt[111] == 42);
    CHECK(pkt[113] == 0x01); CHECK(pkt[114] == 0x03);

    // DMP layer: flags+length (129-115=14), vector, address/data type, increment,
    // property count = 1 + 3, start code 0; then the data.
    CHECK(pkt[115] == 0x70); CHECK(pkt[116] == 14);
    CHECK(pkt[117] == 0x02);
    CHECK(pkt[118] == 0xA1);
    CHECK(pkt[122] == 0x01);
    CHECK(pkt[123] == 0x00); CHECK(pkt[124] == 4);
    CHECK(pkt[125] == 0x00);
    CHECK(pkt[126] == 255); CHECK(pkt[127] == 0); CHECK(pkt[128] == 128);
}

// The built DDP packet carries version+push bits, RGB data type, default destination, and big-endian offset/length.
TEST_CASE("DDP packet header format") {
    uint8_t data[3] = {255, 0, 128};
    uint8_t pkt[mm::DDP_HEADER_SIZE + 3];

    const size_t len = mm::buildDdpPacket(pkt, 0x01020304u, /*push=*/false, data, 3);
    REQUIRE(len == mm::DDP_HEADER_SIZE + 3);
    CHECK(pkt[0] == 0x40);                    // version 01, push clear
    CHECK(pkt[2] == 0x01);                    // RGB
    CHECK(pkt[3] == 0x01);                    // default display
    CHECK(pkt[4] == 0x01); CHECK(pkt[5] == 0x02);
    CHECK(pkt[6] == 0x03); CHECK(pkt[7] == 0x04);   // offset big-endian
    CHECK(pkt[8] == 0x00); CHECK(pkt[9] == 0x03);   // length big-endian
    CHECK(pkt[10] == 255);

    mm::buildDdpPacket(pkt, 0, /*push=*/true, data, 3);
    CHECK(pkt[0] == 0x41);                    // push set on the frame's last packet
}
