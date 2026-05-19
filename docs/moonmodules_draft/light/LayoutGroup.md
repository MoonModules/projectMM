# LayoutGroup

Groups layouts. Shared across all layers and drivers. Defines the physical light topology.

## API

- `addLayout(layout)` — add a layout. No hard-coded max — use dynamic list (heap-allocated).
- `totalLightCount()` — sum of all layouts' light counts. Provided to layers and driver groups for buffer allocation.
- `forEachCoord(callback, ctx)` — iterate all coordinates across all layouts, offsetting physical indices so they don't overlap.

`forEachCoord` is a LayoutGroup function, not a Layer function. The layer USES the LayoutGroup's coordinates to build its LUT, but the iteration itself is owned by the LayoutGroup.

## Layout interface

Layouts must implement virtual dispatch directly — no adapter boilerplate. The v3 prototype required a `GridAdapter` wrapper class because GridLayout used templates for forEachCoord while LayoutBase needed virtual dispatch. This must be eliminated: all layouts implement the virtual interface directly.

- `lightCount()` — number of lights this layout defines
- `forEachCoord(callback, ctx)` — yield (idx, x, y, z) for each light

Callback signature uses the platform typedefs: `void(void* ctx, nrOfLightsType idx, lengthType x, lengthType y, lengthType z)`

## Multiple layouts combined

A LayoutGroup can contain multiple layouts. Example: 16 LED strips can form a panel. Physical indices are offset so they don't overlap between layouts.

## What needs improvement

- Layout control changes must propagate to all layers (LUT rebuild) and driver groups (buffer reallocation). The mechanism is defined in architecture.md (Rebuild Propagation).
- Runtime add/remove/reorder of layouts.
