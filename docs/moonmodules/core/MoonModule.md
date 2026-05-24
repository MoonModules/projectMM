# MoonModule

The base class for everything in the system. Effects, modifiers, layouts, drivers, and system services all inherit from MoonModule.

## Memory target

Aim for the smallest possible base class. Name stored in flash (progmem/constexpr), not in instance memory. Target: zero bytes of instance overhead beyond the vtable pointer and control variables. Every byte costs — on ESP32 without PSRAM, dozens of modules are loaded simultaneously.

Field order optimized for minimal padding: group 8-byte fields, then 4-byte, then 2-byte, then 1-byte. This avoids alignment waste.

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

## Per-module timing

Every MoonModule tracks `loopTimeUs()` — average microseconds per tick, computed over a 1-second window. The Scheduler times top-level modules; containers (Layer, Drivers) time their children. `publishTiming(frameCount)` recurses the tree every second to compute averages.

`tickTimeUs` is the primary performance metric. FPS is derived from it (`1000000 / tickTimeUs`). This gives per-module cost visibility at any depth in the tree.

## Enabled toggle

Every MoonModule has an `enabled` property (default: true). The UI shows a checkbox in the card header to toggle it. Settable via `POST /api/control` with `control=enabled`. Serialized as `"enabled": true/false` in the module JSON (module-level, not in the controls array).

**Semantics are owned by each module, not by the Scheduler.** The Scheduler always calls `loop()`, `loop20ms()`, and `loop1s()` regardless of `enabled`. Modules decide what "disabled" means:

- **Rendering modules** (Layer, Drivers, effects, modifiers): early-return from `loop()` when `enabled()` is false. The buffer keeps its last state; the user sees the layer/driver freeze. This is the typical UX intent of "turn this effect off."
- **System modules** (HttpServer, Network, Filesystem): typically ignore `enabled` and keep accepting connections / serving requests, since "disable HttpServer" via the UI would lock the user out.

**`onEnabled(bool newEnabled)`** is called once per transition by `setEnabled(b)` when the value actually flips. Override it to start/stop sockets, free buffers, switch driver pins to high-impedance, etc. Default is a no-op. Use this instead of polling `enabled()` in the hot path for one-shot transition work.

## Parent/child

Modules form a tree. Parent/child relationships only — no arbitrary DAG. Children run in order within their parent. Top-level modules also run in order. UI supports reordering, backed by the backend.

### Generic children in MoonModule base

Every MoonModule has a dynamic children array. `addChild()`, `removeChild()`, `replaceChildAt(i, fresh)`, and `moveChildTo(child, newIndex)` are implemented once in the base class — containers (Layer, Drivers, Layouts) do not override them. The array starts empty (zero allocation for leaf modules) and grows on demand during setup. This eliminates the per-container typed arrays (`effects_[]`, `drivers_[]`, `layouts_[]`) and typed add methods (`addEffect()`, `addDriver()`, `addLayout()`) that existed in earlier iterations.

`replaceChildAt` is used by [FilesystemModule](FilesystemModule.md) at load time to swap a child whose type differs from the persisted JSON. The caller owns the lifecycle of the returned old child (typically `teardown()` + `Scheduler::deleteTree`).

`moveChildTo(child, newIndex)` reorders a child to an absolute index 0..childCount-1. Intervening siblings shift to fill the vacated slot. Used by the UI's up/down/drag-and-drop reorder via `POST /api/modules/<name>/move {to:N}`. Returns false if `child` isn't found, `newIndex` is out of range, or the child is already at `newIndex`. After a successful move, the caller (currently `HttpServerModule::handleMoveModule`) triggers `Scheduler::rebuild()` so any LUT that depends on modifier/layout order rebuilds.

Children are distinguished by `role()` (Effect, Modifier, Driver, Layout, Generic). Containers that need role-specific iteration (e.g. Layer::loop() only calls loop() on Effects, not Modifiers) filter children by role at the call site.

Parents own their children's lifecycle. Only top-level modules are registered with the Scheduler — parents propagate `setup()`, `onBuildControls()`, `onAllocateMemory()`, `loop()`, and `teardown()` to their children. This means children don't need separate Scheduler registration.

### Lifecycle-aware add/remove

When the UI adds or removes a child at runtime (e.g. switching an effect on a layer, adding a driver), the caller must handle lifecycle:

- **Add at runtime:** caller calls `setup()` → `onBuildControls()` → `onAllocateMemory()` on the new child (since the parent's own setup has already run).
- **Remove at runtime:** caller calls `teardown()` on the child before removing it.
- **Add before setup:** if children are added before `scheduler.setup()` (startup or persistence restore), the parent's own `setup()` propagates to all children — no special handling needed.

This is needed for: effect switching, modifier add/remove, driver hot-plug, and persistence restore after reboot.

`onChildrenReady()` — parent notified when all children finish setup. Keep this minimal.

## Persistence

Module state (control values + `enabled` flag) is persisted to flash by [FilesystemModule](FilesystemModule.md). Modules themselves know nothing about persistence — they just bind variables via `addX(...)` calls in `onBuildControls()`. The Scheduler's phase 2 load hook overlays persisted values onto bound variables before any module's `setup()` runs.

Conditional controls (e.g. fields only visible under a Select mode) are always bound, with a `hidden` flag toggled via `controls_.setHidden(i, true/false)`. This lets the persistence layer load values regardless of the live conditional state, while the UI hides them. See [FilesystemModule.md](FilesystemModule.md) and [Control.md](Control.md).

`markDirty()` / `dirty()` / `clearDirty()` are set by HttpServerModule on every successful control mutation. FilesystemModule polls dirty flags in `loop1s()` and writes any subtree with a dirty descendant after a 2-second debounce.

## Tests

[Module test: MoonModule + Control](../../testing.md#moonmodule) — lifecycle, control binding, clear and rebuild.

## Prior art

### MoonLight — Node ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonBase/Nodes.h))

- Base ~29 bytes + vtable. Effects add only their control variables (uint8_t each).
- No std::string members (uses `Char<N>` fixed-size strings).
- `addControl()` binds to class variable by reference, stores `uintptr_t` pointer.
- `classSize()` reports actual instance size.

### projectMM v1 — StatefulModule ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/core/StatefulModule.h))

- Same addControl-by-reference pattern.

### projectMM v2 — MoonModule ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/core/MoonModule.h))

- `onBuildControls()` / `onAllocateMemory()` separation.
- `onChildrenReady()`.
- Field order optimized 8B→4B→2B→1B, saving 24 bytes.
- `classSize` set via `register_type<T>()`.
- `AutoWireSpec` — an arbitrary dependency-graph (DAG) wiring mechanism. v3 deliberately uses parent/child only; the DAG was more than the domain needs.
