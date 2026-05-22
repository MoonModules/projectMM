#pragma once

// Desktop platform configuration — always uses larger types (PSRAM-like)

namespace mm::platform {

constexpr bool hasPsram = true;

// Desktop is not a target of the Ethernet-only firmware profile; it ships
// WiFi stubs and exercises the hasWiFi==true code path for compile coverage.
constexpr bool hasWiFi = true;

} // namespace mm::platform
