# Core Architecture

## The Problem

Drive 10,000+ addressable LEDs and DMX lighting fixtures (RGB(W) pars, moving heads, dimmers) across multiple synchronized devices at high frame rates. Support LED protocols (WS2812, APA102) and DMX/ArtNet/DDP for conventional lighting fixtures. Run the same core logic on ESP32, desktop, and Raspberry Pi and Teensy. Provide a web UI and network APIs for control.

## Core vs Domain

The system has two layers:
- **Core** — MoonModule base, controls, scheduling, platform abstraction, system services (HTTP, WiFi, filesystem). Domain-neutral.
- **Light domain** — light values, layers, mapping, blending, effects, layouts, modifiers, LED drivers, ArtNet, DDP. Built on top of the core.

These are separated as much as practical. When mixing is needed (for performance or simplicity), it must be an explicit decision — consciously choosing minimalism over separation, not accidentally blurring the boundary. Use domain-neutral naming in those cases ("producer buffer" not "LED buffer", "output driver" not "LED driver" in core interfaces) to keep the door open for future separation.

The light domain architecture is in `docs/architecture-light.md`.

## MoonModules

The core building block is a **MoonModule**. Everything is a MoonModule — not just effects, modifiers, layouts, and drivers, but also system services: HTTP server, WebSocket server, file server, WiFi, mDNS, OTA updates. The core itself is minimal: MoonModule base, buffer management, and a scheduler. Everything else is loaded as a MoonModule.

This means:
- All MoonModules share the same class structure, lifecycle (setup, loop, teardown), and controls. Learn the pattern once, apply it everywhere.
- System services get controls for free — HTTP port, WiFi SSID, mDNS hostname are all configurable through the same UI as effect parameters.
- Capabilities are modular — don't need WiFi? Don't load the WiFi MoonModule. No `#ifdef`s needed.
- System MoonModules that listen (HTTP, WebSocket) poll in their `loop()` — the standard pattern for embedded servers.
- The scheduler handles init-order dependencies between system MoonModules (e.g. WiFi before HTTP, HTTP before WebSocket).

MoonModules should have a minimal memory footprint. The base class and controls use fixed-size storage, no heap per instance. On constrained devices, many modules may be loaded simultaneously.

Modules can be added, changed, replaced, or removed dynamically at runtime. When removed (teardown), all allocated resources are cleaned up.

Each MoonModule is documented in `docs/moonmodules/` as it is built.

## Controls

Every MoonModule exposes **controls** — runtime-configurable parameters visible in the web UI. Examples:
- A grid layout exposes width, height, depth.
- An ArtNet driver exposes destination IP and universe.
- A fire effect exposes speed, cooling, sparking.

Controls are linked to MoonModule class variables. The default value of the variable is the default value of the control when the module is created. Hot-path code reads these variables directly — no function call needed to get the latest value. When a control value changes, the system notifies the owning MoonModule for cold-path reactions (e.g. a layout size change triggers a LUT rebuild).

Controls are shown dynamically: when a control value changes, the control set can be rebuilt. For example, if a control selects a mode, the controls belonging to that mode are shown while others are hidden.

Prefer `uint8_t` (0-255) for slider controls where possible. This minimizes memory per control, aligns with DMX channel values (0-255), and keeps the control range manageable in the UI.

Controls are the bridge between the UI and the engine. The web UI renders them automatically based on what MoonModules declare. The exact control types (slider, toggle, color picker, text input, dropdown) are defined in the UI spec (`docs/moonmodules_draft/core/ui-spec.md`). The principle is: MoonModules declare what they need, the UI renders it.

## Rebuild Propagation

When a control value changes on a layout, the pipeline must rebuild: layers rebuild their LUTs, the DriverGroup reallocates its output buffer. When a modifier control changes, only the affected layer's LUT is rebuilt (the output buffer size doesn't change). This propagation must be built into the framework — not handled by ad-hoc dirty flag checks in the application entry point.

The mechanism (observer pattern, signal/slot, or centralized pipeline manager) will be defined in the module spec before implementation.

## Parallelism

On multi-core systems (ESP32 has 2 cores, desktop/RPi have many), the system exploits parallelism by assigning MoonModules to specific cores. Each MoonModule can declare a core affinity. The scheduler respects this when pinning tasks. On single-core or desktop systems, core affinity is ignored and everything runs on available threads.

The general model is **producers vs consumers**: producers generate data, consumers process and output it. They run on separate cores with double buffering at the boundary — no locks on the hot path. The domain-specific application of this model (which MoonModules are producers, which are consumers, what is double-buffered) is defined in `architecture-light.md`.

## Platform Abstraction

Only abstract what you actually need. Currently implemented:

