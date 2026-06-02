// @module ArtNetSendDriver

#include "doctest.h"
#include "light/drivers/ArtNetSendDriver.h"

#include <cstring>

// The built packet contains the exact header layout the Art-Net spec mandates: ID, OpCode, version, sequence, physical, universe, length, data.
TEST_CASE("ArtNet packet header format") {
    uint8_t data[3] = {255, 0, 128};
    uint8_t packet[mm::ArtNetSendDriver::ARTNET_HEADER_SIZE + 3];

    size_t len = mm::ArtNetSendDriver::buildPacket(packet, 0, 42, data, 3);

    CHECK(len == mm::ArtNetSendDriver::ARTNET_HEADER_SIZE + 3);

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
    uint8_t packet[mm::ArtNetSendDriver::ARTNET_HEADER_SIZE + 6];

    mm::ArtNetSendDriver::buildPacket(packet, 259, 0, data, 6);

    // Universe 259 = 0x0103, little-endian
    CHECK(packet[14] == 0x03);
    CHECK(packet[15] == 0x01);
}

// 256 RGB lights (768 bytes) split across exactly 2 universes (510 + 258), matching the 510-channel-per-universe cap.
TEST_CASE("ArtNet universe splitting for 256 RGB lights") {
    // 256 RGB lights = 768 bytes = 2 universes (510 + 258)
    constexpr size_t maxPerUniverse = mm::ArtNetSendDriver::MAX_CHANNELS_PER_UNIVERSE;
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
    uint8_t packet[mm::ArtNetSendDriver::ARTNET_HEADER_SIZE + 510];

    mm::ArtNetSendDriver::buildPacket(packet, 0, 0, data, 510);

    // 510 = 0x01FE, big-endian
    CHECK(packet[16] == 0x01);
    CHECK(packet[17] == 0xFE);
}
