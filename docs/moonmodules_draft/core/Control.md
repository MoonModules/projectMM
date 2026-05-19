# Control

A named, typed value exposed by a MoonModule to the UI.

## Types

| Type | Storage | UI rendering |
|------|---------|-------------|
| Uint16 | `uint16_t value, min, max` | Slider |
| Bool | `bool value` | Checkbox/toggle |
| Text | `char[64]` | Text input |

## Storage

Uses a union for zero-heap inline storage. Name is `char[24]` (fixed). Truncation happens silently for names > 23 chars and text > 63 chars.

## What worked

- Fixed-size, no heap allocation per control.
- `setControl` clamps Uint16 to min/max automatically.
- `onChange` only fires when value actually changes.

## What needs improvement

- Missing types proven in prior versions: float, uint8_t, int8_t, uint32_t, Coord3D, select/dropdown, display (read-only), progress, button.
- Controls should be linked to class variables by reference (proven in all three prior versions).
- Dynamic control rebuild needed (v2's clearControls pattern).
- No control grouping or nesting.

## Prior art

### MoonLight — addControl ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonBase/Nodes.h#L80))
- Binds to class variable by reference via `reinterpret_cast<uintptr_t>(&variable)`. Hot-path reads variable directly — zero overhead.
- Types: uint8_t, int8_t, uint16_t, uint32_t, int, float, bool, Coord3D.
- UI types: "slider", "select", "toggle", "text", "display".
- Select (dropdown): `addControlValue()` appends options.
- Size code system encodes type for non-template helper.

### projectMM v1 — addControl ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/core/StatefulModule.h))
- Same pattern. Also supports "display" (read-only), "progress" (progress bar), "button" (action).

### projectMM v2 — ControlDescriptor ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/core/MoonModule.h#L40))
- Richer: key, uiType, CtrlType enum, ptr, min/max, default, options array, ownsOptions flag, system flag (survives clearControls).
- Additional: FloatConst, EditStr (editable string with max length), Select with dynamic options.
- `controlAllocBytes()` — pre-check: how much would a control change allocate?
