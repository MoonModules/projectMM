# Multiply Modifier

Static modifier. Tiles the logical image across the physical box `multiply` times per axis, optionally reflecting alternate tiles. With a multiplier of 2 and mirror enabled on an axis, that axis folds in half — the classic kaleidoscope mirror. Multiply subsumes the old MirrorModifier: a pure mirror is `multiply = 2, mirror = true` on the chosen axes.

## Controls

- `multiplyX` (Uint8, 1–8, default 2) — tiles along X (1 = no tiling)
- `multiplyY` (Uint8, 1–8, default 2) — tiles along Y
- `multiplyZ` (Uint8, 1–8, default 1) — tiles along Z
- `mirrorX` (Bool, default true) — reflect alternate (odd) tiles along X
- `mirrorY` (Bool, default true) — reflect alternate tiles along Y
- `mirrorZ` (Bool, default false) — reflect alternate tiles along Z

The defaults (`multiply 2/2/1`, `mirror true/true/false`) reproduce the canonical mirror-XY pipeline: a 128×128 physical grid folds to a 64×64 logical buffer, each logical light fanning out to its four reflected quadrants.

## Effect on the pipeline

- **Logical box shrinks by the multiplier**: `logW = physW / multiplyX` (etc.). 128×128 with multiply 2/2 → 64×64 logical (the effect renders a quarter of the lights).
- **LUT produces 1:N mappings**: each logical light maps to `multiplyX·multiplyY·multiplyZ` physical positions (the tiles). The product is the LUT fan-out, bounded to **≤ 8** (the fixed `physicals[8]` stack buffer in `Layer::rebuildLUT`); each axis is capped at 8 and `mapToPhysical` never writes past `maxOut`.
- **Tile vs fold**: with mirror **off** on an axis, tiles repeat (translate); with mirror **on**, odd-numbered tiles reflect within their tile (`size − 1 − pos`), so multiply 2 + mirror = a fold.
- **Integer division**: a physical extent not divisible by the multiplier leaves uncovered cells at the high edge (they map to nothing) — the same edge behaviour the old mirror had on odd widths, without a shared centre line.

## Cross-domain wiring

A Layer applies its first enabled modifier during `rebuildLUT` (modifier chaining — where order would matter, e.g. multiply-then-checkerboard ≠ checkerboard-then-multiply — is not yet implemented; see [architecture.md § Modifiers](../../../architecture.md#modifiers)). `mapToPhysical` emits physical box indices; the Layer translates them to driver indices and drops any that fall outside the real light set.

## Tests

[Unit tests: MultiplyModifier](../../../tests/unit-tests.md#multiplymodifier) — logical dimensions, tile fan-out, per-axis mirror reflection, the pure-fold equivalence to the old Mirror, the ≤8 fan-out clamp.

[Scenario: scenario_MultiplyModifier_pipeline](../../../tests/scenario-tests.md#scenario_multiplymodifier_pipeline) — full pipeline with the multiply/mirror kaleidoscope, performance bounds.

## Prior art

### MoonLight — M_MoonLight.h Multiply ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Modifiers/M_MoonLight.h))

MoonLight's Multiply node tiles via `position % modifierSize` and reflects odd tiles when its `mirror` flag is set (`position = size − 1 − position`). We expose **per-axis** mirror bools and per-axis multipliers instead of MoonLight's single multiplier coord + single mirror flag, so X/Y/Z fold and tile independently. MoonLight keeps Mirror, Multiply, Transpose, and Kaleidoscope as separate nodes; we fold mirror into Multiply since mirror is just multiply-2-with-reflection.
