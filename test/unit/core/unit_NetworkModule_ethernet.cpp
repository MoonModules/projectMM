// @module NetworkModule

// Unit tests for the runtime Ethernet PHY/pin config seam.
//
// The per-PHY bring-up (RMII EMAC, W5500 SPI) lives in platform_esp32.cpp and is
// ESP32-only, so it isn't reachable on the desktop host. What IS host-testable —
// and what these tests pin — are the two contracts the rest of the system is
// built on:
//
//   1. The EthPhyType enum *ordering*. NetworkModule's `ethType` Select dropdown
//      stores the option index, and platform_esp32.cpp's ethInit() switches on
//      the same values; both assume None=0, LAN8720=1, IP101=2, W5500=3. A silent
//      reorder of the enum would desync the dropdown labels from the dispatch and
//      from every boards.json `ethType` value — caught here, not on hardware.
//
//   2. The desktop platform seam (setEthConfig / ethStop / ethInit) is a safe
//      no-op. NetworkModule::setup() calls setEthConfig() then ethInit() on every
//      platform; the desktop stubs must accept any config and report "no Ethernet"
//      (ethInit()==false) so the WiFi/AP cascade always takes over. This guards
//      the platform.h contract that lets the shared NetworkModule code compile and
//      run unchanged on the host.
//
// The conditional-visibility of the eth pin controls (RMII rows vs SPI rows by
// ethType) is gated behind `if constexpr (platform::hasEthernet)`, which is false
// on desktop, so it can't be exercised here; it's verified on real ESP32 hardware
// (Olimex RMII, S3 W5500) instead.

#include "doctest.h"
#include "platform_config.h"   // EthPhyType, EthPinConfig, hasEthernet, ethConfigDefault
#include "platform/platform.h" // setEthConfig / ethStop / ethInit / ethConnected

// The enum values are a wire contract: the Select index, the ethInit() switch, and
// every boards.json `ethType` all agree on these. Pin them so a reorder fails here.
TEST_CASE("EthPhyType enum values match the dropdown/dispatch contract") {
    CHECK(mm::platform::ethNone    == 0);
    CHECK(mm::platform::ethLan8720 == 1);
    CHECK(mm::platform::ethIp101   == 2);
    CHECK(mm::platform::ethW5500   == 3);
}

// Desktop has no Ethernet: the default PHY type is ethNone, so a board that never
// pushes an eth config still reports "no Ethernet" and the cascade falls through.
TEST_CASE("Desktop ethConfigDefault is ethNone (no Ethernet)") {
    CHECK_FALSE(mm::platform::hasEthernet);
    CHECK(mm::platform::ethConfigDefault.phyType == mm::platform::ethNone);
}

// The platform seam must accept any runtime config and never bring Ethernet up on
// desktop — ethInit() returns false so NetworkModule cascades to WiFi/AP. Pushing a
// fully-populated W5500 config and an RMII config both leave ethInit() false and
// ethConnected() false; ethStop() is safe to call when nothing is running.
TEST_CASE("Desktop Ethernet seam is a safe no-op") {
    mm::platform::EthPinConfig w5500{ mm::platform::ethW5500, 1,
                                      -1, -1, -1, -1, false,
                                      /*miso*/ 5, /*mosi*/ 6, /*sck*/ 7, /*cs*/ 15, /*irq*/ 18 };
    mm::platform::setEthConfig(w5500);
    CHECK_FALSE(mm::platform::ethInit());
    CHECK_FALSE(mm::platform::ethConnected());

    mm::platform::EthPinConfig rmii{ mm::platform::ethLan8720, 0,
                                     -1, -1, /*rst*/ 5, /*clk*/ 17, false,
                                     -1, -1, -1, -1, -1 };
    mm::platform::setEthConfig(rmii);
    CHECK_FALSE(mm::platform::ethInit());
    CHECK_FALSE(mm::platform::ethConnected());

    mm::platform::ethStop();   // safe even though nothing came up
    CHECK_FALSE(mm::platform::ethConnected());

    // Restore the platform default so this test leaves no shared eth-config state
    // for later tests (setEthConfig writes a static on ESP32; a no-op on desktop,
    // but keep the test order-independent regardless of platform).
    mm::platform::setEthConfig(mm::platform::ethConfigDefault);
}
