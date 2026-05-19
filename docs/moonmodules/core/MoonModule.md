# MoonModule

The base class for everything in the system. Effects, modifiers, layouts, drivers, and system services all inherit from MoonModule.

## Memory target

Aim for the smallest possible base class. Name stored in flash (progmem/constexpr), not in instance memory. Target: zero bytes of instance overhead beyond the vtable pointer and control variables. Every byte costs — on ESP32 without PSRAM, dozens of modules are loaded simultaneously.

Field order optimized for minimal padding: group 8-byte fields, then 4-byte, then 2-byte, then 1-byte. This avoids alignment waste (v2 saved 24 bytes vs naive order).

## Lifecycle

- `setup()` — called once after construction
- `loop()` — hot path for effects/drivers. Called every iteration. The Scheduler handles pacing and yielding to other tasks.
- `loop20ms()` — called every ~20ms (for UI updates, control reads, network polling)
- `loop1s()` — called every ~1 second (diagnostics, reconnects, housekeeping)
- `teardown()` — called before destruction, must clean up all allocated resources
- `onAllocateMemory()` — single hook for dynamic allocation after setup. Sets `moduleAllocBytes_`. Called at setup and on reallocation triggers.
- `onBuildControls()` — all addControl() calls here, not in setup(). Supports dynamic rebuild via `clearControls()`.

The Scheduler handles pacing — `loop()` modules don't need to manage timing themselves. `loop20ms` for UI/network. `loop1s` for cold-path housekeeping.

## Controls

See [Control.md](Control.md) for full control specification.

Controls bind to class variables by reference. Hot-path code reads the variable directly — zero overhead. Slider controls default to `uint8_t` (0-255) range where possible, aligning with DMX standard.

`onBuildControls()` prepares the control set. Supports dynamic controls: when a control value changes (e.g. a mode selector), `onBuildControls()` can be re-called to show/hide mode-specific controls.

## Footprint reporting

- `classSize()` — set once at registration via `register_type<T>()`. No per-class boilerplate.
- `dynamicMemorySize()` — heap bytes allocated by this module (set by `onAllocateMemory()`).
- `usPerLoop()` — microseconds per loop iteration, for performance monitoring.

## Parent/child

Modules form a tree. Parent/child relationships only (no arbitrary DAG like v2's AutoWireSpec — simpler, sufficient). Children run in order within their parent. Top-level modules also run in order. UI supports reordering, backed by the backend.

Parents own their children's lifecycle. Only top-level modules are registered with the Scheduler — parents propagate `setup()`, `onBuildControls()`, `onAllocateMemory()`, `loop()`, and `teardown()` to their children. This means children don't need separate Scheduler registration.

### Lifecycle-aware add/remove

When the UI adds or removes a child at runtime (e.g. switching an effect on a layer, adding a driver), the parent's add/remove methods must handle lifecycle:

- **Add at runtime:** parent calls `setup()` → `onBuildControls()` → `onAllocateMemory()` on the new child (since the parent's own setup has already run).
- **Remove at runtime:** parent calls `teardown()` on the child before removing it.
- **Add before setup:** if children are added before `scheduler.setup()` (startup or persistence restore), the parent's own `setup()` propagates to all children — no special handling needed.

This is needed for: effect switching, modifier add/remove, driver hot-plug, and persistence restore after reboot.

`onChildrenReady()` — parent notified when all children finish setup. Keep this minimal.

## Persistence

Module state (control values) persisted to filesystem. Load on setup, save on change. Format and mechanism to be specified — keep simple (one file per module, or one file for all).

## Deferred

- `markDirty()` / `dirty()` / `clearDirty()` — postpone until rebuild propagation is needed in practice.
- No color picker control (RGB) — not needed for v3 initial scope.
- No AutoWireSpec — parent/child is sufficient.
- No `controlAllocBytes()` pre-check — defer until needed.

## Tests

[Module test: MoonModule + Control](../../testing.md#moonmodule) — lifecycle, control binding, clear and rebuild.

## Prior art

### MoonLight — Node ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonBase/Nodes.h))
- Base ~29 bytes + vtable. Effects add only their control variables (uint8_t each).
- No std::string members (uses `Char<N>` fixed-size strings).
- `addControl()` binds to class variable by reference, stores `uintptr_t` pointer.
- `classSize()` reports actual instance size.

### projectMM v1 — StatefulModule ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/core/StatefulModule.h))
- Same addControl-by-reference pattern.

### projectMM v2 — MoonModule ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/core/MoonModule.h))
- `onBuildControls()` / `onAllocateMemory()` separation.
- `onChildrenReady()`.
- Field order optimized 8B→4B→2B→1B, saving 24 bytes.
- `classSize` set via `register_type<T>()`.