- **Time.** `millis()`, `micros()` — microsecond-resolution monotonic clock. (`esp_timer` / `std::chrono`)
- **Memory.** `alloc(size)`, `free(ptr)` — allocator that prefers PSRAM on ESP32, falls back to regular heap. `freeHeap()`, `maxAllocBlock()` for diagnostics. (`heap_caps_malloc` / `std::malloc`)
- **Networking.** `UdpSocket` — UDP send for ArtNet. (`lwip/sockets.h` / BSD sockets)
- **Scheduling.** `yield()` — cooperative yield to OS/RTOS. (`vTaskDelay` / no-op on desktop)
- **Platform config.** `platform_config.h` per platform — compile-time constants like `hasPsram`. Each platform provides its own version; `types.h` includes it without `#ifdef`.

Abstractions are added when needed by a concrete implementation, not pre-designed. All platform-specific code lives in `src/platform/`. Everything outside it compiles cleanly on every target.

## ESP-IDF, No Arduino

The ESP32 target uses ESP-IDF directly, not the Arduino framework. Rationale:
- **Direct hardware control.** RMT peripheral for LED protocols, FreeRTOS task pinning with explicit stack sizes, `heap_caps_malloc` with SPIRAM/8BIT caps, `esp_timer` microsecond timing. Arduino wraps these with abstractions that add overhead and hide control.
- **Native CMake.** ESP-IDF's build system IS CMake (`idf.py` wraps it). No impedance mismatch. Arduino-on-ESP-IDF adds a compatibility layer that complicates the build.
- **Version stability.** ESP-IDF APIs are stable. Arduino-esp32 version churn caused recurring breakage in MoonLight.

Arduino can be added as an ESP-IDF component later if a specific Arduino library is needed. This is officially supported by Espressif and doesn't require restructuring.

### ESP-IDF version

Minimum: ESP-IDF v5.1 (C++20 support via GCC 12+). Recommended: latest stable (v5.4 as of writing). The project also builds on v6.1-dev but that is pre-release — some APIs changed (e.g. `esp_eth_phy_new_lan87xx` → `esp_eth_phy_new_generic`, Ethernet kconfig symbol names). If using a dev version, expect occasional API churn.

MoonDeck's ESP-IDF setup script (`scripts/build/setup_esp_idf.py`) auto-detects the installed version and creates the required Python environment. Run it once after installing or updating ESP-IDF.

### Library strategy

Start without third-party libraries. The platform abstraction layer replaces what libraries typically provide:

