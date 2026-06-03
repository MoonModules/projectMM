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

![MoonDeck PC tab](assets/screenshots/moondeck_pc.png)

Each host writes into its own build dir: `build/macos/`, `build/linux/`, `build/windows/`. The per-host layout mirrors the ESP32 side's `build/esp32-<board>/` shape — one directory per target, no cross-target clobbering on a multi-host dev machine.

## ESP32

The ESP32 target uses ESP-IDF directly, not the Arduino framework.

**Tested IDF version:** **v6.0.0** (`v6.1-dev-399-gd1b91b79b` internally). All builds and hardware tests use this tag. Minimum: ESP-IDF v5.1 (C++20 via GCC 12+); the project targets v6.x APIs (`esp_eth_phy_new_generic`, component manager for mDNS) so v5.x may need adjustments.

### Prerequisites

You need [uv](https://docs.astral.sh/uv/) (Python launcher), CMake 3.20+, and a C++20 compiler. Clone ESP-IDF into `~/esp/esp-idf` (the path the build scripts expect):

```sh
git clone --depth 1 --branch v6.0.0 https://github.com/espressif/esp-idf.git ~/esp/esp-idf
```

Then run the one-time Python environment setup — either open MoonDeck (`uv run scripts/moondeck.py`), go to the ESP32 tab, and click **Setup ESP-IDF**, or run it directly:

```sh
uv run scripts/build/setup_esp_idf.py                                 # one-time
uv run scripts/build/build_esp32.py --firmware esp32                  # WiFi-only
uv run scripts/build/flash_esp32.py --firmware esp32 --port /dev/tty.usbserial-XXXX
uv run scripts/run/monitor_esp32.py --port /dev/tty.usbserial-XXXX
```

The ESP32 tab in MoonDeck wraps the same steps as cards (Setup → Firmware → Build → Port → Flash → Run). The Network bar at the top is the same one shown on the Live tab — it remembers which serial port and WiFi credentials belong to the current LAN, so moving the laptop between networks doesn't require re-picking.

![MoonDeck ESP32 tab](assets/screenshots/moondeck_esp32.png)

### Firmware variants

`build_esp32.py --firmware` selects one of four shipping variants. The key combines chip name + feature flags + (for SKU-sensitive chips) module. ("Firmware" here is the compiled binary; the physical board is a separate concept — see [architecture.md § Firmware vs board](architecture.md#firmware-vs-board).)

| `--firmware` | IDF target | `SDKCONFIG_DEFAULTS` | What's in the image |
|---|---|---|---|
| `esp32` | `esp32` | `sdkconfig.defaults` | WiFi only. No RMII pins reserved. |
| `esp32-eth` | `esp32` | `sdkconfig.defaults;sdkconfig.defaults.eth` | Ethernet only. WiFi components dropped via `-DEXCLUDE_COMPONENTS=esp_wifi;wpa_supplicant;esp_coex` and `-DMM_ETH_ONLY=1`. Smaller image, more free RAM. Olimex ESP32-Gateway pins baked in (LAN8720 @ MDIO 0, PHY RST GPIO 5). |
| `esp32-eth-wifi` | `esp32` | `sdkconfig.defaults;sdkconfig.defaults.eth` | Ethernet + WiFi. Same Olimex pin map. Full Ethernet → WiFi STA → WiFi AP cascade. |
| `esp32s3-n16r8` | `esp32s3` | `sdkconfig.defaults;sdkconfig.defaults.esp32s3-n16r8` | ESP32-S3 DevKitC-1 with the N16R8 module (16 MB flash, 8 MB octal PSRAM). WiFi only. |

ESP-IDF v6.x has no `CONFIG_ESP_WIFI_ENABLED` switch (the symbol is forced on for WiFi-capable SoCs), so dropping WiFi at compile time happens via `EXCLUDE_COMPONENTS` plus `MM_NO_WIFI` (set when `MM_ETH_ONLY=1`, applied in `esp32/main/CMakeLists.txt`). The `esp32-eth` variant takes this path; `esp32-eth-wifi` keeps everything compiled in and uses the runtime cascade in `NetworkModule`.

Each firmware has its own build dir at `build/esp32-<firmware>/`, so all four variants can coexist on disk. `build_esp32.py` points `idf.py -B` at the per-firmware dir; switching firmwares is just a different `--firmware` argument, no clean rebuild penalty. Same-firmware rebuilds stay incremental, as before. Disk usage scales with the number of firmwares built (≈100 MB each), and a future rename would orphan the old dir — clean with `scripts/build/clean_esp32.py --firmware <name>` or `--all`.

Each ESP32-S3 SKU has its own firmware key because the sdkconfig fragment encodes flash size, partition table, and PSRAM mode — flashing an `n16r8` binary onto a different module (e.g. N8R2) either misaligns the partition table (boot loop) or fails PSRAM init. New SKUs become new keys (e.g. `esp32s3-n8r8`); there is no generic `esp32s3` shortcut.

The Ethernet pin map is baked into the build, verified on the [Olimex ESP32-Gateway](https://www.olimex.com/Products/IoT/ESP32/ESP32-GATEWAY/open-source-hardware). Boards with the same LAN8720 PHY but different pinouts (e.g. WT32-ETH01 with reset on GPIO 16) require a local rebuild with adjusted pin defaults.

`--profile` is accepted one release for migration: `--profile default` → `--firmware esp32`, `--profile eth-only` → `--firmware esp32-eth`. The legacy `build_esp32_ethonly.py` wrapper still works (it now forwards `--firmware esp32-eth`).

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
| `build_info_gen` | `library.json` | `src/core/build_info.h` | `library.json` changes |
| `ui_embed` | `src/ui/index.html`, `app.js`, `style.css` | `src/ui/ui_embedded.h` | any UI file changes |

Both are defined in the root `CMakeLists.txt` (desktop) and `esp32/main/CMakeLists.txt` (ESP32). Generated files are gitignored — rebuilt on every clean build.

## After it's running

The system serves the web UI from its embedded HTTP server. Open `http://<device-ip>/` in a browser; on desktop that's typically `http://localhost:8080/`. From the UI you can change effects and modifiers, configure controls, see the 3D preview. The settings persist across reboot.

To run the test suite or any of the checks (platform boundary, specs, KPI), see the MoonDeck reference linked above. The release-readiness gates that wrap these into a checklist live in [CLAUDE.md § Lifecycle Events](../CLAUDE.md#lifecycle-events).
