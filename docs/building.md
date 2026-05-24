# Building, running, flashing

How to get the system running on a desktop, an ESP32, a Teensy, or a Raspberry Pi. Design rationale for the choices below lives in [architecture.md](architecture.md); coding conventions in [coding-standards.md](coding-standards.md); what is tested in [testing.md](testing.md).

## MoonDeck — the dev console

Everything that builds, flashes, runs, tests, monitors, or checks the project — for every target — lives as a script under `scripts/`. The full per-script reference is [scripts/MoonDeck.md](../scripts/MoonDeck.md).

The scripts have two front ends with the same code and arguments:

- **CLI** — `uv run scripts/<group>/<name>.py`. What agents use; what CI uses. Composes with shell, captures exit codes, parses output.
- **MoonDeck** — `uv run scripts/moondeck.py`, then open `http://localhost:8420`. A browser dev console wrapping the same scripts: status dots, run/stop toggles for long-running processes, grouped tabs, output panes. The human control deck.

Use whichever fits. Neither path is "more official" than the other; the scripts are the source of truth and the front ends are interfaces. New work adds a script first; both interfaces follow.

MoonDeck has three tabs:

- **PC** — desktop build, run, test. Fast iteration.
- **ESP32** — chip type and USB port selection. Build, flash, monitor.
- **Live** — device discovery and monitoring against running devices on the network.

Script definitions and configuration live in `scripts/moondeck_config.json` (committed). Script documentation lives in `scripts/MoonDeck.md`, one section per script. Runtime state (selected devices, ports) persists in `scripts/moondeck.json` (gitignored).

## Tooling overview

CMake is the sole build system. The source tree is shared across every platform, but build entry points are separate because ESP-IDF wraps CMake with its own conventions (`idf_component_register()` instead of `add_library()`).

```text
CMakeLists.txt                          ← standard CMake: desktop / RPi + tests
src/
  main.cpp                              ← shared pipeline wiring (mm_main), platform-neutral
  platform/
    desktop/
      main_desktop.cpp                  ← desktop entry point: int main() + SIGINT
      platform_config.h                 ← desktop platform constants
    esp32/
      platform_config.h                 ← ESP32 platform constants (reads sdkconfig)
esp32/
  CMakeLists.txt                        ← ESP-IDF project root (thin wrapper)
  main/
    CMakeLists.txt                      ← idf_component_register() pointing at src/
    main.cpp                            ← ESP32 entry point: app_main() + Ethernet init
  sdkconfig.defaults                    ← board-specific defaults
```

The shared `src/main.cpp` defines `mm_main(keepRunning, gridW, gridH)` — the full pipeline wiring. Each platform provides a thin entry point that does platform-specific init (SIGINT on desktop, Ethernet on ESP32) then calls `mm_main()`.

The project is structured as a small set of CMake libraries: a core library (platform-independent), a platform library (selected at configure time), an application target (links both, provides the entry point). Further decomposition (effects, networking, drivers as separate libraries) happens when the codebase is large enough to justify it.

## Desktop / Raspberry Pi

Desktop and RPi both build with the root `CMakeLists.txt`. RPi can cross-compile against the same tree or build natively on the device — same source.

```sh
uv run scripts/build/build_desktop.py        # build
uv run scripts/run/run_desktop.py            # run as detached background process
uv run scripts/test/test_desktop.py          # unit tests
```

Or use MoonDeck's PC tab for the same operations with a status dot per card. The desktop run detaches and outlives the launching script — the same model as flashing an ESP32, where the device runs independently afterwards.

## ESP32

The ESP32 target uses ESP-IDF directly, not the Arduino framework.

**Tested IDF version:** **v6.1-dev-399-gd1b91b79b**. All builds and hardware tests use this exact commit. Minimum: ESP-IDF v5.1 (C++20 via GCC 12+); the project targets v6.x APIs (`esp_eth_phy_new_generic`, component manager for mDNS) so v5.x may need adjustments.

Run `setup_esp_idf.py` once to auto-detect the installed IDF version and create the required Python environment.

```sh
uv run scripts/build/setup_esp_idf.py        # one-time
uv run scripts/build/build_esp32.py          # default profile (WiFi + Ethernet)
uv run scripts/flash/flash_esp32.py          # flash + monitor
uv run scripts/monitor/monitor_esp32.py      # monitor only
```

### Build profiles

`build_esp32.py --profile` selects which sdkconfig fragments are layered:

