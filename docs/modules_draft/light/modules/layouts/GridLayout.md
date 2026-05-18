# Grid Layout

Arranges lights in a 3D grid (row-major: x varies fastest, then y, then z).

## Controls

- `width` (Uint16, default 128, range 1-1024)
- `height` (Uint16, default 128, range 1-1024)
- `depth` (Uint16, default 1, range 1-64)

## Coordinate Iterator

Yields `(physicalIndex, x, y, z)` for each LED in row-major order.
`pixelCount() = width * height * depth`.

## What worked

- Simple, correct. Most common layout.
- `onChange` calls `markDirty()` to trigger LUT rebuild.
- Works as coordinate iterator — doesn't own LUT.

## What needs improvement

- Needs a LayoutBase adapter (GridAdapter) to work with LayoutGroup's
  virtual dispatch. This boilerplate should be eliminated.
- Default 128x128 is 16384 pixels → 97 ArtNet universes. This is a
  lot for testing. Consider smaller default or configurable presets.
- No serpentine wiring option (alternating row direction). Common in
  real LED matrices.
