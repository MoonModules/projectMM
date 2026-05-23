# Layouts

Top-level container for one or more layouts. Shared by every layer in the Layers container — defines the physical light topology of the installation.

> **Naming convention.** Capital `Layouts` is the container class; lowercase "layout"/"layouts" is the English singular/plural for individual `LayoutBase` children. Capitalisation disambiguates "the Layouts container" from "two layouts stacked". Same rule for `Layers`/layer and `Drivers`/driver.

## API

- `addChild(layout)` — add a layout (heap-allocated list, grows on demand).
- `totalLightCount()` — sum of every layout's light count. Read by Layer and by the Drivers container for buffer allocation.
- `forEachCoord(callback, ctx)` — iterate all coordinates across every layout, offsetting physical indices so they don't overlap.

`forEachCoord` is a Layouts method, not a Layer method. A layer **uses** the Layouts container's coordinates to build its LUT, but the iteration itself is owned by Layouts.

## Layout interface

Layouts inherit from `LayoutBase` and implement the virtual interface directly — no adapter or wrapper boilerplate. `forEachCoord` is a virtual method, not a template.

- `lightCount()` — number of lights this layout defines.
- `forEachCoord(callback, ctx)` — yield (idx, x, y, z) for each light.

Callback signature uses the platform typedefs: `void(void* ctx, nrOfLightsType idx, lengthType x, lengthType y, lengthType z)`.

## Multiple layouts combined

A Layouts container can hold multiple layouts. Example: 16 LED strips making up a panel. Physical indices are offset so they don't overlap between layouts.

## What needs improvement

- Layout control changes must propagate to every layer (LUT rebuild) and to the Drivers container (output buffer reallocation). Mechanism: [architecture.md § Rebuild Propagation](../../architecture.md#rebuild-propagation).
- Runtime add / remove / reorder of layouts.
