# Control

A named, typed value exposed by a MoonModule to the UI.

## Types

| Type | Storage | UI rendering |
|------|---------|-------------|
| Uint16 | `uint16_t value, min, max` | Slider |
| Bool | `bool value` | Checkbox/toggle |
| Text | `char[64]` | Text input |

## Storage

Uses a union for zero-heap inline storage. Name is `char[24]` (fixed).
Truncation happens silently for names > 23 chars and text > 63 chars.

## What worked

- Fixed-size, no heap allocation per control.
- `setControl` clamps Uint16 to min/max automatically.
- `onChange` only fires when value actually changes.

## What needs improvement

- Missing types: RGB color picker, enum/dropdown, float.
- No control grouping or nesting (e.g. "advanced" section).
- `char[24]` for name is tight — some descriptive names get truncated.
- No validation callback (e.g. "IP must be valid format").
