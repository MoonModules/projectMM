# LayoutGroup

Groups layouts. Shared across all layers and drivers. Defines the physical light topology.

## API

- `addLayout(layout)` — add a layout. No hard-coded max — use dynamic list (heap-allocated).
- `totalLightCount()` — sum of all layouts' light counts. Provided to layers and driver groups for buffer allocation.
- `forEachCoord(callback, ctx)` — iterate all coordinates across all layouts, offsetting physical indices so they don't overlap.

`forEachCoord` is a LayoutGroup function, not a Layer function. The layer USES the LayoutGroup's coordinates to build its LUT, but the iteration itself is owned by the LayoutGroup.

## Layout interface

Layouts implement the virtual interface directly — no adapter or wrapper boilerplate. `forEachCoord` is a virtual method, not a template.

- `lightCount()` — number of lights this layout defines
- `forEachCoord(callback, ctx)` — yield (idx, x, y, z) for each light

Callback signature uses the platform typedefs: `void(void* ctx, nrOfLightsType idx, lengthType x, lengthType y, lengthType z)`

## Multiple layouts combined

A LayoutGroup can contain multiple layouts. Example: 16 LED strips can form a panel. Physical indices are offset so they don't overlap between layouts.

## What needs improvement

- Layout control changes must propagate to all layers (LUT rebuild) and driver groups (buffer reallocation). The mechanism is defined in architecture.md (Rebuild Propagation).
- Runtime add/remove/reorder of layouts.
