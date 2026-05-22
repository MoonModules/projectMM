# Core Architecture

## The Problem

Build a modular runtime for resource-constrained embedded devices that the same source compiles for, unmodified, on ESP32, Teensy, desktop, and Raspberry Pi. The runtime must:

- Compose behaviour from small, uniform units (modules) that can be created, configured, reordered, and removed at runtime — including from a network API.
- Expose every module's parameters generically so a single web UI renders any module with zero per-module UI code.
- Run a hot loop with predictable timing and zero steady-state heap allocation on devices with as little as ~320 KB of RAM.
- Persist configuration across reboots, exploit multiple CPU cores where present, and keep all platform-specific code behind one boundary.

This document describes that domain-neutral core. It carries no knowledge of what the modules actually do — that is the *domain* layered on top.

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

MoonModules should have a minimal memory footprint. On constrained devices, many modules may be loaded simultaneously.

### ModuleFactory

A static registry mapping type names (strings) to create functions. Used by the HTTP API to create modules by name at runtime (`POST /api/modules {"type":"NoiseEffect"}`). Registration happens once at startup via a template that also captures `sizeof(T)` for memory reporting:

```cpp
ModuleFactory::registerType<NoiseEffect>("NoiseEffect");
```

The factory is only for dynamic creation (HTTP CRUD). The main pipeline in `main.cpp` constructs modules directly. ModuleFactory is not a MoonModule — it's core infrastructure in `src/core/ModuleFactory.h`.

### Dynamic over fixed-size

Prefer dynamic (grow-on-demand) over fixed-size arrays for structural data like children, module lists, and control sets. Fixed-size arrays impose arbitrary limits, waste memory on instances that don't use the full capacity, and cost memory on instances that need none (e.g. leaf modules with zero children).

Dynamic arrays allocate from the heap during setup. The hot path only iterates these arrays — same pointer arithmetic as a fixed array, no performance difference.

Exception: contiguous data buffers (LED output, LUT tables) are allocated as a single block in `onAllocateMemory()`. These are sized by the layout, not by arbitrary limits, and are already dynamic.

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

Controls are the bridge between the UI and the engine. The web UI renders them automatically based on what MoonModules declare. The exact control types (slider, toggle, color picker, text input, dropdown) are defined in the UI spec (`docs/moonmodules/core/ui.md`). The principle is: MoonModules declare what they need, the UI renders it.

## Persistence

Control values + each module's `enabled` flag are persisted to flash so settings survive a reboot. The mechanism lives in [FilesystemModule](moonmodules/core/FilesystemModule.md):

- **Storage**: one flat JSON file per top-level module under `/.config/<TypeName>.json`. Children are encoded positionally with `<index>.` key prefixes — no nested objects, no arrays. The parser stays minimal (three flat-JSON helpers in `core/JsonUtil.h`).
- **Lifecycle**: `Scheduler::setup()` runs four phases — (1) `onBuildControls` binds every module's full control set, (2) the FilesystemModule load hook overlays persisted values onto the bound variables, (2b) `rebuildControls` re-evaluates conditional `hidden` flags against the loaded state, (3) each module's own `setup()` runs with persisted values already in member variables, (4) `onAllocateMemory` sizes buffers. Modules themselves know nothing about persistence — they just bind their variables.
- **Save trigger**: HttpServerModule marks the target module dirty on every successful control mutation. FilesystemModule debounces 2s in `loop1s()`, walks the tree, and writes any subtree containing a dirty descendant via atomic write-and-rename.
- **Conditional controls**: every conditional control is always bound; the module sets a `hidden` flag (`controls_.setHidden(i, …)`) to tell the UI not to render it. This means the load path can find persisted values regardless of the live conditional state.

The Scheduler stays independent of FilesystemModule's type via a function-pointer hook (`setLoadAllHook`), so there's no circular include and persistence is opt-in: if no FilesystemModule is registered, the load phase is a no-op and the system runs with member-initialized defaults.

## Parallelism

On multi-core systems (ESP32 has 2 cores, desktop/RPi have many), the system exploits parallelism by assigning MoonModules to specific cores. Each MoonModule can declare a core affinity. The scheduler respects this when pinning tasks. On single-core or desktop systems, core affinity is ignored and everything runs on available threads.

The general model is **producers vs consumers**: producers generate data, consumers process and output it. They run on separate cores with double buffering at the boundary — no locks on the hot path. The domain-specific application of this model (which MoonModules are producers, which are consumers, what is double-buffered) is defined in `architecture-light.md`.

## Platform Abstraction

Only abstract what you actually need. Currently implemented:

- **Time.** `millis()`, `micros()` — microsecond-resolution monotonic clock. (`esp_timer` / `std::chrono`)
- **Memory.** `alloc(size)`, `free(ptr)` — allocator that prefers PSRAM on ESP32, falls back to regular heap. `freeHeap()`, `maxAllocBlock()` for diagnostics. (`heap_caps_malloc` / `std::malloc`)
- **Networking.** `UdpSocket` — UDP send for ArtNet. `TcpConnection`/`TcpServer` — HTTP + WebSocket; `TcpConnection::writeChunks` is a non-blocking scatter-gather write so a backpressured browser can't stall the render loop. (`lwip/sockets.h` / BSD sockets)
- **Scheduling.** `yield()` — cooperative yield to OS/RTOS. `delayMs(ms)` — blocking sleep, outside the hot path only. `reboot()` — restart the device. (`vTaskDelay` / `esp_restart` on ESP32; `std::this_thread::sleep_for` / `std::exit` on desktop)
- **Platform config.** `platform_config.h` per platform — compile-time constants like `hasPsram` and `hasWiFi`. Each platform provides its own version; `types.h` includes it without `#ifdef`. Core code branches on these via `if constexpr` (e.g. NetworkModule drops its WiFi cascade when `hasWiFi` is false), so the dead branch is removed from the binary with no `#ifdef` outside `src/platform/`.

