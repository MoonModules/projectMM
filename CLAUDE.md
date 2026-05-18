# CLAUDE.md

## What This Is

A high-performance multi-platform system that drives large LED installations.
ESP32 (ESP-IDF, no Arduino) is the primary target. Also runs on macOS,
Windows, Linux, and Raspberry Pi. C++20. CMake.

See `docs/architecture.md` for system design. This file contains only
rules and constraints for working on the project.

## Principles

- **Minimalism.** Every addition must pay for itself. Prefer removing code
  over adding it. No speculative abstractions.
- **Data over objects.** Design around data flow, not class hierarchies.
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

**Memory.** Allocate buffers as single contiguous blocks outside the hot
path. Never allocate small scattered objects in loops. On ESP32 with PSRAM,
use `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` for large buffers.

**Network input.** Process synchronously at a defined point in the frame
loop. No async network tasks writing into render buffers. Minimize all
network-related buffer overhead.

**Build errors.** Stop. Diagnose root cause. Do not retry or work around.

**Warnings are errors.** Build with `-Wall -Wextra -Werror`.

**Tests must pass.** Run `cmake --build build --target test` before
considering work complete. New core logic needs a corresponding test.

## Code Style

- `#pragma once`
- `constexpr` over `#define`
- `std::span` over pointer + length
- Namespace: `mm`, platform code in `mm::platform`
- No `using namespace` in headers

## Build

```bash
# Desktop (macOS/Linux/Windows)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# ESP32
cd esp32 && idf.py build
idf.py flash monitor

# Tests (desktop)
cmake --build build --target test
```
