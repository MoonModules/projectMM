# MoonModule

The base class for everything in the system. Effects, modifiers, layouts, drivers, and system services all inherit from MoonModule.

## Interface

- `name()` — returns the display name
- `setup()` / `loop()` / `teardown()` — lifecycle, called by Scheduler
- `addControls()` — declare controls at init time
- `onChange(index)` — called when a control value changes

## Controls

Fixed-capacity array (MAX_CONTROLS = 16) of named, typed values:
- `Uint16` — slider with min/max
- `Bool` — toggle
- `Text` — text input (char[64], for IPs, names, etc.)

Controls are the bridge between the UI and the engine. The web UI auto-renders them by type.

`markDirty()` / `dirty()` / `clearDirty()` — allows modules to signal that their state changed and dependents need to update (e.g. layout change triggers LUT rebuild).

## What worked

- Single base class for everything keeps the system uniform.
- Fixed-capacity controls avoid heap allocation.
- `markDirty()` pattern works for propagating changes.

## What needs improvement

- `onChange` propagation is ad-hoc. Need observer/listener pattern (see v2's onChildrenReady as a starting point).
- Control types limited. Need: color picker (RGB), dropdown/enum, select (proven in MoonLight and v2).
- Consider v2's `onBuildControls()` / `onAllocateMemory()` separation instead of doing everything in setup().
- Consider v2's field order optimization for minimal padding.

## Prior art

### MoonLight — Node ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonBase/Nodes.h))
- Base ~29 bytes + vtable. Effects add only their control variables (uint8_t each). A typical effect adds 2 bytes on top.
- No std::string members (uses `Char<N>` fixed-size strings).
- `addControl()` binds to class variable by reference, stores `uintptr_t` pointer. Hot-path reads variable directly.
- Supports: uint8_t, int8_t, uint16_t, uint32_t, int, float, bool, Coord3D, select (dropdown).
- `classSize()` reports actual instance size for memory accounting.

### projectMM v1 — StatefulModule ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/core/StatefulModule.h))
- Same addControl-by-reference pattern as MoonLight. Controls stored as JSON descriptors, variable pointer as `uintptr_t`.

### projectMM v2 — MoonModule ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/core/MoonModule.h))
- Unified Module + StatefulModule into one class.
- `onBuildControls()` — all addControl() calls here, not in setup(). Supports rebuild via `clearControls()`.
- `onAllocateMemory()` — single hook for dynamic allocation, sets `moduleAllocBytes_`.
- `onChildrenReady()` — parent notified when all children finish setup.
- Field order optimized 8B→4B→2B→1B, saving 24 bytes vs naive order.
- AutoWireSpec for declarative input wiring ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/core/MoonModule.h#L55)).
- classSize set once at registration via `register_type<T>()` — zero per-class boilerplate.
