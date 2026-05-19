#include "doctest.h"
#include "core/Scheduler.h"
#include "light/GridLayout.h"
#include "light/RainbowEffect.h"
#include "light/ArtNetSendDriver.h"

TEST_CASE("Full pipeline produces non-zero buffer data") {
    mm::Scheduler scheduler;

    mm::LayoutGroup layoutGroup;
    mm::GridLayout grid;
    grid.width = 4;
    grid.height = 4;
    grid.depth = 1;
    layoutGroup.addLayout(&grid);

    mm::Layer layer;
    layer.setLayoutGroup(&layoutGroup);
    layer.setChannelsPerLight(3);

    mm::RainbowEffect rainbow;
    layer.addEffect(&rainbow);

    mm::DriverGroup driverGroup;
    driverGroup.setLayer(&layer);

    mm::ArtNetSendDriver artnet;
    artnet.fps = 120; // high fps so it sends on first tick
    driverGroup.addDriver(&artnet);

    scheduler.addModule(&layoutGroup);
    scheduler.addModule(&grid);
    scheduler.addModule(&layer);
    scheduler.addModule(&rainbow);
    scheduler.addModule(&driverGroup);
    scheduler.addModule(&artnet);

    scheduler.setup();

    // Run a tick to render one frame
    scheduler.tick();

    // Verify layer buffer has data
    auto& buf = layer.buffer();
    REQUIRE(buf.data() != nullptr);
    REQUIRE(buf.count() == 16);
    REQUIRE(buf.bytes() == 48);

    bool hasNonZero = false;
    for (size_t i = 0; i < buf.bytes(); i++) {
        if (buf.data()[i] != 0) { hasNonZero = true; break; }
    }
    CHECK(hasNonZero);

    scheduler.teardown();
}

TEST_CASE("ArtNet buildPacket produces valid packet from pipeline data") {
    mm::LayoutGroup layoutGroup;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layoutGroup.addLayout(&grid);

    mm::Layer layer;
    layer.setLayoutGroup(&layoutGroup);
    layer.setChannelsPerLight(3);

    mm::RainbowEffect rainbow;
    layer.addEffect(&rainbow);

    layer.onAllocateMemory();
    layer.loop(); // render one frame

    auto& buf = layer.buffer();

    // Build a packet from the first universe worth of data
    uint8_t packet[mm::ArtNetSendDriver::ARTNET_HEADER_SIZE + mm::ArtNetSendDriver::MAX_CHANNELS_PER_UNIVERSE];
    uint16_t chunkLen = static_cast<uint16_t>(
        buf.bytes() > mm::ArtNetSendDriver::MAX_CHANNELS_PER_UNIVERSE
            ? mm::ArtNetSendDriver::MAX_CHANNELS_PER_UNIVERSE
            : buf.bytes()
    );
    size_t packetLen = mm::ArtNetSendDriver::buildPacket(packet, 0, 1, buf.data(), chunkLen);

    // Verify packet structure
    CHECK(packetLen == mm::ArtNetSendDriver::ARTNET_HEADER_SIZE + chunkLen);
    CHECK(std::memcmp(packet, "Art-Net", 8) == 0);
    CHECK(packet[12] == 1); // sequence

    // Verify the data portion contains the rainbow pattern
    bool dataNonZero = false;
    for (size_t i = mm::ArtNetSendDriver::ARTNET_HEADER_SIZE; i < packetLen; i++) {
        if (packet[i] != 0) { dataNonZero = true; break; }
    }
    CHECK(dataNonZero);
}

TEST_CASE("Pipeline universe count for 16x16 grid") {
    // 16x16 = 256 lights * 3 channels = 768 bytes
    // 768 / 510 = 2 universes (510 + 258)
    constexpr size_t totalBytes = 256 * 3;
    constexpr size_t maxPerUniverse = mm::ArtNetSendDriver::MAX_CHANNELS_PER_UNIVERSE;

    size_t count = 0;
    size_t sent = 0;
    while (sent < totalBytes) {
        size_t chunk = totalBytes - sent;
        if (chunk > maxPerUniverse) chunk = maxPerUniverse;
        sent += chunk;
        count++;
    }
    CHECK(count == 2);
}
