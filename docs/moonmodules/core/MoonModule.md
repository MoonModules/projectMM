# MoonModule

The base class for everything in the system. Effects, modifiers, layouts, drivers, and system services all inherit from MoonModule.

## Memory target

Aim for the smallest possible base class. Name stored in flash (progmem/constexpr), not in instance memory. Target: zero bytes of instance overhead beyond the vtable pointer and control variables. Every byte costs — on ESP32 without PSRAM, dozens of modules are loaded simultaneously.

Field order optimized for minimal padding: group 8-byte fields, then 4-byte, then 2-byte, then 1-byte. This avoids alignment waste.

## Lifecycle

`setup()` / `teardown()` bracket the module's life; `loop()` / `loop20ms()` / `loop1s()` are the three tick rates (the [Scheduler](Scheduler.md) owns pacing). Two build hooks separate from `setup()`: `onBuildControls()` holds all `addX()` calls and can be re-run to rebuild the set (e.g. when a Select changes mode), and `onBuildState()` is the single dynamic-allocation hook (sets the module's heap-byte report), called at setup and on any reallocation trigger. Controls bind by reference, so see [Control.md](Control.md) for the rest.

## Footprint reporting

- `classSize()` — set once at registration via `register_type<T>()`. No per-class boilerplate.
- `dynamicMemorySize()` — heap bytes allocated by this module (set by `onBuildState()`).

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

`moveChildTo(child, newIndex)` reorders a child to an absolute index 0..childCount-1. Intervening siblings shift to fill the vacated slot. Used by the UI's up/down/drag-and-drop reorder via `POST /api/modules/<name>/move {to:N}`. Returns false if `child` isn't found, `newIndex` is out of range, or the child is already at `newIndex`. After a successful move, the caller (currently `HttpServerModule::handleMoveModule`) triggers `Scheduler::buildState()` so any LUT that depends on modifier/layout order rebuilds.

Children are distinguished by `role()` (Effect, Modifier, Driver, Layout, Generic). Containers that need role-specific iteration (e.g. Layer::loop() only calls loop() on Effects, not Modifiers) filter children by role at the call site.

Two virtuals govern UI tree-mutation, keeping that policy on the device rather than hardcoded in the web UI (see [architecture.md § Web UI](../../architecture.md#web-ui)): `acceptsChildRoles()` — comma-separated roles this module accepts as user-added children (`""` default; a container like Layer returns `"effect,modifier"`), surfaced per-type in `/api/types`, drives the UI's `+ add child` affordance and picker filter. `userEditable()` — whether the user may delete/replace this module (`true` default; a load-bearing child like PreviewDriver returns `false`), surfaced per-instance in `/api/state` (emitted only when false). The `+ add child` policy lives on the parent; the deletable/replaceable policy lives on the child.

Parents own their children's lifecycle. Only top-level modules are registered with the Scheduler — parents propagate `setup()`, `onBuildControls()`, `onBuildState()`, `loop()`, `loop20ms()`, `loop1s()`, and `teardown()` to their children. This means children don't need separate Scheduler registration.

### Lifecycle-aware add/remove

When the UI adds or removes a child at runtime (e.g. switching an effect on a layer, adding a driver), the caller must handle lifecycle:

- **Add at runtime:** caller calls `setup()` → `onBuildControls()` → `onBuildState()` on the new child (since the parent's own setup has already run).
- **Remove at runtime:** caller calls `teardown()` on the child before removing it.
- **Add before setup:** if children are added before `scheduler.setup()` (startup or persistence restore), the parent's own `setup()` propagates to all children — no special handling needed.

This is needed for: effect switching, modifier add/remove, driver hot-plug, and persistence restore after reboot.

## Status slot

`setStatus(msg, severity)` / `status()` / `severity()` / `clearStatus()` carry a short message the module wants the user to see right now — Layer writes "buffer reduced — not enough memory" on memory degradation, NetworkModule writes "Eth: 192.168.1.210" or "No network". Severity is `Status` / `Warning` / `Error` and the UI picks the chip emoji (ℹ️ / ⚠️ / ❌). The slot stores a pointer (no copy), so callers pass flash string literals or a module-owned char buffer with stable lifetime. Wire contract (only emitted when set): `/api/state` and `/api/system` each carry `"status":"…","severity":"status|warning|error"` — see [HttpServerModule.md](HttpServerModule.md).

## Persistence

Module state (control values + `enabled` flag) is persisted to flash by [FilesystemModule](FilesystemModule.md). Modules themselves know nothing about persistence — they just bind variables via `addX(...)` calls in `onBuildControls()`. The Scheduler's phase 2 load hook overlays persisted values onto bound variables before any module's `setup()` runs.

Conditional controls (e.g. fields only visible under a Select mode) are always bound, with a `hidden` flag toggled via `controls_.setHidden(i, true/false)`. This lets the persistence layer load values regardless of the live conditional state, while the UI hides them. See [FilesystemModule.md](FilesystemModule.md) and [Control.md](Control.md).

`markDirty()` / `dirty()` / `clearDirty()` are set by HttpServerModule on every successful control mutation. FilesystemModule polls dirty flags in `loop1s()` and writes any subtree with a dirty descendant after a 2-second debounce.

## Tests

[Unit tests: MoonModule](../../tests/unit-tests.md#moonmodule) — lifecycle, control binding, clear and rebuild.

## Prior art

### MoonLight — Node ([source](https://github.com/ewowi/MoonLight/blob/main/src/MoonBase/Nodes.h))

- Base ~29 bytes + vtable. Effects add only their control variables (uint8_t each).
- No std::string members (uses `Char<N>` fixed-size strings).
- `addControl()` binds to class variable by reference, stores `uintptr_t` pointer.
- `classSize()` reports actual instance size.

### projectMM v1 — StatefulModule ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/core/StatefulModule.h))

- Same addControl-by-reference pattern.

### projectMM v2 — MoonModule ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/core/MoonModule.h))

- `onBuildControls()` / `onBuildState()` separation.
- `onChildrenReady()` — parent-notified-after-children hook. Not carried over; child setup ordering is handled by Scheduler's 4-phase boot instead.
- Field order optimized 8B→4B→2B→1B, saving 24 bytes.
- `classSize` set via `register_type<T>()`.
- `AutoWireSpec` — an arbitrary dependency-graph (DAG) wiring mechanism. projectMM deliberately uses parent/child only; the DAG was more than the domain needs.

## Source

[MoonModule.h](../../../src/core/MoonModule.h)
