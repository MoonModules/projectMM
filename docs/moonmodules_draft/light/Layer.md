# Layer

Owns a buffer, a mapping LUT, effects, and modifiers. References a shared LayoutGroup.

## Ownership

- **Buffer** — logical light data, sized to logical dimensions
- **MappingLUT** — maps logical lights to physical positions
- **Effects** (ordered list, max 4) — write lights into buffer
- **Modifiers** (ordered list, max 4) — transform LUT or light values

## Key Operations

### rebuildLUT()

Cold-path. Called when layout or modifier controls change.

1. Scans LayoutGroup for physical dimensions (w, h, d)
2. Applies static modifiers to compute logical dimensions (e.g. 128x128 → 64x64 with mirror X+Y)
3. Allocates buffer to logical dimensions
4. Allocates LUT with logical count and total destinations
5. For each logical light, asks modifier for physical destinations via virtual interface
6. Without modifier: 1:1 mapping

### render(elapsed_ms)

Hot-path. Called every frame.

1. Creates RenderContext with buffer, logical dims, elapsed time
2. Runs each effect in order (all write to same buffer)
3. Runs dynamic modifiers in order via virtual `transformLights()` (each transforms the buffer)

## What worked

- Layer correctly owns buffer + LUT. Separation from LayoutGroup is clean.
- Multiple effects in a layer works (sequential overwrite).
- Mirror modifier producing 1:N LUT entries works (kaleidoscope).

## What needs improvement

- Modifiers should use virtual interface (transformCoord, transformLights), not dynamic_cast.
- Rebuild should be deferred to frame boundary, not triggered mid-frame from HTTP handler.
- Should add: per-layer brightness (MoonLight), transition brightness for smooth effect switching.
- Should add: layer start/end position within physical layout (MoonLight's startPct/endPct).
- Should add: oneToOneMapping fast path flag (MoonLight).

## Prior art

### MoonLight — VirtualLayer ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Layers/VirtualLayer.h))
- `startPct`/`endPct` as Coord3D percentages (0-100) of the total fixture. Proven layer-within-fixture positioning.
- `oneToOneMapping` flag for fast path when mapping is identity.
- `brightness` per layer (0-255) + `transitionBrightness` for smooth fade-in/out on effect switch.
- `fadeBy` — requested fade amount consumed by PhysicalLayer next frame.
- `virtualChannels` — per-layer buffer (effects write here, compositeTo maps to physical).
- `effectDimension` — 1D/2D/3D, derived from effect and layout.

### projectMM v2 — PixelEffectBase ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/effects/PixelEffectBase.h))
- Shared spine eliminates ~70 lines boilerplate per effect. Concrete effect implements only `build_effect_controls()` + `render_(px, w, h, d)`.
- Layout resolution by category, not type string.
- DataBuffer + DataRegistry for producer/consumer decoupling.

### projectMM v1 — EffectsLayer ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/modules/layers/EffectsLayer.h))
Container for effects. Owns Channel (pixel buffer). Effects wired via `setInput("layer", ...)`.
