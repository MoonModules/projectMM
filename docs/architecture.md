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

Each MoonModule is documented in `docs/modules/` as it is built.

## Controls

Every MoonModule exposes **controls** — runtime-configurable parameters visible in the web UI. Examples:
- A grid layout exposes width, height, depth.
- An ArtNet driver exposes destination IP and universe.
- A fire effect exposes speed, cooling, sparking.

Controls are linked to MoonModule class variables. The default value of the variable is the default value of the control when the module is created. Hot-path code reads these variables directly — no function call needed to get the latest value. When a control value changes, the system notifies the owning MoonModule for cold-path reactions (e.g. a layout size change triggers a LUT rebuild).

Controls are shown dynamically: when a control value changes, the control set can be rebuilt. For example, if a control selects a mode, the controls belonging to that mode are shown while others are hidden.

Controls are the bridge between the UI and the engine. The web UI renders them automatically based on what MoonModules declare. The exact control types (slider, toggle, color picker, text input, dropdown) are defined in the UI spec (`docs/modules_draft/core/ui-spec.md`). The principle is: MoonModules declare what they need, the UI renders it.

## Rebuild Propagation

When a control value changes on a layout, the pipeline must rebuild: layers rebuild their LUTs, the DriverGroup reallocates its output buffer. When a modifier control changes, only the affected layer's LUT is rebuilt (the output buffer size doesn't change). This propagation must be built into the framework — not handled by ad-hoc dirty flag checks in the application entry point.

The mechanism (observer pattern, signal/slot, or centralized pipeline manager) will be defined in the module spec before implementation.

## Platform Abstraction

Only abstract what you actually need. Currently that means:

- **Time.** Microsecond-resolution monotonic clock. (`esp_timer` / `std::chrono`)
- **Memory.** Allocator that prefers PSRAM on ESP32, falls back to regular heap. (`heap_caps_malloc` / `std::malloc`)
- **Threads.** Create a thread pinned to a specific core (ESP32 has 2), with mutex/semaphore primitives. (`FreeRTOS` / `std::thread`)
- **LED drivers.** Per-protocol, per-platform. RMT on ESP32, DMA/OctoWS2811 on Teensy, SPI on RPi, SDL2 or terminal on desktop.
- **Networking.** HTTP server, WebSocket, UDP sockets. (`esp_http_server` / BSD sockets / platform library). Teensy: USB serial or Ethernet shield.
- **Filesystem.** Read/write config and UI assets. (`LittleFS` / `std::filesystem`). Teensy: SD card or flash.

Abstractions are added when needed by a concrete implementation, not pre-designed. All platform-specific code lives in `src/platform/`. Everything outside it compiles cleanly on every target.

## Build System

CMake is the sole build system. The source tree is shared across all platforms, but build entry points are separate because ESP-IDF wraps CMake with its own conventions (`idf_component_register()` instead of `add_library()`).

```
CMakeLists.txt              ← standard CMake: desktop/RPi build + tests
esp32/
  CMakeLists.txt            ← ESP-IDF project root (thin wrapper)
  main/
    CMakeLists.txt          ← idf_component_register() pointing at src/
```

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

### Unit tests (desktop)

Core logic runs on desktop, so all non-hardware code is testable via `ctest`. Priority areas:
- Color math (HSV→RGB, blending, scale8)
- Buffer operations (allocate, fill, clear, bounds)
- Mapping LUT (1:0, 1:1, 1:N, rebuild on config change)
- Blend+map pass (correct physical output from logical layers)

Test framework will be chosen when we write the first test. Preference for something header-only and lightweight (doctest, Catch2, or plain `assert`).

### Performance tests (desktop)

Automated checks that verify architectural rules at runtime:
- **Zero-allocation render loop.** Run N frames, intercept `malloc`/`free` (via overriding or platform allocator hooks), fail if any allocation occurs during steady-state rendering.
- **Frame time regression.** Measure render time for a known workload (e.g. 10K lights, 3 layers, rainbow effect). Fail if it exceeds a threshold. Not a hard real-time guarantee, but catches gross regressions.

### Live system tests (on-device)

For ESP32 targets, test what desktop can't:
- LED output produces correct signal (protocol-level, verified with logic analyzer or known-good reference).
- Multi-device sync achieves sub-millisecond accuracy.
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

- **MoonModule interface.** Draft exists in `docs/modules_draft/core/`. Needs review: virtual modifier interface, rebuild propagation.
- **Config persistence.** Controls need to be saved/loaded. The storage format and mechanism will be specified when we build that module.
- **Web UI.** Draft spec exists in `docs/modules_draft/core/ui-spec.md`. Needs review: multiple layers, live preview, WebSocket.
- **File/directory structure.** The platform boundary is fixed. The rest will grow as modules are specified and implemented.
