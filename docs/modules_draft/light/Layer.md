# Layer

Owns a buffer, a mapping LUT, effects, and modifiers. References a
shared LayoutGroup.

## Ownership

- **Buffer** — logical pixel data, sized to logical dimensions
- **MappingLUT** — maps logical pixels to physical positions
- **Effects** (ordered list, max 4) — write pixels into buffer
- **Modifiers** (ordered list, max 4) — transform LUT or pixels

## Key Operations

### rebuildLUT()

Cold-path. Called when layout, modifier controls, or modifier
add/remove changes.

1. Scans fixture for physical dimensions (w, h, d)
2. Applies static modifiers (MirrorModifier) to compute logical
   dimensions (e.g. 128x128 → 64x64 with mirror X+Y)
3. Allocates buffer to logical dimensions
4. Allocates LUT with logical count and total destinations
5. For each logical pixel, asks modifier for physical destinations
6. Without modifier: 1:1 mapping (logIdx → physIdx)

### render(frame)

Hot-path. Called every frame.

1. Creates RenderContext with buffer, logical dims, frame number
2. Runs each effect in order (all write to same buffer)
3. Runs dynamic modifiers in order (each transforms the buffer)

## What worked

- Layer correctly owns buffer + LUT. Separation from LayoutGroup is
  clean.
- Multiple effects in a layer works (sequential overwrite).
- Mirror modifier producing 1:N LUT entries works (kaleidoscope).
- `logDims_` correctly tracks reduced dimensions after mirror.

## What needs improvement

- `rebuildLUT` is tightly coupled to MirrorModifier via dynamic_cast.
  Should use a modifier interface method instead.
- The render method is also tightly coupled to RotateModifier via
  dynamic_cast. Need a generic dynamic modifier interface.
- When rebuildLUT is called from HTTP handler, it happens mid-frame
  cycle. Should be deferred to frame boundary.
- No blending mode between effects. Effects just overwrite. Need at
  least: overwrite, additive, alpha blend.
- Layer doesn't know its own index or name. Makes debugging and UI
  identification difficult.
