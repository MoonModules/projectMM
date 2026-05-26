#pragma once

// ESP32 platform configuration — PSRAM detected via sdkconfig

#include "sdkconfig.h"

namespace mm::platform {

#ifdef CONFIG_SPIRAM
constexpr bool hasPsram = true;
#else
constexpr bool hasPsram = false;
#endif

// WiFi is compiled out in the Ethernet-only build profile. ESP-IDF v6.x has no
// CONFIG_ESP_WIFI_ENABLED switch, so the eth-only build instead drops the WiFi
// components via EXCLUDE_COMPONENTS and defines MM_NO_WIFI (see esp32/main/CMakeLists.txt).
#ifdef MM_NO_WIFI
constexpr bool hasWiFi = false;
#else
constexpr bool hasWiFi = true;
#endif

// Ethernet is only available on boards whose sdkconfig fragment enables the
// ESP32 EMAC (sdkconfig.defaults.eth — Olimex pin map). Other boards (plain
// ESP32 WiFi-only, ESP32-S3 with no EMAC) define MM_NO_ETH and get stubbed-out
// platform::eth* functions, mirroring the desktop platform layer.
#ifdef MM_NO_ETH
constexpr bool hasEthernet = false;
#else
constexpr bool hasEthernet = true;
#endif

// OTA (esp_https_ota) is available on every ESP32 build — the OTA partition
// layout in partitions/*.csv reserves app0/app1 unconditionally, and esp_https_ota
// is in baseline ESP-IDF. FirmwareUpdateModule + the /api/firmware/url route
// `if constexpr` on this so desktop builds get a 501-returning stub instead.
constexpr bool hasOta = true;

// Improv WiFi listens on UART0 for WiFi credentials. Disabled on Ethernet-only
// builds (--board esp32-eth) — the WiFi headers and the esp_wifi_scan_* calls
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