Abstractions are added when needed by a concrete implementation, not pre-designed. All platform-specific code lives in `src/platform/`. Everything outside it compiles cleanly on every target.

## ESP-IDF, No Arduino

The ESP32 target uses ESP-IDF directly, not the Arduino framework. Rationale:
- **Direct hardware control.** RMT peripheral for LED protocols, FreeRTOS task pinning with explicit stack sizes, `heap_caps_malloc` with SPIRAM/8BIT caps, `esp_timer` microsecond timing. Arduino wraps these with abstractions that add overhead and hide control.
- **Native CMake.** ESP-IDF's build system IS CMake (`idf.py` wraps it). No impedance mismatch. Arduino-on-ESP-IDF adds a compatibility layer that complicates the build.
- **Version stability.** ESP-IDF APIs are stable. Arduino-esp32 version churn caused recurring breakage in MoonLight.

Arduino can be added as an ESP-IDF component later if a specific Arduino library is needed. This is officially supported by Espressif and doesn't require restructuring.

### ESP-IDF version

Currently tested on: **ESP-IDF v6.1-dev-399-gd1b91b79b**. This is the reference version — all builds and hardware tests use this exact commit.

Minimum: ESP-IDF v5.1 (C++20 support via GCC 12+). The project targets v6.x APIs (e.g. `esp_eth_phy_new_generic`, component manager for mDNS). Building on v5.x may require API adjustments.

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
- **ESP32:** `cd esp32 && idf.py build` — the wrapper pulls in `src/` from the parent directory. Prefer `python scripts/build/build_esp32.py`, which also handles build profiles.
- **Raspberry Pi:** cross-compile using the root CMakeLists.txt, or build natively on the device.

### ESP32 build profiles

`build_esp32.py` takes a `--profile` argument selecting which sdkconfig fragments are layered:

- `--profile default` (default) — WiFi + Ethernet. The full Ethernet → WiFi STA → WiFi AP cascade.
- `--profile eth-only` — WiFi compiled out entirely. ESP-IDF v6.x has no `CONFIG_ESP_WIFI_ENABLED` switch (the symbol is non-settable, forced on for WiFi-capable SoCs), so the eth-only profile drops the WiFi components via `-DEXCLUDE_COMPONENTS=esp_wifi;wpa_supplicant;esp_phy;esp_coex` and defines `MM_NO_WIFI` (passed as `-DMM_ETH_ONLY=1`, applied in `esp32/main/CMakeLists.txt`). No WiFi driver/`wpa_supplicant` in the binary, no WiFi FreeRTOS tasks. Smaller image, more free RAM, for Ethernet-only deployments. `build_esp32_ethonly.py` is a thin wrapper (`--profile eth-only` baked in) used by the MoonDeck "Build (Ethernet-only)" button.

Switching profiles forces a clean reconfigure — `build_esp32.py` removes `build/` + `sdkconfig` when the profile changes (tracked via a `build/.mm_profile` marker), so the CMake cache (`EXCLUDE_COMPONENTS`, `MM_ETH_ONLY`) is reseeded correctly. Same-profile rebuilds stay incremental.

The project is structured as a set of CMake libraries:
- A core library (platform-independent logic)
- A platform library (selected at configure time)
- An application target (links both, provides the entry point)

Further decomposition (effects, networking, drivers as separate libraries) will happen when the codebase is large enough to justify it.

### Pre-compilation steps

CMake runs these automatically before compilation when their source files change:

| Step | Source | Generated | Trigger |
|------|--------|-----------|---------|
| `version_gen` | `library.json` | `src/core/version.h` | library.json changes |
| `ui_embed` | `src/ui/index.html`, `app.js`, `style.css` | `src/ui/ui_embedded.h` | any UI file changes |

Both are defined in the root `CMakeLists.txt` (desktop) and `esp32/main/CMakeLists.txt` (ESP32). Generated files are gitignored — they're rebuilt on every clean build.

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

### Type casting

Use project typedefs (`lengthType`, `nrOfLightsType`) consistently so types match and casts are unnecessary. When casts are needed:

- **`static_cast`** — converts a value between related types. Checked at compile time. Use only at system boundaries: byte protocol packing, OS API return values, overflow-prevention with wider intermediates. If you need `static_cast` between project types, the types should be made to match instead.
- **`reinterpret_cast`** — reinterprets raw memory as a different type. No conversion, no safety. Avoid. The only legitimate use is raw byte/memory access (e.g. `reinterpret_cast<const sockaddr*>` for socket APIs).
- **`dynamic_cast`** — runtime-checked cast from base to derived class. Requires RTTI which is disabled on ESP32 (`-fno-rtti`). Not used in this project.

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
- **Web UI.** Spec at `docs/moonmodules/core/ui.md` describes the current implementation (status bar, card layout, 9 control types, type picker, theme, WS lifecycle, 3D preview). Open design questions: multi-layer UI, modifier chain visualization, presets, canvas/node-graph view.
- **File/directory structure.** The platform boundary is fixed. The rest will grow as modules are specified and implemented.
