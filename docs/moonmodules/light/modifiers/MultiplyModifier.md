# Multiply Modifier

![MultiplyModifier controls](../../../assets/screenshots/MultiplyModifier.png)

![MultiplyModifier preview](../../../assets/screenshots/MultiplyModifier.gif)

Static modifier. Tiles the logical image across the physical box `multiply` times per axis, optionally reflecting alternate tiles. With a multiplier of 2 and mirror enabled on an axis, that axis folds in half — the classic kaleidoscope mirror. Multiply subsumes the old MirrorModifier: a pure mirror is `multiply = 2, mirror = true` on the chosen axes.

## Controls

- `multiplyX` (Uint8, 1–64, default 2) — tiles along X (1 = no tiling)
- `multiplyY` (Uint8, 1–64, default 2) — tiles along Y
- `multiplyZ` (Uint8, 1–64, default 1) — tiles along Z
- `mirrorX` (Bool, default true) — reflect alternate (odd) tiles along X
- `mirrorY` (Bool, default true) — reflect alternate tiles along Y
- `mirrorZ` (Bool, default true) — reflect alternate tiles along Z

The defaults (`multiply 2/2/1`, `mirror all on`) reproduce the canonical mirror-XY pipeline: a 128×128 physical grid folds to a 64×64 logical buffer, each logical light fanning out to its four reflected quadrants. (`mirrorZ` on is a no-op on a 2D/depth-1 layout.)

## Effect on the pipeline

- **Logical box shrinks by the multiplier**: `logW = physW / multiplyX` (etc.). 128×128 with multiply 2/2 → 64×64 logical (the effect renders a quarter of the lights). The effective multiplier clamps to the axis extent — `multiplyZ` on a depth-1 layout clamps to 1 (no-op), never blanking the layer.
- **Fan-out is the fold**: each physical light folds (`pos % logicalSize`) onto its logical cell, so the `multiplyX·multiplyY·multiplyZ` physical lights of the tiles all land on one logical light — the 1:N mapping emerges from the build with no fan-out list and no cap (see [ModifierBase § Fan-out is free](../ModifierBase.md)).
- **Tile vs fold**: with mirror **off** on an axis, tiles repeat (translate); with mirror **on**, odd-numbered tiles reflect within their tile (`size − 1 − pos`), so multiply 2 + mirror = a fold.
- **Integer division**: a physical extent not divisible by the multiplier leaves uncovered cells at the high edge (they map to nothing) — the same edge behaviour the old mirror had on odd widths, without a shared centre line.

## Cross-domain wiring

A Layer folds all its enabled modifiers as a chain (order matters: multiply-then-checkerboard ≠ checkerboard-then-multiply). The fold contract is in [ModifierBase](../ModifierBase.md).

## Tests

[Unit tests: MultiplyModifier](../../../tests/unit-tests.md#multiplymodifier) — logical dimensions, tile fan-out, per-axis mirror reflection, the pure-fold equivalence to the old Mirror, the multiplyZ-on-2D no-op, and the extent clamp.

[Scenario: scenario_MultiplyModifier_pipeline](../../../tests/scenario-tests.md#scenario_multiplymodifier_pipeline) — full pipeline with the multiply/mirror kaleidoscope, performance bounds.

## Prior art

### MoonLight — M_MoonLight.h Multiply ([source](https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Nodes/Modifiers/M_MoonLight.h))

MoonLight's Multiply node tiles via `position % modifierSize` and reflects odd tiles when its `mirror` flag is set (`position = size − 1 − position`). We expose **per-axis** mirror bools and per-axis multipliers instead of MoonLight's single multiplier coord + single mirror flag, so X/Y/Z fold and tile independently. MoonLight keeps Mirror, Multiply, Transpose, and Kaleidoscope as separate nodes; we fold mirror into Multiply since mirror is just multiply-2-with-reflection.

## Source

[MultiplyModifier.h](../../../../src/light/modifiers/MultiplyModifier.h)
