# Grid Layout

Arranges lights in a 3D grid (row-major: x varies fastest, then y, then z). Full-density mapping — every position maps to a light.

## Controls

- `width` (slider, default 16, range 1-1024)
- `height` (slider, default 16, range 1-1024)
- `depth` (slider, default 1, range 1-32)
- `serpentine` (toggle, default false) — odd rows (y%2==1) run right-to-left so the physical strip snakes continuously without gaps at row turns. Common in real LED matrices.

## Coordinate Iterator

Yields `(physicalIndex, x, y, z)` for each light in row-major order. With serpentine enabled, the mapping reverses x within odd rows: `if (serpentine && y % 2 == 1) x = width - 1 - x`.

## Edge cases

- Width or height = 0: prevented by min=1 on controls.
- Serpentine only affects x-direction. Vertical serpentine would need a separate option.
- Very large grids may exceed available memory for LUT allocation.

## Prior art

### MoonLight — L_MoonLight.h Panel ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h))
Panel layout with serpentine, multiple panel arrangements. Uses `addLight()` to define each position.

### projectMM v1 — GridLayout ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/modules/layouts/GridLayout.h))
Width/height/depth/serpentine controls. Mapping rebuilt in onUpdate(), parent notified via onChildrenReady().
v1 GridLayout (commit 54b50bc) had identical controls including serpentine. Mapping table rebuilt in `onUpdate()`, parent notified via `onChildrenReady()`.

### projectMM v2 — GridLayoutModule ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/GridLayoutModule.h))
Same controls. Uses LayoutModule base class ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/layouts/LayoutModule.h)).
