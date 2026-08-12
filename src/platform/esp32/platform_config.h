#pragma once

// ESP32 platform configuration — PSRAM detected via sdkconfig

#include "sdkconfig.h"

#include <cstdint>

// The RMT TX-channel count moved into the RMT HAL's low-level header when the
// RMT HAL graduated into its own `esp_hal_rmt` component on the v6.1 line (the
// header re-appears at `hal/rmt_ll.h`, now shipped by `esp_hal_rmt` instead of
// the monolithic `hal` component). Espressif also removed the public
// `SOC_RMT_TX_CANDIDATES_PER_GROUP` soc-cap from `soc/soc_caps.h` in the same
// pass, so the LL symbol is the only per-target definition of the count on
// beta1. Included at file scope (not inside the namespace) so the standard
// headers it pulls land in `::`, and only on RMT-bearing builds.
#ifdef CONFIG_SOC_RMT_SUPPORTED
#include "hal/rmt_ll.h"
#endif

// MM_RAMFUNC — "this function executes from RAM, not flash" (the __ramfunc concept from STM32/Zephyr,
// spelled IRAM_ATTR in ESP-IDF). For code an ISR runs on a tight deadline: flash-resident code shares
// one instruction cache between both cores, so a hot render loop evicts an ISR's code path between
// invocations and every firing pays flash-refetch latency. A macro (not if constexpr) because it is a
// function ATTRIBUTE; defined per-platform here behind the platform boundary, empty on desktop.
#include "esp_attr.h"
#define MM_RAMFUNC IRAM_ATTR

