# LayoutGroup

Groups layouts. Shared across all layers and drivers. Defines the
physical light topology.

## API

- `addLayout(LayoutBase*)` — add a layout (max 8)
- `totalLightCount()` — sum of all layouts' light counts
- `forEachCoord(callback, ctx)` — iterate all coordinates across all
  layouts, offsetting physical indices so they don't overlap

## LayoutBase Interface

Layouts must implement:
- `lightCount()` — number of lights this layout defines
- `forEachCoord(CoordCallback, ctx)` — yield (idx, x, y, z) for each

The callback signature: `void(void* ctx, uint32_t idx, int16_t x, int16_t y, int16_t z)`

## What worked

- Shared across layers and drivers — single source of truth for
  physical topology.
- Physical index offsetting when multiple layouts are combined.
- Non-template callback via function pointer + void* ctx works for
  virtual dispatch.

## What needs improvement

- Each layout needs a `GridAdapter` wrapper class (inherits both
  GridLayout and LayoutBase) because GridLayout uses templates for
  forEachCoord and LayoutBase needs virtual dispatch. This boilerplate
  should be eliminated — either make all layouts implement LayoutBase
  directly, or use a different dispatch mechanism.
- When a layout's controls change, all layers must rebuild their LUTs.
  This propagation is ad-hoc (dirty flag checked in main loop). Should
  be event-driven.
- No way to remove or reorder layouts at runtime.
