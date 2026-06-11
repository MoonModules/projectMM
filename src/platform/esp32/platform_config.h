#pragma once

// ESP32 platform configuration — PSRAM detected via sdkconfig

#include "sdkconfig.h"

#include <cstdint>

namespace mm::platform {

#ifdef CONFIG_SPIRAM
constexpr bool hasPsram = true;
#else
constexpr bool hasPsram = false;
#endif

// Which ESP32 silicon family this build targets. Drivers that pick a peripheral
// per chip (the RMT LED driver runs on classic ESP32; LCD_CAM on the S3 later)
// `if constexpr` on these instead of #ifdef'ing in domain code. Desktop sets all
// false. Keyed off the IDF target macro the toolchain defines. Note most
// capability gating (rmtTxChannels, lcdLanes) keys off SOC flags rather than
// these family flags, so a new target works untouched — isEsp32P4 exists only
// for the genuinely chip-specific seams (the P4's Ethernet pin map, and later
// its C6-co-processor WiFi).
#ifdef CONFIG_IDF_TARGET_ESP32S3
constexpr bool isEsp32 = false;
constexpr bool isEsp32S3 = true;
constexpr bool isEsp32P4 = false;
#elif defined(CONFIG_IDF_TARGET_ESP32P4)
constexpr bool isEsp32 = false;
constexpr bool isEsp32S3 = false;
constexpr bool isEsp32P4 = true;
#elif defined(CONFIG_IDF_TARGET_ESP32)
constexpr bool isEsp32 = true;
constexpr bool isEsp32S3 = false;
constexpr bool isEsp32P4 = false;
#else
constexpr bool isEsp32 = false;
constexpr bool isEsp32S3 = false;
constexpr bool isEsp32P4 = false;
#endif

// RMT TX channels this chip offers (8 on classic ESP32, 4 on the S3, straight
// from the IDF SOC capability config). Doubles as the RMT capability flag: the
// RMT LED driver and its main.cpp registration guard on `rmtTxChannels > 0`
// instead of a chip-family flag, so a new RMT-bearing target works untouched.
#ifdef CONFIG_SOC_RMT_SUPPORTED
constexpr uint8_t rmtTxChannels = CONFIG_SOC_RMT_TX_CANDIDATES_PER_GROUP;
#else
constexpr uint8_t rmtTxChannels = 0;
#endif

// Parallel WS2812 lanes over the LCD_CAM i80 bus (ESP32-S3 among current
// targets). The peripheral does 16; this increment deliberately caps at 8 —
// half the DMA footprint, and widening is a constant change, not a redesign.
// SOC-derived like rmtTxChannels so a future LCD_CAM-bearing chip works
// untouched.
//
// Gate on SOC_LCDCAM_I80_LCD_SUPPORTED (the LCD_CAM peripheral's i80 mode),
// NOT SOC_LCD_I80_SUPPORTED: the classic ESP32 sets the latter for its
// unrelated I2S-LCD peripheral, so gating on it wired this driver onto the
// classic chip and hung its boot trying to init an esp_lcd i80 bus the chip
// doesn't have. SOC_LCDCAM_I80_LCD_SUPPORTED is defined only on chips with the
// real LCD_CAM (S3/P4), which is what esp_lcd's i80 driver actually needs.
#ifdef CONFIG_SOC_LCDCAM_I80_LCD_SUPPORTED
constexpr uint8_t lcdLanes = 8;
#else
constexpr uint8_t lcdLanes = 0;
#endif

// WiFi is compiled out in the Ethernet-only build profile. ESP-IDF v6.x has no
// CONFIG_ESP_WIFI_ENABLED switch, so the eth-only build instead drops the WiFi
// components via EXCLUDE_COMPONENTS and defines MM_NO_WIFI (see esp32/main/CMakeLists.txt).
#ifdef MM_NO_WIFI
constexpr bool hasWiFi = false;
#else
constexpr bool hasWiFi = true;
#endif

// Ethernet is only available on firmware variants whose sdkconfig fragment
// enables the ESP32 EMAC (sdkconfig.defaults.eth — Olimex pin map). Other
// firmwares (plain ESP32 WiFi-only, ESP32-S3 with no EMAC) define MM_NO_ETH
// and get stubbed-out platform::eth* functions, mirroring the desktop layer.
#ifdef MM_NO_ETH
constexpr bool hasEthernet = false;
#else
constexpr bool hasEthernet = true;
#endif

// Per-board Ethernet RMII / PHY pin map. The pins are NOT runtime-configurable
// today (full runtime PHY/pin selection is a 2.0 backlog item); they are a
// compile-time-per-target constant so the platform boundary stays clean — no
// scattered #ifdefs in ethInit(), which reads this struct instead of literals.
// Plain ints (not IDF enums) keep this header free of esp_eth includes; ethInit
// translates rmiiClockExtIn → EMAC_CLK_EXT_IN/OUT and isIp101 → the PHY ctor.
struct EthPinConfig {
    int phyAddr;
    int mdcGpio;        // SMI clock; -1 = leave at IDF default
    int mdioGpio;       // SMI data;  -1 = leave at IDF default
    int rstGpio;        // PHY reset
    int rmiiClockGpio;  // RMII 50 MHz reference clock pin
    bool rmiiClockExtIn;  // true = clock IN (board feeds it), false = chip drives it OUT
    bool isIp101;       // true = IP101 PHY ctor, false = generic
};

// ESP32-P4-NANO (Waveshare): IP101 PHY, addr 1, MDC/MDIO 31/52, reset 51,
// external 50 MHz RMII clock fed IN on GPIO50. Source: Waveshare wiki +
// schematic + the ESPHome device page (two independent sources agree).
// Else: Olimex ESP32-Gateway Rev G — LAN8720 (generic PHY), addr 0, reset 5,
// chip drives the RMII clock OUT on GPIO17, MDC/MDIO left at IDF defaults (the
// pins this board has always used; now a named config instead of literals).
constexpr EthPinConfig ethPins =
    isEsp32P4 ? EthPinConfig{ /*phyAddr*/ 1, /*mdc*/ 31, /*mdio*/ 52, /*rst*/ 51,
                              /*rmiiClk*/ 50, /*extIn*/ true,  /*ip101*/ true }
              : EthPinConfig{ /*phyAddr*/ 0, /*mdc*/ -1, /*mdio*/ -1, /*rst*/ 5,
                              /*rmiiClk*/ 17, /*extIn*/ false, /*ip101*/ false };

// OTA (esp_https_ota) is available on every ESP32 build — the OTA partition
// layout in partitions/*.csv reserves app0/app1 unconditionally, and esp_https_ota
// is in baseline ESP-IDF. FirmwareUpdateModule + the /api/firmware/url route
// `if constexpr` on this so desktop builds get a 501-returning stub instead.
constexpr bool hasOta = true;

// Improv WiFi listens on UART0 for WiFi credentials. Disabled on Ethernet-only
// firmwares (--firmware esp32-eth) — the WiFi headers and the esp_wifi_scan_* calls
// the listener uses are not linked there, and there's no WiFi STA to provision
// either way. The S3's native USB-Serial-JTAG (separate from UART0) is not
// supported by the Improv listener; see the ImprovProvisioningModule spec for
// the user-facing footnote.
#ifdef MM_NO_WIFI
constexpr bool hasImprov = false;
#else
constexpr bool hasImprov = true;
#endif

} // namespace mm::platform
