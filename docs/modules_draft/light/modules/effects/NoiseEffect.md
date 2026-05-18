# Noise 2D Effect

2D Perlin noise field, animated over time.

## Controls

- `speed` (Uint16, default 1, range 1-255) — animation speed
- `scale` (Uint16, default 30, range 1-255) — noise zoom level

## Rendering

For each pixel at (x, y): computes `noise8(x*scale + frame*speed, y*scale)`,
maps noise value to hue via `RGB::fromHSV(n, 255, 255)`.

## Critical lesson: brightness

Initially used `fromHSV(n, 255, n)` which made most pixels nearly
black (noise value used as both hue AND brightness). The fix:
always use full brightness (v=255). Noise value should only control
hue or be mapped to a palette.

## What worked

- 2D noise field looks good with proper brightness.
- Scale control gives useful visual variation.
- Works with mirror modifier.

## What needs improvement

- The underlying noise implementation (Noise.h) is basic integer
  Perlin. Output distribution may cluster around certain values.
  Consider a better noise implementation or at least verify output
  distribution across the 0-255 range.
- No palette support — always maps noise to HSV rainbow.
- No 3D noise variant (could animate z-axis over time for richer
  movement).
