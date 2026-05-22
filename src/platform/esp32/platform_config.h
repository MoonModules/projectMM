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

} // namespace mm::platform
