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

**Build errors.** If a build fails, stop. Diagnose the root cause. Do not
retry or work around it.

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
source $IDF_PATH/export.sh
idf.py build
idf.py flash monitor

# Tests (desktop)
cmake --build build --target test
```

## Architecture

Defined in `docs/architecture.md`. That document wins when there is a
conflict with anything else.
