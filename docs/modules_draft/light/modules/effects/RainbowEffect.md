# Rainbow 2D Effect

Diagonal rainbow pattern across a 2D grid, animated over time.

## Controls

- `speed` (Uint16, default 1, range 1-255) — animation speed

## Rendering

Uses `RGB::fromHSV(hue, 255, 255)` with hue derived from x+y
coordinates plus frame*speed. Always full brightness.

## What worked

- Clean 2D pattern using width() and height() from RenderContext.
- Integer-only math, no allocations in loop().
- Works correctly with mirror modifier (renders into reduced logical
  space, LUT replicates to all quadrants).

## What needs improvement

- Pattern is simple (diagonal lines). More interesting 2D rainbow
  patterns: radial, spiral, plasma.
- No palette support. Currently locked to full HSV rainbow.
