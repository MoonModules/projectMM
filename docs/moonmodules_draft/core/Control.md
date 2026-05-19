# Control

A named, typed value exposed by a MoonModule to the UI. Must be the lightest weight possible.

## Design

Controls bind to class variables by reference (proven in MoonLight, v1, v2). The control stores a pointer to the variable. Hot-path code reads the variable directly — zero overhead. No getter/setter.

## Types

| Type | Storage | UI | DMX-compatible |
|------|---------|-----|----------------|
| uint8_t | 1 byte, min/max | Slider (0-255) | Yes — preferred default |
| bool | 1 byte | Toggle | Yes (0/1) |
| char[N] | fixed buffer | Text input | No |
| select | uint8_t index | Dropdown | Yes (mode selection) |

Additional types — add only when needed:
- uint16_t — for values that genuinely exceed 255 (e.g. universe number)
- float — use minimally, prefer uint8_t where possible
- Coord3D — for position controls
- display (read-only) — for status values
- progress — for progress bars

No color picker (RGB) control type — effects use palette index (uint8_t) instead.

## Memory footprint

Controls must be as small as possible. Each control descriptor stores: pointer to variable (4 bytes on ESP32), name pointer to flash (4 bytes), type enum (1 byte), min/max (type-dependent). Target: under 16 bytes per control descriptor.

This 16-byte target is for the in-memory runtime descriptor only. Previous versions used ArduinoJson for control storage which was significantly heavier (JSON object per control, dynamic allocation). The v3 approach: control VALUE lives in the class variable (1-4 bytes, no overhead), control DESCRIPTOR is the lightweight metadata for UI rendering and persistence.

Fixed-capacity array per module. No heap allocation per control. Capacity chosen at compile time — if a module needs more controls than the default capacity, it's probably too complex.

## Persistence

Control values must be saved/loaded persistently (filesystem). The mechanism:
- **Save:** iterate control descriptors, read each variable via its pointer, write key:value to a file. Format: compact binary or minimal JSON — TBD based on what's simplest.
- **Load:** read file, match keys to control descriptors, write values via pointer. Apply pending values in `onBuildControls()` (same pattern as v2's `applyPending_`).
- **When:** save on control change (debounced). Load at setup before `onBuildControls()`.
- **Scope:** one file per module instance, or one file for all modules — decide based on filesystem constraints (LittleFS on ESP32 has limited file count).

## Dynamic controls

`onBuildControls()` in MoonModule supports rebuilding the control set at runtime. When a mode selector changes, call `onBuildControls()` again — it clears and rebuilds, showing only controls relevant to the current mode.

## Deferred

- No `controlAllocBytes()` pre-check — defer until needed.
- No richer types (v2-style ControlDescriptor) until genuinely needed.

## Prior art

### MoonLight — addControl ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonBase/Nodes.h#L80))
- Binds via `reinterpret_cast<uintptr_t>(&variable)`. Types: uint8_t, int8_t, uint16_t, uint32_t, int, float, bool, Coord3D. UI types: "slider", "select", "toggle", "text", "display". Select via `addControlValue()`.

### projectMM v1 — addControl ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/core/StatefulModule.h))
- Same pattern. Also supports "display", "progress", "button".

### projectMM v2 — ControlDescriptor ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/core/MoonModule.h#L40))
- Richer but heavier: key, uiType, CtrlType enum, ptr, min/max, default, options array, ownsOptions flag, system flag. Not all of this weight is justified for v3.
