#pragma once

// Desktop platform configuration — always uses larger types (PSRAM-like)

#include <cstdint>

namespace mm::platform {

constexpr bool hasPsram = true;

// Not an ESP32 chip family — chip-specific drivers (RMT LED, LCD_CAM) `if
// constexpr` these false and compile to inert stubs on desktop.
constexpr bool isEsp32 = false;
constexpr bool isEsp32S3 = false;

// No RMT peripheral — the RMT LED driver guards on this and is inert on desktop.
constexpr uint8_t rmtTxChannels = 0;

// No LCD_CAM peripheral — the LCD LED driver guards on this and is inert too.
constexpr uint8_t lcdLanes = 0;

// No Parlio peripheral — the Parlio LED driver guards on this and is inert too.
constexpr uint8_t parlioLanes = 0;

// No I2S microphone — AudioModule guards on this and is inert on desktop. The
// audioFft seam still has a (naive-DFT) desktop implementation so the audio
// band math runs end-to-end in host tests; only live capture is absent.
constexpr bool hasI2sMic = false;

// Desktop is not a target of the Ethernet-only firmware profile; it ships
// WiFi stubs and exercises the hasWiFi==true code path for compile coverage.
constexpr bool hasWiFi = true;

// Desktop has no separate WiFi co-processor (the ESP32-P4 + C6 case); the
// coprocessorWifi() read-out and its SystemModule control compile out here.
constexpr bool hasWifiCoprocessor = false;

// OTA writes to an ESP-IDF OTA partition; desktop has none. FirmwareUpdateModule
// + the /api/firmware/url route `if constexpr (hasOta)` to a 501 stub instead.
constexpr bool hasOta = false;

// Improv WiFi reads from UART0; desktop has neither a UART nor a WiFi stack.
// ImprovProvisioningModule's setup() `if constexpr (hasImprov)` skips the
// listener-install on desktop.
constexpr bool hasImprov = false;

} // namespace mm::platform
