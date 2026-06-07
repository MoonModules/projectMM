# Checkerboard Modifier

Static modifier. Masks the layer in a checkerboard pattern: lights in the "off" squares are dropped (they receive nothing), lights in the "on" squares pass through unchanged. Unlike Multiply, this doesn't remap or resize — it's a spatial on/off mask applied to whatever the effect drew.

## Controls

- `size` (Uint8, 1–64, default 2) — checker square edge, in lights
- `invert` (Bool, default false) — flip which squares pass through

## Effect on the pipeline

- **Logical dimensions unchanged** (identity) — the box is the same; only which cells contribute changes.
- **1:1 or 1:0 mapping** — each logical light maps to itself (one physical position) if its square is "on", or to **nothing** if "off". The "drop" is expressed as `outCount = 0` from `mapToPhysical`; `Layer::rebuildLUT` records that as a logical light with no destination (the same zero-destination path the sparse layout translation already uses), so a dropped light simply doesn't appear in the driver buffer. `maxMultiplier()` is 1 — it never fans out.
- **Square parity**: a light at `(x,y,z)` belongs to square `(x/size, y/size, z/size)`; the square is "on" when the sum of those indices is even (flipped by `invert`).

## Cross-domain wiring

A Layer applies its first enabled modifier during `rebuildLUT`; modifier chaining (where Checkerboard-then-Multiply would differ from Multiply-then-Checkerboard) is not yet implemented — see [architecture.md § Modifiers](../../../architecture.md#modifiers). The mask integrates with no `ModifierBase` contract change because the contract already permits a logical light to map to zero physical positions.

## Tests

[Unit tests: CheckerboardModifier](../../../tests/unit-tests.md#checkerboardmodifier) — identity dimensions, the drop pattern for both `invert` phases, `size` grouping into squares.

[Scenario: scenario_modifier_swap](../../../tests/scenario-tests.md#scenario_modifier_swap) — replaces the Layer's modifier between Multiply and Checkerboard and verifies the pipeline stays live across each swap.

## Prior art

### MoonLight — M_MoonLight.h Checkerboard ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Modifiers/M_MoonLight.h))

MoonLight's Checkerboard drops lights by setting `position.x = UINT16_MAX` (a sentinel the layout pass skips), with `size`, `invert`, and a `group` flag. We express the drop as `outCount = 0` (our equivalent, no sentinel needed) and start with `size` + `invert`; `group` is deferred.