- **`default`** — WiFi + Ethernet. Full Ethernet → WiFi STA → WiFi AP cascade. The standard binary.
- **`eth-only`** — WiFi compiled out. ESP-IDF v6.x has no `CONFIG_ESP_WIFI_ENABLED` switch (the symbol is forced on for WiFi-capable SoCs), so the profile drops the WiFi components via `-DEXCLUDE_COMPONENTS=esp_wifi;wpa_supplicant;esp_phy;esp_coex` and defines `MM_NO_WIFI` (passed as `-DMM_ETH_ONLY=1`, applied in `esp32/main/CMakeLists.txt`). No WiFi driver or `wpa_supplicant` in the binary, no WiFi FreeRTOS tasks. Smaller image, more free RAM, for Ethernet-only deployments. `build_esp32_ethonly.py` is a thin wrapper (`--profile eth-only` baked in) for the MoonDeck "Build (Ethernet-only)" button.

Switching profiles forces a clean reconfigure — `build_esp32.py` removes `build/` and `sdkconfig` when the profile changes (tracked via `build/.mm_profile`), so the CMake cache is reseeded correctly. Same-profile rebuilds stay incremental.

### Why not Arduino

The ESP32 target uses ESP-IDF directly for three reasons:

- **Direct hardware control.** RMT peripheral for LED protocols, FreeRTOS task pinning with explicit stack sizes, `heap_caps_malloc` with SPIRAM/8BIT caps, `esp_timer` microsecond timing. Arduino wraps these with abstractions that add overhead and hide control.
- **Native CMake.** ESP-IDF's build system *is* CMake (`idf.py` wraps it). No impedance mismatch. Arduino-on-ESP-IDF adds a compatibility layer that complicates the build.
- **Version stability.** ESP-IDF APIs are stable. Arduino-esp32 version churn caused recurring breakage in MoonLight.

Arduino can be added as an ESP-IDF component later if a specific Arduino library is needed; this is officially supported by Espressif and doesn't require restructuring.

### Third-party libraries

The platform abstraction layer replaces what libraries typically provide. Today no third-party libraries are pulled in:

| Library | Why not | What replaces it |
|---|---|---|
| [FastLED](https://github.com/FastLED/FastLED) | Arduino-dependent. LED protocol drivers (RMT, SPI) are available natively in ESP-IDF; FastLED's colour math is small enough to reimplement. | Own colour math in core. Own LED drivers per platform in `src/platform/`. |
| [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) | Arduino-dependent. Past memory-leak issues. Ties us to Arduino. | Own HTTP server via ESP-IDF's `esp_http_server` (ESP32) or BSD sockets (desktop). Reconsider if Arduino-as-component is added. |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | Works on ESP-IDF, but heavy: dynamic allocation, large footprint. | Own fixed-size control storage. JSON only for API serialisation, not internal state. |

When a library is genuinely needed (e.g. FastLED for specific hardware support), it lives inside `src/platform/` and is not referenced from core or light-domain code.

## Teensy

Teensy 4.x is in the supported target list. Buffers and pipeline configuration scale to 1 MB of internal RAM; OctoWS2811 gives excellent DMA-based LED output. Ethernet is built in on Teensy 4.1 and optional on 4.0.

Build flow is via the root `CMakeLists.txt` with a Teensy toolchain file. The platform layer for Teensy is added when the first hardware target is wired in.

## Pre-compilation steps

CMake runs these automatically before compilation when their source files change:

| Step | Source | Generated | Trigger |
|------|--------|-----------|---------|
| `version_gen` | `library.json` | `src/core/version.h` | `library.json` changes |
| `ui_embed` | `src/ui/index.html`, `app.js`, `style.css` | `src/ui/ui_embedded.h` | any UI file changes |

Both are defined in the root `CMakeLists.txt` (desktop) and `esp32/main/CMakeLists.txt` (ESP32). Generated files are gitignored — rebuilt on every clean build.

## After it's running

The system serves the web UI from its embedded HTTP server. Open `http://<device-ip>/` in a browser; on desktop that's typically `http://localhost:8080/`. From the UI you can change effects and modifiers, configure controls, see the 3D preview. The settings persist across reboot.

To run the test suite or any of the checks (platform boundary, specs, KPI), see the MoonDeck reference linked above. The release-readiness gates that wrap these into a checklist live in [CLAUDE.md § Lifecycle Events](../CLAUDE.md#lifecycle-events).