namespace mm::platform {

#ifdef CONFIG_SPIRAM
constexpr bool hasPsram = true;
#else
constexpr bool hasPsram = false;
#endif

// Which ESP32 silicon family this build targets. Capability gating
// (rmtTxChannels, lcdLanes, parlioLanes, hasI2sMic) keys off SOC flags, not the
// family, so most new chips work untouched without a per-chip flag. Only two chips
// earn an `is<Chip>` flag, for seams that aren't SOC-derived: isEsp32P4 (its
// Ethernet pin defaults in `ethConfigDefault` + co-processor WiFi via
// hasWifiCoprocessor) and isEsp32S3 (its W5500-SPI Ethernet default — see below).
// Keyed off the IDF target macro; false on desktop.
#ifdef CONFIG_IDF_TARGET_ESP32P4
constexpr bool isEsp32P4 = true;
#else
constexpr bool isEsp32P4 = false;
#endif

// isEsp32S3 earns its place the same way isEsp32P4 does: a chip-specific seam not
// derivable from a SOC flag — the S3 has no internal EMAC, so its Ethernet default
// is W5500-over-SPI (where classic/P4 default to RMII). Used only for ethConfigDefault.
#ifdef CONFIG_IDF_TARGET_ESP32S3
constexpr bool isEsp32S3 = true;
#else
constexpr bool isEsp32S3 = false;
#endif

// isEsp32S31: the S31 is the only target whose EMAC is RGMII / 1 Gb (SOC_EMAC_SUPPORT_1000M),
// where classic/P4 are RMII — so its Ethernet default is a distinct RGMII PHY (YT8531) with a
// different pin set. Not derivable from a SOC flag (the RGMII data pins are board wiring, not a
// chip property). Used by ethConfigDefault and ethInitEmac's RGMII branch/log.
#ifdef CONFIG_IDF_TARGET_ESP32S31
constexpr bool isEsp32S31 = true;
#else
constexpr bool isEsp32S31 = false;
#endif

// RMT TX channels this chip offers (8 on classic ESP32, 4 on the S3 / P4 / S31,
// straight from the RMT HAL — `RMT_LL_TX_CANDIDATES_PER_INST`, included above).
// Doubles as the RMT capability flag: the RMT LED driver and its main.cpp
// registration guard on `rmtTxChannels > 0` instead of a chip-family flag, so a
// new RMT-bearing target works untouched.
#ifdef CONFIG_SOC_RMT_SUPPORTED
constexpr uint8_t rmtTxChannels = RMT_LL_TX_CANDIDATES_PER_INST;
#else
constexpr uint8_t rmtTxChannels = 0;
#endif

// Parallel WS2812 lanes over the LCD_CAM i80 bus (ESP32-S3 / P4). The peripheral
// does 16 data lines and the driver uses all of them: it derives the actual bus
// width (8 or 16 — power-of-two only) from the configured pin count, so this is
// the ceiling, not a self-imposed cap. SOC-derived like rmtTxChannels so a future
// LCD_CAM-bearing chip works untouched.
//
// Gate on SOC_LCDCAM_I80_LCD_SUPPORTED (the LCD_CAM peripheral's i80 mode),
// NOT SOC_LCD_I80_SUPPORTED: the classic ESP32 sets the latter for its
// unrelated I2S-LCD peripheral, so gating on it wired this driver onto the
// classic chip and hung its boot trying to init an esp_lcd i80 bus the chip
// doesn't have. SOC_LCDCAM_I80_LCD_SUPPORTED is defined only on chips with the
// real LCD_CAM (S3/P4/S31), which is what esp_lcd's i80 driver actually needs.
// The LCD_CAM i80 bus does 16 data lines. The driver derives the actual bus width
// (8 or 16 — power-of-two only) from the configured pin count; this is the MAX it
// may reach. LCD requires exactly 8 or 16 real pins (i80 rejects an NC data line).
#ifdef CONFIG_SOC_LCDCAM_I80_LCD_SUPPORTED
constexpr uint8_t lcdLanes = 16;
#else
constexpr uint8_t lcdLanes = 0;
#endif

// hasLcdCam — is this LCD_CAM silicon (S3/P4/S31)? Separate from the lane COUNT because the host
// sets a non-zero count so the parallel driver RUNS there against a memory bus, while having no
// LCD_CAM at all. On a real chip the two coincide; the pin expander keys off the capability.
constexpr bool hasLcdCam = (lcdLanes > 0);

// Parallel WS2812 lanes over the Parlio (Parallel IO) TX peripheral — the
// ESP32-P4's scale path. The unit does 16 data lines; the driver derives the bus
// width (8 or 16) from the pin count. SOC-derived like the others, so a future
// Parlio-bearing chip works untouched. Unlike i80, Parlio takes the data GPIOs
// directly (no sacrificial WR/DC) and accepts an NC unused lane, so it runs any
// 1..16 count with no exactly-8-or-16 rule.
#ifdef CONFIG_SOC_PARLIO_SUPPORTED
constexpr uint8_t parlioLanes = 16;
#else
constexpr uint8_t parlioLanes = 0;
#endif

// Parallel WS2812 lanes over the classic ESP32's I2S peripheral in LCD/i80 mode — the
// classic chip's ONLY >8-lane route (it has neither LCD_CAM nor Parlio). IDF's esp_lcd
// component backs the SAME esp_lcd i80 API (esp_lcd_new_i80_bus / tx_color, 8-or-16 bus
// width, WR/DC) with the I2S peripheral on the classic ESP32 (esp_lcd_panel_io_i2s.c),
// using WHOLE-FRAME chained DMA — so MultiPinLedDriver reuses the MultiPinLedDriver code path and
// the i80Ws2812* seam, not a bespoke ISR ring. Gate CLASSIC-ONLY: SOC_LCD_I80_SUPPORTED
// is set on the classic chip (I2S backend) AND the LCD_CAM chips (S3/P4/S31, LCD_CAM backend), so
// exclude the LCD_CAM chips — otherwise both this and lcdLanes would be non-zero on those chips and
// the chip would register both drivers. The `defined(A) && !defined(B)` shape mirrors
// hasEthW5500 below. The i80 bus does 16 data lines; the driver derives 8 or 16 from the
// pin count and requires exactly that many real pins (i80 rejects an NC data line).
#if defined(CONFIG_SOC_LCD_I80_SUPPORTED) && !defined(CONFIG_SOC_LCDCAM_I80_LCD_SUPPORTED)
constexpr uint8_t i2sLanes = 16;
#else
constexpr uint8_t i2sLanes = 0;
#endif

// I2S audio input (an INMP441-class digital MEMS microphone). SOC-derived like
// the LED-peripheral flags: every current ESP32 has I2S, so this is true on all
// of them, but the gate keeps AudioService + the I2S platform seam inert on any
// future I2S-less target and on desktop. The audio math (RMS, FFT bands) is
// host-tested domain code; only the I2S read and the FFT kernel sit behind the
// boundary, both guarded by `if constexpr (platform::hasI2sMic)`.
#ifdef CONFIG_SOC_I2S_SUPPORTED
constexpr bool hasI2sMic = true;
#else
constexpr bool hasI2sMic = false;
#endif


// Some boards put the mic behind an I2S audio codec configured over I2C (vs a
// direct I2S MEMS mic). The codec type + its control pins are fixed by the
// firmware's hardware target, so they live here (like ethConfigDefault), not as
// AudioService controls. The I2S data pins (ws/sd/sck) stay user controls.
// `audioCodecInit` (platform.h) consumes these; CodecType is neutral so a second
// codec is just another enum value + a backend branch.
enum class CodecType : uint8_t { None = 0, Es8311 = 1 };
struct AudioCodecPins {
    uint16_t i2cSda;
    uint16_t i2cScl;
    uint16_t mclk;      // I2S master clock the codec needs (separate from BCLK/WS)
    uint8_t  i2cAddr;   // 7-bit codec I2C address (ES8311 default 0x18)
};

// The ESP32-S31 Function-CoreBoard and the dedicated JC-ESP32P4-M3-DEV firmware
// carry an ES8311. Other firmware variants keep the direct-mic/no-codec path.
#ifdef CONFIG_IDF_TARGET_ESP32S31
constexpr CodecType audioCodecType = CodecType::Es8311;
constexpr AudioCodecPins audioCodecPins = { /*sda*/ 51, /*scl*/ 50, /*mclk*/ 52, /*addr*/ 0x18 };
#elif defined(CONFIG_MM_JC_P4_M3)
constexpr CodecType audioCodecType = CodecType::Es8311;
constexpr AudioCodecPins audioCodecPins = { /*sda*/ 7, /*scl*/ 8, /*mclk*/ 13, /*addr*/ 0x18 };
#else
constexpr CodecType audioCodecType = CodecType::None;
constexpr AudioCodecPins audioCodecPins = { 0, 0, 0, 0 };
#endif

// WiFi is compiled out in the Ethernet-only build profile. ESP-IDF v6.x has no
// CONFIG_ESP_WIFI_ENABLED switch, so the eth-only build instead drops the WiFi
// components via EXCLUDE_COMPONENTS and defines MM_NO_WIFI (see esp32/main/CMakeLists.txt).
#ifdef MM_NO_WIFI
constexpr bool hasWiFi = false;
#else
constexpr bool hasWiFi = true;
#endif

// The P4 has no native radio; when it has WiFi at all (the esp32p4-eth-wifi build),
// that WiFi runs on the on-board ESP32-C6 over SDIO via esp_wifi_remote / esp_hosted.
// The esp_wifi_* API is identical to native and esp_hosted self-initialises at boot,
// so the WiFi *path* needs no branch. This flag exists only so the co-processor
// firmware read-out (SystemModule's `wifiCoproc` control + platform::coprocessorWifi)
// compiles in ONLY on a build that actually has a co-processor — on every other
// target the buffer, the calls, and the control vanish (if constexpr), keeping the
// flash/RAM cost off boards that can't use it.
constexpr bool hasWifiCoprocessor = isEsp32P4 && hasWiFi;

// Ethernet is only available on firmware variants whose sdkconfig fragment
// enables the ESP32 EMAC (sdkconfig.defaults.eth — the default LAN8720 RMII pin map). Other
// firmwares (plain ESP32 WiFi-only, ESP32-S3 with no EMAC) define MM_NO_ETH
// and get stubbed-out platform::eth* functions, mirroring the desktop layer.
#ifdef MM_NO_ETH
constexpr bool hasEthernet = false;
#else
constexpr bool hasEthernet = true;
#endif

// True when the firmware carries an IP stack at all — WiFi OR Ethernet. UdpSocket
// (lwIP BSD sockets) is present whenever either is, so features that only need
// "some network" (WLED audio sync, any UDP interop) gate on this rather than
// hasWiFi — an Ethernet-only board (the MHC-WLED P4 shield) still has UDP.
constexpr bool hasNetwork = hasWiFi || hasEthernet;

// Enough compute headroom for a per-pixel FLOAT algorithm — a raymarcher, a fractal, a feedback
// loop that iterates per light. This is the ONE exception to the integer-only render-path rule in
// coding-standards, and it is gated rather than assumed: an effect behind this constant is not
// compiled at all where it is false, so no ESP32 firmware carries the float code and the rule is
// not weakened on the targets it protects.
//
// The cost is per PIXEL, not per chip, so this is a statement about the FPU rather than about
// fixture size: a target with hardware float can run such an effect on a small panel and cannot on
// a large one, and the effect's own controls are what trade quality for cost. The classic ESP32 has
// no FPU at all, so it stays out; the S3 and P4 have single-precision hardware.
// Derived from the SoC capability, not a hand-kept chip list: every ESP32 variant IDF supports
// declares SOC_CPU_HAS_FPU (checked S3, P4, S31 and the classic ESP32 — all 1), so a new target
// inherits the right answer without an edit here. What separates them is SPEED and clock, which
// the effect's own `steps` control and the fixture size decide — not whether the code exists.
constexpr bool hasHeavyCompute = SOC_CPU_HAS_FPU;

// Preprocessor mirror of the flag above: a whole effect can be compiled out only by
// `#if`, which `constexpr bool` cannot drive. Keep the two in step.
#define MM_HEAVY_COMPUTE SOC_CPU_HAS_FPU

// Which Ethernet PHY *drivers* this firmware actually carries. The W5500 SPI
// driver is compiled in only on chips with no internal EMAC and the SPI-eth
// fragment (the S3 — CONFIG_ETH_USE_SPI_ETHERNET set, CONFIG_ETH_USE_ESP32_EMAC
// not). NetworkModule gates the *live* W5500 reconfigure on this: on a classic /
// P4 board (RMII only) ethInit() can't bring up W5500, so the live path must not
// tear down the working RMII interface for a type it can't init. Mirrors the
// MM_ETH_W5500 marker in platform_esp32.cpp; false on desktop (no SPI-eth there).
// hasNamedNetInterfaces — false on every ESP32: one MAC per chip, so a raw sender has nothing to
// choose between and a NIC-name control would do nothing. See the desktop config for the true case.
constexpr bool hasNamedNetInterfaces = false;

#if defined(CONFIG_ETH_USE_SPI_ETHERNET) && !defined(CONFIG_ETH_USE_ESP32_EMAC)
constexpr bool hasEthW5500 = true;
#else
constexpr bool hasEthW5500 = false;
#endif

// Ethernet PHY type. The DRIVER for each type is compiled into the firmware per
// chip (RMII EMAC on classic/P4, W5500 SPI on the S3 — see the sdkconfig
// fragments); WHICH type a given board uses, and its pins, are runtime config
// (deviceModels.json → NetworkModule → platform::setEthConfig). Plain int values keep
// this header free of esp_eth includes; ethInit() maps them to the IDF ctors.
enum EthPhyType {
    ethNone    = 0,  // no Ethernet on this board (the default — WiFi only)
    ethLan8720 = 1,  // RMII, generic PHY (Olimex Gateway, QuinLED Dig-Octa)
    ethIp101   = 2,  // RMII, IP101 PHY (Waveshare P4-NANO; managed component, P4-only)
    ethW5500   = 3,  // SPI, external W5500 module (ESP32-S3 boards — SE16, LightCrafter)
    ethYt8531  = 4,  // RGMII, YT8531 PHY (ESP32-S31 CoreBoard; on-chip 1 Gb EMAC, S31-only)
};

// Per-board Ethernet pin/PHY map — runtime-configurable (no longer a fixed
// compile-time constant). RMII fields apply to LAN8720/IP101; the spi* fields to
// W5500. ethInit() reads the runtime `ethConfig` (set from deviceModels.json via
// platform::setEthConfig); the per-chip `ethConfigDefault` below seeds it so an
// un-provisioned board still works. -1 = "leave at IDF default / unused".
struct EthPinConfig {
    int phyType;        // EthPhyType
    int phyAddr;
    // RMII (internal EMAC):
    int mdcGpio;        // SMI clock; -1 = IDF default
    int mdioGpio;       // SMI data;  -1 = IDF default
    int rstGpio;        // PHY reset
    int rmiiClockGpio;  // RMII 50 MHz reference clock pin
    bool rmiiClockExtIn;  // true = clock IN (board feeds it), false = chip drives it OUT
    // (RMII data lines TX_EN/TXD0/TXD1/CRS_DV/RXD0/RXD1 are not configurable here:
    // fixed in silicon on the classic ESP32, and on the P4 the IDF EMAC macro already
    // defaults them to the NANO wiring (49/34/35/28/29/30). No board varies them.)
    // W5500 (external SPI MAC+PHY):
    int spiMiso;
    int spiMosi;
    int spiSck;
    int spiCs;
    int spiIrq;
};

// Per-chip default, seeding the runtime config so a board with no eth block in
// deviceModels.json still comes up on the historically-wired pins:
//  - P4 → Waveshare P4-NANO: IP101, addr 1, MDC/MDIO 31/52, reset 51, ext 50 MHz
//    clock IN on GPIO50 (Waveshare wiki + schematic + ESPHome page agree).
//  - classic ESP32 → the common LAN8720 RMII wiring: addr 0, reset 5, chip drives
//    RMII clock OUT on GPIO17, MDC/MDIO at IDF defaults (e.g. Olimex ESP32-Gateway).
//  - S3 → no built-in EMAC, so the default is W5500 SPI but with no pins set
//    (phyType ethW5500, pins -1): a W5500 S3 board MUST provide its SPI pins via
//    deviceModels.json — there's no universal S3 default to guess.
//  - S31 → Function-CoreBoard-1: RGMII YT8531, MDC/MDIO 5/6, reset 7. The RGMII
//    *data* pins (TX_CTL/TXD0-3, RX_CTL/RXD0-3, clocks) are board-fixed and live in
//    ethInitEmac()'s S31 branch, not this struct (same reason RMII data pins don't —
//    see above); rmiiClock* are unused for RGMII (clocks are set there too). See
//    docs/reference/esp32-s31-coreboard.md for the schematic pin map.
constexpr EthPinConfig ethConfigDefault =
    isEsp32P4   ? EthPinConfig{ /*phyType*/ ethIp101, /*addr*/ 1, /*mdc*/ 31, /*mdio*/ 52,
                                /*rst*/ 51, /*rmiiClk*/ 50, /*extIn*/ true,
                                /*miso*/ -1, /*mosi*/ -1, /*sck*/ -1, /*cs*/ -1, /*irq*/ -1 }
  : isEsp32S31  ? EthPinConfig{ /*phyType*/ ethYt8531, /*addr*/ -1, /*mdc*/ 5, /*mdio*/ 6,
                                /*rst*/ 7, /*rmiiClk*/ -1, /*extIn*/ false,
                                /*miso*/ -1, /*mosi*/ -1, /*sck*/ -1, /*cs*/ -1, /*irq*/ -1 }
  : isEsp32S3   ? EthPinConfig{ /*phyType*/ ethW5500, /*addr*/ 1, /*mdc*/ -1, /*mdio*/ -1,
                                /*rst*/ -1, /*rmiiClk*/ -1, /*extIn*/ false,
                                /*miso*/ -1, /*mosi*/ -1, /*sck*/ -1, /*cs*/ -1, /*irq*/ -1 }
              :   EthPinConfig{ /*phyType*/ ethLan8720, /*addr*/ 0, /*mdc*/ -1, /*mdio*/ -1,
                                /*rst*/ 5, /*rmiiClk*/ 17, /*extIn*/ false,
                                /*miso*/ -1, /*mosi*/ -1, /*sck*/ -1, /*cs*/ -1, /*irq*/ -1 };

// OTA (esp_https_ota) is available on every ESP32 build — the OTA partition
// layout in partitions/*.csv reserves app0/app1 unconditionally, and esp_https_ota
// is in baseline ESP-IDF. FirmwareUpdateModule + the /api/firmware/url route
// `if constexpr` on this so desktop builds get a 501-returning stub instead.
constexpr bool hasOta = true;

// Improv-serial is the device's serial RPC channel (UART0 + native USB-Serial-JTAG):
// the WiFi-provisioning RPCs (WIFI_SETTINGS, GET_WIFI_NETWORKS) AND the vendor RPCs
// (SET_DEVICE_MODEL, SET_TX_POWER, APPLY_OP — "Improv = REST over serial"). The
// transport is always available on ESP32, so the listener runs everywhere — including
// Ethernet-only builds (`--firmware esp32-eth*`), where the WiFi-only RPCs are compiled
// out (the `esp_wifi_*` calls aren't linked) but the vendor RPCs still work, so the web
// installer can push a device-model's config over serial to an eth device just as it
// does to a WiFi one. `hasImprov` is therefore true on every ESP32 target; only desktop
// (no serial peripheral) leaves it false.
constexpr bool hasImprov = true;

} // namespace mm::platform

// MM_MOONLIVE_HAS_HOST_JIT — always 0 on ESP32. The unit tests that consume this macro run on
// the *desktop* host binary; on-device MoonLive uses its own per-ISA backends (Xtensa / RISC-V
// in src/platform/esp32/moonlive_emit.cpp), validated by the live hardware run, not host tests.
// Kept in platform_config.h so the core header stays free of architecture #ifs.
#define MM_MOONLIVE_HAS_HOST_JIT 0

// MM_LINKS_ALL_LED_DRIVERS — 0 on ESP32: a board links only the drivers its silicon can run, so
// the type picker stays honest and the binary lean. See the desktop config for why the host is
// the other way round.
#define MM_LINKS_ALL_LED_DRIVERS 0
