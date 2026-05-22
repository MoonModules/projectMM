# Glow Particles 2D Effect

Soft-glowing particles rendered as a metaball field. Particles move with independent velocities and bounce off the edges; the per-pixel field summation produces chaotic organic blobs — like `MetaballsEffect` with more freedom of movement.

## Controls

- `enabled` (bool) — from `EffectBase`
- `count` (uint8_t, default 5, range 1-8) — number of glow sources
- `speed` (uint8_t, default 60, range 1-255) — movement speed
- `radius` (uint8_t, default 24, range 4-64) — influence radius (larger = more merging)
- `hue_shift` (uint8_t, default 0, range 0-255) — global hue rotation

## Rendering

Position update uses 12.4 fixed-point arithmetic. Per pixel: `field += (radius² × 64) / (dx² + dy² + 1)` across all active particles. Brightness clamps to 255; hue derives from the field magnitude.

No heap allocations. Per-particle state: 8 bytes × 8 = 64 bytes.

## Tests

[Module test](../../../testing.md#stateless-effects) — non-zero output, spatial variation.