| Previously used | Why not now | v3 approach |
|----------------|-------------|-------------|
| [**FastLED**](https://github.com/FastLED/FastLED) | Arduino-dependent. LED protocol drivers (RMT, SPI) are available natively in ESP-IDF. FastLED's color math can be reimplemented in pure C++. | Own color math in core. Own LED drivers per platform in `src/platform/`. |
| [**ESPAsyncWebServer**](https://github.com/ESP32Async/ESPAsyncWebServer) | Arduino-dependent. Previously had memory leak issues, now under active development and reportedly improved. Still ties us to Arduino framework. | Own HTTP server via ESP-IDF's `esp_http_server` (ESP32) or BSD sockets (desktop). Can reconsider if Arduino-as-component is added. |
| [**ArduinoJson**](https://github.com/bblanchon/ArduinoJson) | Not Arduino-dependent (works with ESP-IDF), but heavy: dynamic allocation, large footprint. | Own fixed-size control storage. JSON only for API serialization, not internal state. |

If a library is genuinely needed later (e.g. FastLED for specific hardware support), it can be added as an ESP-IDF component. The rule: the library lives inside `src/platform/` and is not referenced from core or light domain code.

## Build System

CMake is the sole build system. The source tree is shared across all platforms, but build entry points are separate because ESP-IDF wraps CMake with its own conventions (`idf_component_register()` instead of `add_library()`).

```
CMakeLists.txt                          ← standard CMake: desktop/RPi build + tests
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

- **Desktop/RPi:** `cmake -B build && cmake --build build` from the root.
- **ESP32:** `cd esp32 && idf.py build` — the wrapper pulls in `src/` from the parent directory.
- **Raspberry Pi:** cross-compile using the root CMakeLists.txt, or build natively on the device.

The project is structured as a set of CMake libraries:
- A core library (platform-independent logic)
- A platform library (selected at configure time)
- An application target (links both, provides the entry point)

Further decomposition (effects, networking, drivers as separate libraries) will happen when the codebase is large enough to justify it.

## Developer Tooling

**MoonDeck** (`scripts/moondeck.py`) is a Python web server providing a browser-based console for all project scripts. Run with `uv run scripts/moondeck.py`, open `http://localhost:8420`.

Three tabs:
- **PC** — desktop build, run, test. Fast iteration.
- **ESP32** — chip type and USB port selection. Build, flash, monitor.
- **Live** — device discovery and monitoring. Select devices for operations.

Each script is a small card with a `?` help link and a Run button. Output streams to a shared log window. Scripts are individual Python files in `scripts/` — always runnable standalone via `uv run scripts/<name>.py` as well as from MoonDeck.

Script definitions and configuration live in `scripts/moondeck_config.json` (committed). Script documentation lives in `scripts/MoonDeck.md`, one section per script. Runtime state (selected devices, ports) persists in `scripts/moondeck.json` (gitignored).

## Testing

Three test categories, each with a clear purpose. Full inventory of what is tested: [docs/testing.md](testing.md).

### Module tests (desktop, `test/test_*.cpp`)

Test individual MoonModules in isolation. Each module has its own test file. Run via doctest (`ctest` or `./build/test/mm_tests -s`). These verify that a module's API, edge cases, and output are correct — independent of how the module is wired into a pipeline.

Module specs in `docs/moonmodules/` link to their test sections in `docs/testing.md` so end users can see what is tested for each module.

Core priority areas:
- MoonModule lifecycle (setup, loop, teardown ordering)
- Control operations (add, set, bind by reference)
- Scheduler (module dispatch, timing)

Light domain areas are in `architecture-light.md`.

### Scenario tests (desktop, `test/scenarios/*.json`)

Test the system as an integrated pipeline. Scenarios are declarative JSON files — each defines a sequence of steps (`add_module`, `set_control`) with optional performance bounds. The scenario runner (`test/scenario_runner.cpp`) replays steps in-process and checks output and timing.

Currently in-process only. When the HTTP API is added, the same JSON files will work with a Python runner against a live system (same approach as projectMM v1's `deploy/scenario.py`).

### Regression tests

When a bug is found, the fix includes a new test (module test or scenario) that reproduces the bug. This ensures the bug stays fixed. The test references the bug in a comment so the connection is traceable.

### Performance tests (desktop)

Automated checks that verify architectural rules at runtime:
- **Zero-allocation render loop.** Run N frames, intercept `malloc`/`free` (via overriding or platform allocator hooks), fail if any allocation occurs during steady-state rendering.
- **Frame time bounds.** Scenario tests include `"bounds": {"fps": {"min": N}}` to catch performance regressions.

### Live system tests (on-device)

For ESP32 targets, test what desktop can't:
- Memory stays within bounds over long runs (no leaks, no fragmentation drift).

These are manual or semi-automated for now. Automation strategy will emerge with the hardware.

## Linting and Static Analysis

### Compiler warnings

All targets build with `-Wall -Wextra -Werror`. No warnings allowed in CI.

### Platform boundary enforcement

An automated check (script or CI step) scans all files outside `src/platform/` for:
- `#ifdef` / `#if defined` with platform macros
- `#include` of platform-specific headers (`esp_*`, `freertos/*`, `driver/*`, `SDL.h`, `wiringPi.h`, etc.)

Fails the build if any are found.

### Hot path lint

A custom check (clang-tidy plugin, script, or code review rule) flags allocation calls (`new`, `malloc`, `make_unique`, `make_shared`, `push_back`, `std::string` constructors) inside functions identified as hot path (render loop and its callees). This can start as a code review convention and become automated as the codebase grows.

### Code formatting

clang-format with a project `.clang-format` file. Applied in CI — code that doesn't match the format fails the check. Developers can run `clang-format` locally or via editor integration.

### When checks run

- **Pre-commit (optional):** clang-format, platform boundary check.
- **CI (mandatory):** all of the above plus full test suite.
- Exact CI setup will be configured when we set up the repository's CI pipeline.

## Web UI

The UI is three hand-maintained files: `index.html`, `app.js`, `style.css`. No frameworks, no build tools, no npm. These files are served directly by the embedded HTTP server.

The UI is **MoonModule-driven**. It does not contain hard-coded knowledge of specific effects, layouts, or drivers. Instead, it queries the system for the current MoonModule tree (layers, their effects, modifiers, layouts, drivers — each with their controls) and renders them generically:

- Each MoonModule is shown with its name and its declared controls.
- Controls are auto-rendered based on type (slider, toggle, color picker, text input, dropdown).
- MoonModules can be switched (e.g. change which effect a layer uses) and linked together (e.g. assign a layout to a layer).

Adding a new MoonModule with controls requires **zero changes** to the UI files. The UI discovers and renders whatever MoonModules the system reports.

## What We Leave Undesigned

These will be specified in module docs before implementation:

- **MoonModule interface.** Draft exists in `docs/moonmodules_draft/core/`. Needs review: virtual modifier interface, rebuild propagation.
- **Config persistence.** Controls need to be saved/loaded. The storage format and mechanism will be specified when we build that module.
- **Web UI.** Draft spec exists in `docs/moonmodules_draft/core/ui-spec.md`. Needs review: multiple layers, live preview, WebSocket.
- **File/directory structure.** The platform boundary is fixed. The rest will grow as modules are specified and implemented.
