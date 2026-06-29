# Grid Layout

![GridLayout controls](../../../assets/screenshots/GridLayout.png)

Arranges lights in a 3D grid, row-major (x fastest, then y, then z). Full-density — every position maps to a light. Controls: `width`, `height`, `depth`, `serpentine`.

## Mapping

A plain grid (`serpentine` off) emits driver index `i` at box cell `i`, so the Layer takes the **1:1 unshuffled memcpy fast path** — the mapping isn't *declared* identity, it's *measured*: the Layer walks the coords once and only skips the mapping table when the order is natural. `serpentine` wires odd rows in reverse (boustrophedon — the strip snakes back and forth), so driver index `i` no longer equals box cell `i`: the grid is dense but **shuffled**, which routes it through the box→driver mapping LUT exactly as a sparse layout does. A handy lever for exercising both the identity and non-identity mapping paths from one layout. The Layer buffer and driver buffer are separate when memory allows (for parallelism), shared when memory is tight. `defaultGridSize` (16) is owned here and also read by the composition roots to size the boot grid.

## Tests

[Unit tests: GridLayout](../../../tests/unit-tests.md#gridlayout) — row-major coordinate iteration, 3D grids, Layouts multi-layout offset.

## Prior art

### MoonLight — L_MoonLight.h Panel ([source](https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h))

Panel layout with serpentine, multiple panel arrangements. Uses `addLight()` to define each position.

### projectMM v1 — GridLayout ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/modules/layouts/GridLayout.h))

Width/height/depth/serpentine controls. Mapping rebuilt in onUpdate(), parent notified via onChildrenReady().

### projectMM v2 — GridLayoutModule ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/GridLayoutModule.h))

Same controls. Uses LayoutModule base class.

## Source

[GridLayout.h](../../../../src/light/layouts/GridLayout.h)
