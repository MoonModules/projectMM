# Layer

A `Layer` MoonModule (role `ModuleRole::Layer`, child of the [Layers](Layers.md) container) owns a buffer, a mapping LUT, effects, and modifiers. References a shared [Layouts](Layouts.md).

> **Naming convention.** Capital `Layer` and `Layers` are class names; lowercase "layer"/"layers" is the English singular/plural — used freely when the sentence makes the meaning clear. Capitalisation disambiguates "the Layers container" from "two layers stacked". Same rule for `Layouts`/layout and `Drivers`/driver.

## Ownership

- **Buffer** — logical light data, sized to logical dimensions
- **MappingLUT** — maps logical lights to physical positions
- **Effects** (ordered list) — write lights into buffer. No hard-coded max — dynamic list (heap-allocated, grown as needed).
- **Modifiers** (ordered list) — transform LUT or light values. Same dynamic list approach.

## start/end controls

Each Layer carries six `int16_t` controls — `startX`, `startY`, `startZ`, `endX`, `endY`, `endZ` — that select a region of the shared Layouts **expressed as percentages of the physical extent on each axis**. Defaults are `start = 0, end = 100` (the full layout). Percentages are resilient to physical layout changes: a `startX = 25` Layer stays at the same relative position when the panel resizes from 64×64 to 128×128, rather than ending up at the wrong absolute pixel.

Negative values and values > 100 are legal: a future modifier could drag a Layer in or out of the visible area by shifting start/end past 0% or 100% (e.g. `startX = -50` means the Layer extends 50% off the left edge of the layout). `ControlType::Int16` is the wire type so negative values round-trip correctly through `/api/state`, `/api/types`, and persistence.

Today (single-Layer pipeline) `rebuildLUT()` ignores the controls — the values are persisted state, not yet wired. They surface in the UI now so the surface stays stable when the composition follow-up activates them. **Rounding rule (when activated):** `start` percentages round toward the lower pixel (floor), `end` percentages round toward the higher pixel (ceiling). This guarantees a non-zero region on small panels (e.g. `start = 33, end = 66` on a 4-wide axis produces pixels 1..3 inclusive, not 1..2 or 2..2). Spec: [architecture.md § Layers and Layer](../../architecture.md#layers-and-layer).

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

See [EffectBase § Dimensions and auto-extrusion](EffectBase.md#dimensions-and-auto-extrusion) for the effect-side contract and [test_extrude](../../testing.md#extrude) for the pinned tests.

## EffectBase overlap

Consider whether Layer itself can provide the rendering context (buffer, dims, elapsed time) directly to effects, eliminating the need for a separate EffectBase class. The layer already knows everything the effect needs.

## Prior art

### MoonLight — VirtualLayer ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Layers/VirtualLayer.h))

- `startPct`/`endPct` as Coord3D percentages (0-100) of the total fixture.
- `oneToOneMapping` flag for fast path.
- `brightness` per layer (0-255) + `transitionBrightness` for smooth fade-in/out.
- `virtualChannels` — per-layer buffer.
- `effectDimension` — 1D/2D/3D.
- `nodes` vector for effects/modifiers (dynamic, not fixed-capacity).
- `forEachLight` — per-logical-light iteration that asks the modifier for physical destinations; v3's LUT build uses the same per-light virtual-dispatch pattern.

### projectMM v1 — EffectsLayer ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/modules/layers/EffectsLayer.h))

Container for effects. Owns Channel (pixel buffer). Effects wired via `setInput("layer", ...)`.

### projectMM v2 — PixelEffectBase ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/effects/PixelEffectBase.h))

Shared spine eliminates ~70 lines boilerplate per effect. Layout resolution by category, not type string. DataBuffer + DataRegistry for producer/consumer decoupling.
