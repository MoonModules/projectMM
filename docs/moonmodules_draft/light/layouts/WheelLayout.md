# Wheel Layout

Arranges lights along spokes of a wheel (bicycle wheel pattern).

## Controls

- `spokes` (Uint16, default 8, range 2-64) — number of spokes
- `ledsPerSpoke` (Uint16, default 10, range 1-256) — LEDs per spoke

## Coordinate Iterator

Yields `(physicalIndex, x, y, 0)` for each LED on each spoke.
Positions are computed using trigonometry (cos/sin). LEDs radiate
from center outward along each spoke.

`lightCount() = spokes * ledsPerSpoke`.

## What worked

- Coordinate computation is correct — spokes are evenly distributed.
- Different spoke counts produce different angular distributions.

## What needs improvement

- Uses `double` math (cos, sin, round) for coordinate computation.
  Not a hot-path issue (only runs during LUT build) but could use
  integer approximations for ESP32 without FPU.
- `gridSize()` returns the bounding box but isn't used anywhere.
- No inner radius control (all spokes start from center). Real wheels
  might have a hub.
- Same LayoutBase adapter boilerplate as GridLayout.

## Prior art

### MoonLight — various ring/spoke layouts ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h))
Ring, Rings241, and other circular/radial layout types.

### projectMM v2 — WheelLayoutModule ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/layouts/WheelLayoutModule.h))
Wheel layout with spoke-based coordinate generation. Uses LayoutModule base.
