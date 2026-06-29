# Layer

A `Layer` MoonModule (role `ModuleRole::Layer`, child of the [Layers](Layers.md) container) owns a buffer, a mapping LUT, effects, and modifiers. References a shared [Layouts](Layouts.md).

> **Naming convention.** Capital `Layer` and `Layers` are class names; lowercase "layer"/"layers" is the English singular/plural — used freely when the sentence makes the meaning clear. Capitalisation disambiguates "the Layers container" from "two layers stacked". Same rule for `Layouts`/layout and `Drivers`/driver.

## Ownership

- **Buffer** — logical light data, sized to logical dimensions
- **MappingLUT** — maps logical lights to physical positions
- **Effects** (ordered list) — write lights into buffer. No hard-coded max — dynamic list (heap-allocated, grown as needed).
- **Modifiers** (ordered list) — transform LUT or light values. Same dynamic list approach.

## blendMode / opacity controls

Two controls govern how this Layer composites onto the layers below it: `blendMode` (a select — `alpha` over, or `additive` sum-with-clamp) and `opacity` (`uint8`, 0 = invisible, 255 = full). They are **inert on the Layer** — the Layer never reads them; it just carries them so they travel through add / delete / reorder with no separate synchronised list. The [Drivers](Drivers.md) container reads each enabled Layer's two values plus the [Layers](Layers.md) container's child order and does the actual compositing (bottom layer overwrites, each layer above blends per its mode + opacity). The bottom (first-composited) Layer's `blendMode`/`opacity` are moot — nothing sits under it. The blend math itself lives in [BlendMap](BlendMap.md). Precedent for "value here, logic in Drivers": the per-X `Correction` data Drivers applies.

## Key operations

### rebuildLUT()

Cold-path. Called when layout or modifier controls change.

1. Gets physical dimensions and total light count from Layouts
2. Applies static modifiers to compute logical dimensions (e.g. 128x128 → 64x64 with mirror X+Y)
3. Allocates buffer to logical dimensions
4. Allocates LUT
5. For each logical light, asks modifier for physical destinations via virtual interface
6. Without modifier AND with grid layout (no sparse, no serpentine, X-then-Y order): 1:1 unshuffled — `oneToOneMapping` flag set, mapping table skipped entirely

### render(elapsed_ms)

Hot-path. Called every frame.

1. Creates rendering context with buffer, logical dims, elapsed time
2. Runs each effect in order (all write to same buffer)
3. **After each effect's `loop()`, calls `extrude(eff->dimensions())`** — duplicates the effect's written slice across the axes it doesn't iterate (see below)
4. Runs dynamic modifiers in order via virtual `transformLights()` (each transforms the buffer)

### extrude(effectDim)

Lets a low-dimensional effect work on a higher-dimensional layer without per-effect changes. Reads the effect's `dimensions()` declaration and fills the unused axes from the slice the effect wrote:

- `Dim::D3` — early return; no work, no allocation, one comparison + branch.
- `Dim::D2` — `memcpy` the z=0 slice across every z>0 (guarded by `depth > 1`).
- `Dim::D1` — `memcpy` the y=0 row across every y>0 within z=0, then duplicate z=0 across z (each guarded).

Hot-path cost is zero for D3 effects (the default) and zero when the layer's unused axes are size 1 (a D2 effect on a 2D layer, a D1 effect on a 1D layer). Real `memcpy` work only happens when the layer has more dimensions than the effect writes — exactly the case the framework is meant to handle.

See [EffectBase § Dimensions and auto-extrusion](EffectBase.md#dimensions-and-auto-extrusion) for the effect-side contract and [Unit tests: Layer](../../tests/unit-tests.md#layer) for the pinned tests (see `unit_Layer_extrude.cpp`).

## Status

The Layer's status line (the `MoonModule` status slot) shows the **logical** box the effects render into — `"<w>×<h>×<d>"`, the dimensions its modifiers reshape. This differs from the physical bounding box shown on the [Layouts](Layouts.md#status) container: a Mirror-XY modifier on a 128×128 physical layout renders into a 64×64 logical box (the half that gets folded), so the Layer reads `64×64×1` while Layouts reads `128×128×1`. The gap between the two is the modifier's effect, made visible.

The same slot carries memory-degradation warnings (`Severity::Warning`/`Error`) when a build can't fit: `"modifier LUT skipped — not enough memory"`, `"sparse LUT build failed — not enough memory"`, `"buffer reduced — not enough memory"`, `"buffer allocation failed — not enough memory"`. A warning wins over the neutral box line — when the Layer is degraded, that's what the user needs to see. Recomputed on every rebuild (`onBuildState`), not per tick.

## EffectBase overlap

Consider whether Layer itself can provide the rendering context (buffer, dims, elapsed time) directly to effects, eliminating the need for a separate EffectBase class. The layer already knows everything the effect needs.

## Prior art

### MoonLight — VirtualLayer ([source](https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Layers/VirtualLayer.h))

- `startPct`/`endPct` as Coord3D percentages (0-100) of the total fixture.
- `oneToOneMapping` flag for fast path.
- `brightness` per layer (0-255) + `transitionBrightness` for smooth fade-in/out.
- `virtualChannels` — per-layer buffer.
- `effectDimension` — 1D/2D/3D.
- `nodes` vector for effects/modifiers (dynamic, not fixed-capacity).
- `forEachLight` — per-logical-light iteration that asks the modifier for physical destinations; the LUT build uses the same per-light virtual-dispatch pattern.

### projectMM v1 — EffectsLayer ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/modules/layers/EffectsLayer.h))

Container for effects. Owns Channel (pixel buffer). Effects wired via `setInput("layer", ...)`.

### projectMM v2 — PixelEffectBase ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/effects/PixelEffectBase.h))

Shared spine eliminates ~70 lines boilerplate per effect. Layout resolution by category, not type string. DataBuffer + DataRegistry for producer/consumer decoupling.

## Source

[Layer.h](../../../src/light/layers/Layer.h)
