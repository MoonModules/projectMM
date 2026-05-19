# Scheduler

Domain-neutral. Manages MoonModule lifecycles and timing.

## Interface

- `add(MoonModule*)` — register a module (max 32)
- `setup()` — calls `addControls()` then `setup()` on all modules
- `loop()` — calls `loop()` on all modules
- `teardown()` — calls `teardown()` on all modules
- `elapsed()` — milliseconds since setup (used by effects for animation)

## What worked

- Simple, domain-neutral. Doesn't know about lights.

## What needs improvement

- Needs multi-core support (proven in MoonLight and v2). Per-module core affinity.
- Needs different loop rates (loop20ms, loop1s) — not all modules need 60fps updates.
- Effects should use elapsed time (millis) for animation, not frame count (architecture decision).
- Dependency tracking between modules (init-order, rebuild propagation) should be in the scheduler, not in main.cpp.

## Prior art

### MoonLight — effectTask / svelteTask
- Two FreeRTOS tasks: effects on core 1, system/drivers on core 0.
- Per-node: `loop()` every frame, `loop20ms()` for slow updates.

### projectMM v2 — Scheduler ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/core/Scheduler.h))
- Multi-core: runs N `core_loop()` threads (default 2 on ESP32). Each module declares `coreAffinity()` (0 or 1). Uses `pal::task_create_pinned` with 8KB stacks.
- Module ticking: setup, loop, loop20ms, loop1s, loop10s, teardown — dispatched per core.
- `stop()` — atomic flag for clean shutdown.

### projectMM v1 — Scheduler ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/core/Scheduler.h))
Time-sliced dispatch: setup, loop, loop20ms, loop1s for all modules. Single-threaded.
