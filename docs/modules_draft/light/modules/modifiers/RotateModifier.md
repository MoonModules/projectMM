# Rotate Modifier

Dynamic modifier. Rotates the 2D pixel buffer around its centre each
frame. Has per-pixel cost on the hot path.

## Controls

- `speed` (Uint16, default 1, range 1-255) — rotation speed

## Implementation

- Allocates a temporary buffer each frame (via platform::alloc)
- Computes rotation angle from frame * speed
- For each destination pixel, samples from rotated source position
  using inverse rotation (cos/sin)
- Copies result back to original buffer

## Status

Not working correctly in testing. Needs debugging.

## What needs improvement

- **Allocates on hot path**. The temp buffer is allocated and freed
  every frame via platform::alloc. This violates the zero-alloc hot
  path rule. Should pre-allocate in setup or use a persistent scratch
  buffer.
- **Uses floating point** (cos, sin). On ESP32 without FPU this is
  slow. Consider integer rotation using fixed-point math or lookup
  tables.
- **Nearest-neighbor sampling**. The inverse rotation uses truncation
  to find source pixel. This produces visible aliasing. Bilinear
  interpolation would look better but costs more.
- **2D only**. Doesn't handle 3D rotation. For 3D, would need rotation
  matrices around arbitrary axes.
- **Layer::render is coupled** to RotateModifier via dynamic_cast.
  Should use a generic dynamic modifier interface (e.g.
  `transformPixels(span, frame, w, h, d)` as a virtual method on a
  modifier base class).
- **No interpolation of angle** between frames. At low frame rates,
  the rotation appears to jump. Should use time-based angle, not
  frame-based.
