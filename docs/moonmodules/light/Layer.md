# Layer

Owns a buffer, a mapping LUT, effects, and modifiers. References a shared LayoutGroup.

## Ownership

- **Buffer** — logical light data, sized to logical dimensions
- **MappingLUT** — maps logical lights to physical positions
- **Effects** (ordered list) — write lights into buffer. No hard-coded max — use dynamic list (heap-allocated, grown as needed).
- **Modifiers** (ordered list) — transform LUT or light values. Same dynamic list approach.

## Key operations

### rebuildLUT()

Cold-path. Called when layout or modifier controls change.

1. Gets physical dimensions and total light count from LayoutGroup
2. Applies static modifiers to compute logical dimensions (e.g. 128x128 → 64x64 with mirror X+Y)
3. Allocates buffer to logical dimensions
4. Allocates LUT
5. For each logical light, asks modifier for physical destinations via virtual interface
6. Without modifier AND with grid layout (no sparse, no serpentine, X-then-Y order): 1:1 unshuffled — `oneToOneMapping` flag set, mapping table skipped entirely

### render(elapsed_ms)

Hot-path. Called every frame.

1. Creates rendering context with buffer, logical dims, elapsed time
2. Runs each effect in order (all write to same buffer)
3. Runs dynamic modifiers in order via virtual `transformLights()` (each transforms the buffer)

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
