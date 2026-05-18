#include "doctest.h"
#include "light/modules/drivers/ArtNetSendDriver.h"

using namespace mm::light;

TEST_CASE("ArtNetSendDriver name and controls") {
    ArtNetSendDriver driver;
    driver.addControls();
    CHECK(std::strcmp(driver.name(), "ArtNet Send") == 0);
    CHECK(driver.controlCount() == 3);
}

TEST_CASE("ArtNet packet header format") {
    uint8_t packet[ArtNetSendDriver::HEADER_SIZE + 512] = {};
    RGB pixels[3] = {{10, 20, 30}, {40, 50, 60}, {70, 80, 90}};

    ArtNetSendDriver::buildPacket(packet, 0, 9, pixels, 3);

    // "Art-Net\0"
    CHECK(std::memcmp(packet, "Art-Net", 8) == 0);
    // OpCode: 0x5000 little-endian
    CHECK(packet[8] == 0x00);
    CHECK(packet[9] == 0x50);
    // Protocol version: 14 big-endian
    CHECK(packet[10] == 0x00);
    CHECK(packet[11] == 14);
    // Universe 0
    CHECK(packet[14] == 0);
    CHECK(packet[15] == 0);
    // Length: 9 big-endian
    CHECK(packet[16] == 0);
    CHECK(packet[17] == 9);
}

TEST_CASE("ArtNet packet pixel data") {
    uint8_t packet[ArtNetSendDriver::HEADER_SIZE + 512] = {};
    RGB pixels[2] = {{10, 20, 30}, {40, 50, 60}};

    ArtNetSendDriver::buildPacket(packet, 0, 6, pixels, 2);

    CHECK(packet[18] == 10); // r
    CHECK(packet[19] == 20); // g
    CHECK(packet[20] == 30); // b
    CHECK(packet[21] == 40);
    CHECK(packet[22] == 50);
    CHECK(packet[23] == 60);
}

TEST_CASE("ArtNet packet universe field") {
    uint8_t packet[ArtNetSendDriver::HEADER_SIZE + 512] = {};
    RGB pixel = {0, 0, 0};

    ArtNetSendDriver::buildPacket(packet, 5, 3, &pixel, 1);
    CHECK(packet[14] == 5); // universe low byte
    CHECK(packet[15] == 0); // universe high byte

    ArtNetSendDriver::buildPacket(packet, 256, 3, &pixel, 1);
    CHECK(packet[14] == 0);  // 256 low byte
    CHECK(packet[15] == 1);  // 256 high byte
}

TEST_CASE("ArtNet universe splitting: 170 pixels = 1 universe") {
    // 170 pixels * 3 = 510 bytes, fits in one universe
    size_t pixels = 170;
    size_t universes = (pixels + ArtNetSendDriver::PIXELS_PER_UNIVERSE - 1)
                     / ArtNetSendDriver::PIXELS_PER_UNIVERSE;
    CHECK(universes == 1);
}

TEST_CASE("ArtNet universe splitting: 171 pixels = 2 universes") {
    size_t pixels = 171;
    size_t universes = (pixels + ArtNetSendDriver::PIXELS_PER_UNIVERSE - 1)
                     / ArtNetSendDriver::PIXELS_PER_UNIVERSE;
    CHECK(universes == 2);
}
