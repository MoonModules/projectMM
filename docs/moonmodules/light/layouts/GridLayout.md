# Grid Layout

![GridLayout controls](../../../assets/screenshots/GridLayout.png)

Arranges lights in a 3D grid, row-major (x fastest, then y, then z). Full-density — every position maps to a light. Controls: `width`, `height`, `depth`.

## Mapping

Default settings (no serpentine, X-then-Y) are **1:1 unshuffled** — the `oneToOneMapping` flag is set and the mapping table skipped entirely. The Layer buffer and driver buffer are separate when memory allows (for parallelism), shared when memory is tight. `defaultGridSize` (16) is owned here and also read by the composition roots to size the boot grid.

## Tests

[Unit tests: GridLayout](../../../tests/unit-tests.md#gridlayout) — row-major coordinate iteration, 3D grids, Layouts multi-layout offset.

## Prior art

### MoonLight — L_MoonLight.h Panel ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h))

Panel layout with serpentine, multiple panel arrangements. Uses `addLight()` to define each position.

### projectMM v1 — GridLayout ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/modules/layouts/GridLayout.h))

Width/height/depth/serpentine controls. Mapping rebuilt in onUpdate(), parent notified via onChildrenReady().

### projectMM v2 — GridLayoutModule ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/GridLayoutModule.h))

Same controls. Uses LayoutModule base class.

## Source

[GridLayout.h](../../../../src/light/layouts/GridLayout.h)
