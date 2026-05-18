# CLAUDE.md

## What This Is

A high-performance multi-platform system that drives large LED installations.
ESP32 (ESP-IDF, no Arduino) is the primary target. Also runs on macOS,
Windows, Linux, and Raspberry Pi. C++20. CMake.

## Principles

- **Minimalism.** Every addition must pay for itself. Prefer removing code
  over adding it. No speculative abstractions.
- **Data over objects.** The system is a render pipeline: generate pixels →
  map to positions → push to hardware. Design around data flow, not class
  hierarchies.
- **Let structure emerge.** Don't pre-design files, classes, or interfaces
  for things that don't exist yet. Build what you need, refactor when
  patterns become clear.

## Hard Rules

**Platform boundary.** All `#ifdef`, platform-specific `#include`s, and
hardware API calls live exclusively in `src/platform/`. Everything outside
`src/platform/` compiles on every target without modification.

**Hot path (render loop):**
- No heap allocations (`new`, `malloc`, `push_back`, `std::string` construction)
- No blocking (`delay`, `sleep`, `mutex.lock()` — use `try_lock` or lock-free)
- Integer math preferred over `float` in per-pixel work

**Memory strategy.**
- All buffers (pixel, mapping, staging) are allocated as single contiguous
  blocks outside the hot path (at startup or when configuration changes,
  e.g. LED count or fixture size). Never allocate small scattered objects
  in loops.
- On ESP32 with PSRAM: use `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` for
  large buffers. PSRAM (especially OPI) has sufficient bandwidth for
  sequential pixel streaming.
- On ESP32 without PSRAM: memory is tight. Adapt to what's available —
  fewer layers, no double buffering, fewer LEDs.
- Desktop/RPi: use `std::malloc`. No special handling needed.

**Network input.** Data arriving asynchronously (ArtNet UDP, WebSocket)
must be written into a staging buffer, never directly into the active
render buffer. The swap between staging and active happens as an atomic
pointer swap at the frame boundary — no locks on the hot path.

**Build errors.** If a build fails, stop. Diagnose the root cause. Do not
retry or work around it.

**Warnings are errors.** All targets build with `-Wall -Wextra -Werror`.

**Tests must pass.** Run `cmake --build build --target test` before
considering work complete. New core logic needs a corresponding test.

## Code Style

- `#pragma once`
- `constexpr` over `#define`
- `std::span` over pointer + length
- Namespace: `mm`, platform code in `mm::platform`
- No `using namespace` in headers

## CMake Strategy

The source tree is shared, but build entry points are separate:
- `CMakeLists.txt` at the root is a standard CMake project for desktop/RPi.
- `esp32/CMakeLists.txt` is a thin ESP-IDF project wrapper that pulls in
  the same `src/` sources via `idf_component_register()`.

This avoids polluting CMake files with `if(ESP_PLATFORM)` conditionals.
Both entry points build the same code — only the build tooling differs.

## Build

```bash
# Desktop (macOS/Linux/Windows)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# ESP32
source $IDF_PATH/export.sh
idf.py build
idf.py flash monitor

# Tests (desktop)
cmake --build build --target test
```

## Architecture

Defined in `docs/architecture.md`. That document wins when there is a
conflict with anything else.
