# Modifiers

Every modifier, one compact row each. A modifier sits between an [effect](../effects/effects.md) and the output: it reshapes *where* pixels land (or masks them) without changing the effect's drawing. Modifiers compose — a [Layer](../Layer.md) folds its whole modifier stack each rebuild; a *dynamic* modifier (one that overrides `modifyLive`) also runs a per-frame pass. See [ModifierBase](../ModifierBase.md) for the static-vs-dynamic split.

Columns: **Name** (with its `tags()` emoji — see the [tag emoji legend](../../../architecture.md#tag-emoji-legend)), **Preview**, **Kind** (static = baked into the mapping at rebuild; dynamic = per-frame remap), **Description**, **Controls**, **Tests**. The per-library file split is future work — see the [folder-structure decision](../../../backlog/folder-structure-proposal.md).

## MoonLight modifiers

| Name | Preview | Kind | Description | Controls | Tests |
|---|---|---|---|---|---|
| <a id="multiply"></a>**Multiply** 💫 | <img src="../../../assets/light/modifiers/MultiplyModifier.gif" width="300"> | static | Tiles the logical image across the box `multiply` times per axis, optionally mirroring alternate tiles (a pure mirror is `multiply = 2, mirror = true`). | `multiplyX`, `multiplyY`, `multiplyZ`, `mirrorX`, `mirrorY`, `mirrorZ` | [tests](../../../tests/unit-tests.md#multiplymodifier) |
| <a id="checkerboard"></a>**Checkerboard** 💫 | <img src="../../../assets/light/modifiers/CheckerboardModifier.gif" width="300"> | static | Masks the layer in a checkerboard: "off" squares are dropped, "on" squares pass through unchanged. | `size`, `invert` | [tests](../../../tests/unit-tests.md#checkerboardmodifier) |

## projectMM-native modifiers

| Name | Preview | Kind | Description | Controls | Tests |
|---|---|---|---|---|---|
| <a id="region"></a>**Region** | — | static | Carves the layer to a sub-rectangle given as percentages of the physical extent (so it survives a resize); outside the region is dark. | `startX`, `startY`, `startZ`, `endX`, `endY`, `endZ` | [tests](../../../tests/unit-tests.md#regionmodifier) |
| <a id="randommap"></a>**RandomMap** | — | dynamic | Remaps every light to another via a true 1:1 permutation, reshuffling to a fresh permutation on a `bpm` timer — the arrangement scrambles each beat, the content is untouched. | `bpm` | [tests](../../../tests/unit-tests.md#randommapmodifier) |
| <a id="rotate"></a>**Rotate** | — | dynamic | Rotates the 2D image around its centre, turning continuously over time (the codebase's transform-matrix reference). | `speed` | [tests](../../../tests/unit-tests.md#rotatemodifier) |

## Source

- [CheckerboardModifier.h](../../../../src/light/modifiers/CheckerboardModifier.h)
- [MultiplyModifier.h](../../../../src/light/modifiers/MultiplyModifier.h)
- [RandomMapModifier.h](../../../../src/light/modifiers/RandomMapModifier.h)
- [RegionModifier.h](../../../../src/light/modifiers/RegionModifier.h)
- [RotateModifier.h](../../../../src/light/modifiers/RotateModifier.h)
