# Modifiers

Every modifier, one block each: its preview, what it does, and what each control means — together. A modifier sits between an [effect](../effects/effects.md) and the output: it reshapes *where* pixels land (or masks them) without changing the effect's drawing. Modifiers compose — a [Layer](../Layer.md) folds its whole modifier stack each rebuild; a *dynamic* modifier (one that overrides `modifyLive`) also runs a per-frame pass. See [ModifierBase](../ModifierBase.md) for the static-vs-dynamic split. Each block's emoji are its `tags()` (see the [tag emoji legend](../../../architecture.md#tag-emoji-legend)); **Kind** is static (baked into the mapping at rebuild) or dynamic (per-frame remap). Modifiers are grouped into sections, and each block carries that modifier's preview, behaviour, and control descriptions together. (For how this page maps to the source/asset folders, see the [folder-structure decision](../../../backlog/folder-structure-proposal.md).)

## MoonLight modifiers

<a id="checkerboard"></a>

### Checkerboard 💫 · static

<img src="../../../assets/light/modifiers/CheckerboardModifier.gif" width="300" alt="Checkerboard modifier preview">

Masks the layer in a checkerboard: "off" squares are dropped, "on" squares pass through unchanged.

- `size` — checker square edge in lights (≥1).
- `invert` — flip which squares pass through vs are masked.

[Tests](../../../tests/unit-tests.md#checkerboardmodifier)

<a id="multiply"></a>

### Multiply 💫 · static

<img src="../../../assets/light/modifiers/MultiplyModifier.gif" width="300" alt="Multiply modifier preview">

Tiles the logical image across the box `multiply` times per axis, optionally mirroring alternate tiles (a pure mirror is `multiply = 2, mirror = true`).

- `multiplyX` / `multiplyY` / `multiplyZ` — tile count per axis (1–64; 1 = no tiling).
- `mirrorX` / `mirrorY` / `mirrorZ` — reflect alternate tiles on that axis (with a count of 2, folds the axis in half — the kaleidoscope mirror).

[Tests](../../../tests/unit-tests.md#multiplymodifier)

## projectMM-native modifiers

<a id="randommap"></a>

### RandomMap · dynamic

Remaps every light to another via a true 1:1 permutation, reshuffling to a fresh permutation on a `bpm` timer — the arrangement scrambles each beat, the content is untouched.

- `bpm` — reshuffles per minute (0–60; 6 ≈ a fresh permutation every 10 s; 0 = frozen).

[Tests](../../../tests/unit-tests.md#randommapmodifier)

<a id="region"></a>

### Region · static

Carves the layer to a sub-rectangle given as percentages of the physical extent (so it survives a resize); outside the region is dark.

- `startX` / `startY` / `startZ` and `endX` / `endY` / `endZ` — the sub-rectangle bounds as **percentages** of each axis's physical extent (0 = start of axis, 100 = end), so the region survives a resize; values may go negative or past 100 to push the window off-screen.

[Tests](../../../tests/unit-tests.md#regionmodifier)

<a id="rotate"></a>

### Rotate · dynamic

Rotates the 2D image around its centre, turning continuously over time (the codebase's transform-matrix reference).

- `speed` — rotation speed (1–255; turns faster as it rises).

[Tests](../../../tests/unit-tests.md#rotatemodifier)

## Source

- [CheckerboardModifier.h](../../../../src/light/modifiers/CheckerboardModifier.h)
- [MultiplyModifier.h](../../../../src/light/modifiers/MultiplyModifier.h)
- [RandomMapModifier.h](../../../../src/light/modifiers/RandomMapModifier.h)
- [RegionModifier.h](../../../../src/light/modifiers/RegionModifier.h)
- [RotateModifier.h](../../../../src/light/modifiers/RotateModifier.h)
