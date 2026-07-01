# FastLED — monthly activity digest

What landed on [FastLED](https://github.com/FastLED/FastLED)'s main branch, month by month. External-context reference (like the v1/v2/MoonLight inventories) — a factual log of a friend repo's releases, not projectMM's own history or roadmap. Newest month on top. The reusable prompt that generates these digests lives in [README.md](README.md).

## June 2026 (up to 3.10.4)

Released **3.10.4** (2026-06-16), cut from `master`.

**New**
- STM32: Arduino UNO Q board support.
- New NXP LPC8xx family drivers land (LPC804 PLU, LPC845 bit-bang + PWM/DMA-to-GPIO, LPC11xx) — early bring-up, bench-validated.
- Unified `fl::Watchdog` API with real hardware implementations across platforms (ESP32, Teensy 4, AVR, Apollo3, RP2040, STM32) plus a non-allocating reset/crash-classification helper.
- Wave simulation: opt-in 9-point isotropic Laplacian (smoother 2D waves), exposed as a UI toggle in several example sketches.
- ScreenMap gains a v2 schema (auto-detected), and a new `.fled` container format for video/screenmaps that `FxSdCard` can load.
- FFT now auto-detects ESP-DSP on ESP32 (no opt-in macro needed).

**Fixed**
- RGBW colorimetric path reworked: native LED gamut + D65 as the default source, improved strict/boosted solvers, corrected dual- and 3-channel solving.
- ESP32: reliable streaming for SPI strips over ~680 LEDs; ESP32-C6 routes Serial to HWCDC with non-blocking writes and rejects USB-serial pins for LED output.
- Teensy Audio selection fixed on low-memory boards; nRF52 now honors configured SPI data rates.
- LuminescentGrand example: corrected serpentine column wiring/orientation.

## June 2026 (post-3.10.4)

**New**
- Teensy 4.x LED driver bring-up: ObjectFLED and FlexIO parallel output engines plus a new LPUART-based WS2812 driver (inverted-TX + eDMA), and FlexPWM-based RX capture.
- New WS2812-style RX capture path on classic ESP32 (RMT4) and LPC845 (SCT+DMA).
- ARM Cortex-M DSP-extension SIMD backend wired up for Teensy 3.x/4.x (faster scale/blend on those chips).

**Fixed**
- SM16824E chipset timing corrected to match the datasheet.
- RGBW gamut configuration is now kept per strip.
- Fixed a memory leak in the chunked `fl::deque` container.

Note: the bulk of June's ~481 commits were internal (LPC bring-up scaffolding, the AutoResearch hardware-test harness, a Python→Rust C++ linter migration, RPC/JSON size-and-speed tuning, and test-file splits) and are not user-visible. Notable issue traffic was dominated by the Teensy driver and LPC845 bring-up trackers; a user-reported "multi strip problem" (#3340) was triaged and closed, and a deque refactor briefly broke a macOS-arm64 audio unit test (#3286, fixed same month).

_Auditability: 481 commits with author-date in 2026-06-01..2026-06-30 on `master` (first-parent/merged view); split at the 3.10.4 release (published 2026-06-16, an ancestor of `master`). Issues reviewed via `search/issues` for `created:2026-06-01..2026-06-30` and `closed:2026-06-01..2026-06-30` (~50 each); user-facing ones folded in above, the remainder were internal bring-up/CI/linter trackers._

## May 2026

*Summarised from 150 first-parent commits on `master`, 2026-05-01 … 2026-05-31.*

**New**

- New **Channels API** for managing multiple LED drivers at once — a `fl::Bus` type, `FastLED.add<Bus>(...)`, `fl::enableAllDrivers()`, and `FastLED.setExclusiveDriver(...)`, with a diagnostic that warns when a strip's driver doesn't match its bus. (The month's biggest effort.)
- **RGBW / RGBWW colour**: proper colorimetric RGB→RGBW conversion with a lookup table, colour-temperature (CCT) control, and an RGB+CCT mode.
- ESP32-P4 gains a SIMD (PIE) acceleration backend for faster pixel processing.

**Faster**

- Big speedups to the ESP32-P4 **PARLIO** parallel driver — encoding and transmission now overlap, and a chipset-aware encode path is ~5× faster than before.
- ESP32-P4 "Wave8" output ~1.2–2× faster via new transpose and lookup-table paths.

**Hardware & build**

- Builds cleanly on **ESP-IDF 6**, and can now be used as a standalone ESP-IDF component.
- ESP32 OTA support fixed (adds the required update/mDNS dependencies).
- Fixes for several boards: nRF52 Xiao BLE Sense and other nRF52 variants, ESP8266 / STM32 / ESP32-C3 / Teensy 4 / UNO R4 WiFi build issues, and ESP32-S3 LCD-clockless ISR safety.

**Fixed**

- RGBW driver no longer kept a dangling pointer to its colour profile (could crash or corrupt output).
- AVR boards no longer try to compile RGBWW examples that overflow their memory.

## April 2026

*Summarised from 237 first-parent commits on `master`, 2026-04-01 … 2026-04-30.*

**New**

- **Audio "silence gate"** across the audio-reactive features — tempo, spectral metrics, and the Vibe effect now fade out cleanly when the input goes quiet instead of reacting to noise.
- Audio FFT can run on the ESP-DSP hardware backend (faster spectrum analysis on ESP32).
- ESP32-S3 LCD driver gains ISR-driven chunked DMA output (smoother large-strip output); coroutine tasks can be pinned to a chosen core.
- RMT receive (reading signals in) gains DMA streaming; a long-strip SPI bug (#2254) fixed.

**Fixed**

- ESP32-S3 LCD-clockless GPIO crash; SPI bus ownership/buffer-reuse issues when switching drivers.
- Board fixes: Teensy 4.1 pins 40–54, Arduino Due (sam3x8e), ESP8266 register-name clash, Digispark ATtiny85.

## March 2026

*Summarised from 444 first-parent commits on `master`, 2026-03-01 … 2026-03-31.*

- Mostly an internal stability and build-correctness month (sanitizer fixes, WASM build speed, IWYU/PCH hygiene) — little user-facing.
- **Fixed:** AVR builds (replaced defaulted `noexcept` with explicit implementations in container types); ESP32-C3/C5 and Teensy build breakages; printf/Arduino-compatibility shim; an audio-path bug.

## February 2026

*Summarised from 516 first-parent commits on `master`, 2026-02-01 … 2026-02-28.*

- **Fixed (broad platform-stability month):** AVR math bugs (left-shifts wider than AVR's 16-bit int; `PROGMEM` LUT reads), ESP32-C6 dual-mode async/sync SPI, ESP32 WROOM + mbedTLS, ESP8266, Teensy LC, UART compiler issues, RGBW mode, and the UCS chipset preamble.
- HTTP-server / loopback networking pieces stabilised; OTA validation fixed.

## January 2026

*Summarised from 582 first-parent commits on `master`, 2026-01-01 … 2026-01-31.*

- A heavy **stability-hardening month** — most of the work was fixing memory and initialization bugs surfaced by sanitizers (ASan/LSan/UBSan): use-after-free, memory leaks, static-initialization-order issues, shared-pointer errors.
- **Fixed (user-visible):** a crash in power management; RP2350 system defines not being included; an ISR error where an int was read as a bool; i2s/LCD-CAM.

## December 2025

*Summarised from 392 first-parent commits on `master`, 2025-12-01 … 2025-12-31.*

**New**

- **PARLIO driver maturation** (ESP32 parallel output): streaming support, up to 16 lanes, larger per-channel LED counts, background DMA buffer worker, and a low-level hardware abstraction layer — plus many alignment/timing fixes (the "1-bit shift" buffer-boundary bug).
- **Validation / proof-of-life framework**: hardware-in-the-loop validation, an ESP32 watchdog (with a USB-disconnect fix), and a result banner.
- New `Potentiometer` class (hysteresis + calibration); 16-bit PWM pin support (`setPwm16`); per-channel gain on the HD108 chipset; `Serial` gains `printf`.
- Signal **receive (RX)** gains raw edge-time capture and a safe sketch-halt.

## November 2025

*Summarised from 528 first-parent commits on `master`, 2025-11-01 … 2025-11-30.*

**New**

- **Channels / ChannelBusManager foundation** — a unified, priority-based driver manager with fallback, centralized SPI driver registration, and a new ChannelEngine-based SPI driver + RMT4 driver for ESP32 IDF 4.x. (Start of the multi-driver architecture that continues through to May.)
- **PARLIO** gains a runtime-configurable multi-channel driver with auto-select (and dropped ESP32-S3, which uses LCD instead).
- New **UCS7604** controller; a generic clockless waveform generator; video playback support.
- **Audio-reactive** effects expand — downbeat-darkness effect, AnimartrixRing audio reactivity, configurable UI audio.
- WASM web preview moves to dedicated worker threads (drops Asyncify) with incremental/PCH build speedups.
- Experimental RISC-V interrupt support.

## October 2025

*Summarised from 1,185 first-parent commits on `master`, 2025-10-01 … 2025-10-31.*

**New**

- **RP2040 automatic parallel output** using the standard FastLED API.
- Per-platform ESP32 clockless controllers + configurable ESP32/ESP8266 timing; nanosecond timing support for ARM K66/KL26; SPI chipset controllers split into their own headers.
- Fallback **OTA** implementation for ESP-IDF < 4.0.
- PARLIO strategic buffer-breaking at colour boundaries.
- 8-bit math optimised for ATtiny; math template/float overloads; a beat-detection `AudioProcessor` facade.
- New "advanced effects" and "LED cookbook" documentation chapters.

## September 2025 (post-3.10.3)

*Summarised from 162 first-parent commits on `master`, 2025-09-21 … 2025-09-30 (after the 3.10.3 release on the 20th).*

**New**

- **WASM web-preview overhaul** — Three.js-based tile rendering, instanced LED rendering, SharedArrayBuffer zero-copy frames, a background-worker async controller, and an improved video recorder (native `captureStream`, 60 FPS, better MP4 compatibility).
- New hardware-accelerated **ezWS2812** GPIO + SPI drivers for Silicon Labs MGM240 / EFR32MG24 (MG24) boards.
- **Codec support** — progressive JPEG decoding (4 ms time budget), and metadata parsing for GIF/JPEG/MPEG1.
- Bilinear interpolation for upscaling effects; `FxNoiseRing` low-memory mode.
- README/wiring guidance for high-parallel LED setups (incl. ObjectFLED parallel capacity).

