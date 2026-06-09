# Scheduler

Domain-neutral. Manages MoonModule lifecycles and dispatch.

## Loop rates

- `loop()` — hot path for effects and drivers. Called every iteration. The Scheduler handles pacing (yielding to other tasks between iterations via `taskYIELD()` on ESP32, optional sleep on desktop).
- `loop20ms()` — every ~20ms. UI updates, control reads, network polling.
- `loop1s()` — every ~1 second. Diagnostics, reconnects, housekeeping.

Not all modules need `loop()`. System modules (HTTP, WiFi) use `loop20ms()` or `loop1s()` only.

## Timing

Effects use a synchronized clock for animation (millis from platform). This is frame-rate independent — same visual speed at 30fps and 60fps. For multi-device sync, the leader synchronizes this clock across devices. No frame counter needed.

## Core affinity

Each top-level module declares a core affinity (0 or 1 on ESP32). The scheduler pins the module's task to that core. Child modules inherit the parent's core. On single-core or desktop systems, core affinity is ignored.

## Module ordering

Child modules run in their declared order within the parent. Top-level modules also run in declared order. The UI supports reordering, backed by the scheduler. Only parent/child relationships — no arbitrary dependency graph.

## Prior art

### MoonLight — effectTask / svelteTask

- Two FreeRTOS tasks: effects on core 1, system/drivers on core 0.
- Per-node: `loop()` every frame, `loop20ms()` for slow updates.

### projectMM v1 — Scheduler ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/core/Scheduler.h))

- Time-sliced dispatch: setup, loop, loop20ms, loop1s for all modules. Single-threaded.

### projectMM v2 — Scheduler ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/core/Scheduler.h))

- Multi-core: runs N `core_loop()` threads (default 2 on ESP32). Each module declares `coreAffinity()`. Uses `pal::task_create_pinned` with 8KB stacks.
- Module ticking: setup, loop, loop20ms, loop1s, loop10s, teardown — dispatched per core.

## Source

[Scheduler.cpp](../../../src/core/Scheduler.cpp) · [Scheduler.h](../../../src/core/Scheduler.h)
