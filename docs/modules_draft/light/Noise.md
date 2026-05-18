# Noise

Integer Perlin noise implementation. Header-only with static permutation
table.

## API

- `noise8(x, y)` — 2D noise, returns 0-255
- `noise8(x, y, z)` — 3D noise, returns 0-255
- `noise16(x, y)` — 16-bit precision (shifts noise8 result left 8)
- `noise16(x, y, z)` — 3D 16-bit variant

## Implementation

Standard Perlin noise with Ken Perlin's permutation table (doubled to
512 entries to avoid wrapping). Uses integer gradients and lerp.

Input coordinates are split into integer part (>>8) and fractional part
(&0xFF). Each noise "cell" is 256 units wide in input space.

## What worked

- Deterministic: same inputs always produce same output.
- Integer-only math on hot path.
- 2D and 3D variants both work.

## What needs improvement

- Output distribution may not be uniform across 0-255. The lerp and
  gradient math can produce values clustering around 128.
- `noise16` is just `noise8 << 8` — not true 16-bit precision.
- No octave/fractal noise (multiple noise calls at different scales
  summed together). Would be useful for organic patterns.
- The 512-byte permutation table is `inline constexpr` — each
  translation unit that includes the header gets a copy. On ESP32
  this wastes flash. Consider `extern const` with a .cpp definition.
