#pragma once

// Desktop platform configuration — always uses larger types (PSRAM-like)

#include <cstdint>

// MM_RAMFUNC — RAM-resident code attribute for deadline-bound ISR paths (see the ESP32
// platform_config.h for the rationale). Desktop code always executes from RAM; empty.
#define MM_RAMFUNC

namespace mm::platform {

constexpr bool hasPsram = true;

// Not an ESP32-P4, so the P4-specific seams (Ethernet pin map, co-processor WiFi)
// compile out on desktop. Mirrors the esp32 config, which keeps the three chips that
// earn a flag: P4 and S3 for their Ethernet defaults, S31 for its RGMII PHY, and now
// Hub75Driver, which offers a per-chip generic pin set and must know which chip it is
// on to avoid listing a P4's free GPIOs to an S3.
constexpr bool isEsp32P4 = false;
constexpr bool isEsp32S3 = false;
constexpr bool isEsp32S31 = false;

// RMT channels the host reports. Non-zero for the same reason as the parallel lane counts: the
// desktop build emulates the peripheral so RmtLedDriver actually RUNS here, rather than guarding
// itself off and leaving its encode + channel-assignment logic testable only on hardware. 4 matches
// the S3 / P4 / S31 TX channel count (the classic ESP32 has 8), so the host exercises the tighter
// of the two real constraints.
constexpr uint8_t rmtTxChannels = 4;

// A host has no EMAC and no fixed pads: its Ethernet is a named interface, not wired signals. The
// count is 0 so the same NetworkModule code publishes nothing here; the array still holds one dummy
// element because a zero-size array is a GCC/Clang extension MSVC refuses, and this header is
// compiled by MSVC on the Windows CI job. See the ESP32 config for what this is for.
struct EthFixedPad { const char* name; uint8_t gpio; };
constexpr EthFixedPad ethFixedPads[] = {{"", 0}};
constexpr uint8_t ethFixedPadCount = 0;

// Lane counts the parallel backends report on desktop. NOT zero, deliberately: everything in the
// repo runs on the desktop build — the platform layer just has no hardware behind the call. A
// zero here makes every backend's lanesAvailable() report "not my silicon", so ParallelLedDriver
// idles and its ~2500-line body never executes off-device: not runnable, not unit-testable, and
// invisible to every AST-based check.
//
// 16 is the widest real rig (LightCrafter 16), so the host exercises the same lane-splitting and
// bus-rounding arithmetic the hardware does rather than a degenerate 1-lane path. The bus behind
// them is a heap buffer (platform_desktop.cpp § Parallel-WS2812 buses): the driver encodes real
// WS2812 bit patterns into real memory, and only the DMA hand-off is absent.
constexpr uint8_t lcdLanes = 16;

// hasLcdCam — TRUE on the host, like the lane counts above: the desktop build emulates the
// peripheral rather than declaring itself incapable. Saying false here would leave the pin-expander
// path (a real feature with real config validation) unreachable off-device, which is the same gap
// the zero lane counts used to create. There is no LCD_CAM silicon; there is a memory bus that
// behaves like one.
constexpr bool hasLcdCam = true;
constexpr uint8_t parlioLanes = 16;

// MultiPinLedDriver's lanesAvailable() reads lcdLanes + i2sLanes, so this stays 0 — otherwise the
// i80 backend would claim 32 lanes, which no real chip offers.
constexpr uint8_t i2sLanes = 0;

// No I2S microphone — AudioService guards on this and is inert on desktop. The
// No pin-wired I2S peripheral on a desktop host; live audio comes from OS capture
// devices instead (hasAudioCapture below), through the same audioMicRead seam.
constexpr bool hasI2sMic = false;

// OS audio capture (microphone / loopback devices) via the vendored miniaudio backend in
// platform_desktop_audio.cpp. The device is picked by AudioService's `device` control.
constexpr bool hasAudioCapture = true;

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
// Kept in step with the ESP32 list (platform/esp32/platform_config.h) so core code can name a
// PHY type without knowing which platform it compiles for. `ethYt8531`/`ethOpeneth` never occur
// on desktop; they exist here so the NAMES resolve everywhere.
enum EthPhyType { ethNone = 0, ethLan8720 = 1, ethIp101 = 2, ethW5500 = 3,
                  ethYt8531 = 4, ethOpeneth = 5 };
struct EthPinConfig {
    int phyType; int phyAddr;
    int mdcGpio; int mdioGpio; int rstGpio; int rmiiClockGpio; bool rmiiClockExtIn;
    int spiMiso; int spiMosi; int spiSck; int spiCs; int spiIrq;
};
// Desktop has no Ethernet — hasEthernet false, the default is ethNone. NetworkModule
// (shared code) `if constexpr (hasEthernet)`s the eth controls off, and seeds its
// members from this default; both must exist here for that shared code to compile.
constexpr bool hasEthernet = false;

// hasNamedNetInterfaces — the host has several NICs and a raw sender must name one ("eth0", "en0").
// True on desktop, false on a microcontroller with a single MAC, where the name would be a control
// that does nothing. Drivers use it to hide the field rather than to choose behaviour.
constexpr bool hasNamedNetInterfaces = true;

// hasNdi — the host can be an NDI video source. True on desktop, false on every ESP32: the NDI
// runtime is a desktop shared library with no microcontroller build, and the per-frame encode does
// not belong on one. Gates NdiDriver's registration, so the type picker only offers it where it can
// actually run. The runtime itself is loaded on demand and is NOT redistributed (see
// platform_desktop.cpp), so this flag says "the platform supports it", not "it is installed".
constexpr bool hasNdi = true;

// hasHls: the host can stream its rendered output as H.264/HLS by piping raw frames to the
// ffmpeg found on PATH (a runtime dependency of the user's, like the NDI runtime and Npcap).
// True on desktop and the Pi; the encoder process seam lives in the platform layer.
constexpr bool hasHls = true;
// hasEncoderChoice: ffmpeg offers several H.264 encoders (software and per-vendor hardware), so
// the pick is the user's. False where the platform has exactly one encoder, which hides the
// control rather than offering a choice of one.
constexpr bool hasEncoderChoice = true;
// hasFsSegments: ffmpeg writes the playlist and segments to disk, so the driver manages that
// directory and the HTTP server serves it as files. False where segments live in RAM (the P4).
constexpr bool hasFsSegments = true;
// Some-IP-stack flag (WiFi OR Ethernet) — mirrors the esp32 config so shared code
// (WLED audio sync, UDP interop) gates on "has network" uniformly. True on desktop
// via the WiFi stubs (UdpSocket has a desktop implementation).
constexpr bool hasNetwork = hasWiFi || hasEthernet;

// ethPhyIsFixed, true where the interface is a property of the PLATFORM rather than of the board,
// so a persisted or catalog-supplied PHY type must not override it. The ESP32 side is where that
// case is real (see its platform_config.h); desktop has no Ethernet to fix.
constexpr bool ethPhyIsFixed = false;

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
constexpr bool hasHeavyCompute = true;    // a desktop CPU has the headroom to spare

// Preprocessor mirror of the flag above: a whole effect can be compiled out only by
// `#if`, which `constexpr bool` cannot drive. Keep the two in step.
#define MM_HEAVY_COMPUTE 1
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

// MM_MOONLIVE_HAS_HOST_JIT — 1 when this desktop host has BOTH the emit blob AND the general
// assembler (moonlive_asm_host.cpp) available. Both arm64 and x86-64 are supported; the x86-64
// backend switches internally on _WIN32 between Microsoft x64 (Windows) and System V (Linux /
// Intel-macOS). Any other host ISA falls through here and MoonLive::compile / compileSource
// fail cleanly (scripted modules render dark). Tests and scenarios that presuppose a working
// JIT gate on this macro. Kept in platform_config.h (not core) per the platform-boundary rule:
// no `#if defined(__aarch64__)` outside src/platform/. A #define (not constexpr) so #include-
// side test files can use it in `#if` — CLAUDE.md's `if constexpr` preference is for runtime
// branches inside code, not preprocessor gating around whole TEST_CASEs.
// MM_MOONLIVE_FORCE_NO_HOST_JIT makes a JIT-capable machine build as a backend-less one
// (build_desktop.py --no-jit) — useful for testing the dark-render path on the same box that
// normally has a live backend.
#if (defined(__aarch64__) || defined(__x86_64__) || defined(_M_X64)) && !defined(MM_MOONLIVE_FORCE_NO_HOST_JIT)
    #define MM_MOONLIVE_HAS_HOST_JIT 1
#else
    #define MM_MOONLIVE_HAS_HOST_JIT 0
#endif

// MM_LINKS_ALL_LED_DRIVERS — 1 where the build links every LED driver regardless of silicon.
// The desktop host does: the repo's rule is that everything runs there, with the platform layer
// simply having no hardware behind the call. A driver excluded from the host binary cannot be
// unit-tested, cannot be seen by any AST-based check, and only ever runs where it is hardest to
// debug. A #define (not constexpr) because it gates `#include`s in main.cpp, which `if constexpr`
// cannot do — and it lives here, not in core, per the platform-boundary rule.
#define MM_LINKS_ALL_LED_DRIVERS 1
