# Checkerboard Modifier

![CheckerboardModifier controls](../../../assets/light/modifiers/CheckerboardModifier.png)

![CheckerboardModifier preview](../../../assets/light/modifiers/CheckerboardModifier.gif)

Static modifier. Masks the layer in a checkerboard pattern: lights in the "off" squares are dropped (they receive nothing), lights in the "on" squares pass through unchanged. Unlike Multiply, this doesn't remap or resize — it's a spatial on/off mask applied to whatever the effect drew.

## Controls

- `size` (Uint8, 1–64, default 2) — checker square edge, in lights
- `invert` (Bool, default false) — flip which squares pass through

## Effect on the pipeline

- **Logical box unchanged** — a mask doesn't resize the box (no `modifyLogicalSize`); only which cells contribute changes.
- **Pass or drop** — `modifyLogical` returns `true` to pass a light through unchanged, or `false` to drop it (an "off" square), so a dropped physical light has no logical source and stays dark.
- **Square parity**: a light at `(x,y,z)` belongs to square `(x/size, y/size, z/size)`; the square is "on" when the sum of those indices is even (flipped by `invert`).

## Cross-domain wiring

A Layer folds all its enabled modifiers as a chain (Checkerboard-then-Multiply differs from Multiply-then-Checkerboard). The fold + reject contract is in [ModifierBase](../ModifierBase.md).

## Tests

[Unit tests: CheckerboardModifier](../../../tests/unit-tests.md#checkerboardmodifier) — identity dimensions, the drop pattern for both `invert` phases, `size` grouping into squares.

[Scenario: scenario_modifier_swap](../../../tests/scenario-tests.md#scenario_modifier_swap) — replaces the Layer's modifier between Multiply and Checkerboard and verifies the pipeline stays live across each swap.

## Prior art

### MoonLight — M_MoonLight.h Checkerboard ([source](https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Nodes/Modifiers/M_MoonLight.h))

MoonLight's Checkerboard drops lights by setting `position.x = UINT16_MAX` (a sentinel the layout pass skips), with `size`, `invert`, and a `group` flag. We express the drop as `modifyLogical` returning `false` (no sentinel needed) and start with `size` + `invert`; `group` is deferred.

## Source

[CheckerboardModifier.h](../../../../src/light/modifiers/CheckerboardModifier.h)
