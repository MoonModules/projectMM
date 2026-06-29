#pragma once

// Desktop platform configuration — always uses larger types (PSRAM-like)

#include <cstdint>

namespace mm::platform {

constexpr bool hasPsram = true;

// Not an ESP32-P4, so the P4-specific seams (Ethernet pin map, co-processor WiFi)
// compile out on desktop. Mirrors the esp32 config, which keeps only isEsp32P4
// (the general isEsp32/isEsp32S3 family flags had no users and were removed).
constexpr bool isEsp32P4 = false;

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

// Audio-codec config type — desktop has no codec (audioCodecInit stubs to true),
// but platform.h declares audioCodecInit(CodecType, const AudioCodecPins&, …) for
// every platform, so the types must exist here too. Mirror the esp32 definitions;
// desktop is always CodecType::None.
enum class CodecType : uint8_t { None = 0, Es8311 = 1 };
struct AudioCodecPins { uint16_t i2cSda; uint16_t i2cScl; uint16_t mclk; uint8_t i2cAddr; };
constexpr CodecType audioCodecType = CodecType::None;
constexpr AudioCodecPins audioCodecPins = { 0, 0, 0, 0 };

// Desktop is not a target of the Ethernet-only firmware profile; it ships
// WiFi stubs and exercises the hasWiFi==true code path for compile coverage.
constexpr bool hasWiFi = true;

// Ethernet PHY config type — desktop has no Ethernet (ethInit() stubs to false),
// but platform.h declares setEthConfig(const EthPinConfig&) for every platform, so
// the type must exist here too. Mirror the esp32 struct; the desktop stub ignores it.
enum EthPhyType { ethNone = 0, ethLan8720 = 1, ethIp101 = 2, ethW5500 = 3 };
struct EthPinConfig {
    int phyType; int phyAddr;
    int mdcGpio; int mdioGpio; int rstGpio; int rmiiClockGpio; bool rmiiClockExtIn;
    int spiMiso; int spiMosi; int spiSck; int spiCs; int spiIrq;
};
// Desktop has no Ethernet — hasEthernet false, the default is ethNone. NetworkModule
// (shared code) `if constexpr (hasEthernet)`s the eth controls off, and seeds its
// members from this default; both must exist here for that shared code to compile.
constexpr bool hasEthernet = false;
// No SPI-Ethernet (W5500) driver on desktop either — NetworkModule's live-reconfigure
// path gates on this, so it must exist on every platform (mirrors the esp32 flag).
constexpr bool hasEthW5500 = false;
constexpr EthPinConfig ethConfigDefault{ ethNone, 0, -1, -1, -1, -1, false, -1, -1, -1, -1, -1 };

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
