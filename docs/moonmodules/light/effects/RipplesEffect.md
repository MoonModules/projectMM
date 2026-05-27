# Ripples 2D Effect

![RipplesEffect controls](../../../assets/screenshots/RipplesEffect.png)

![RipplesEffect preview](../../../assets/screenshots/RipplesEffect.gif)

Expanding concentric rings from random centre points. Each ripple grows outward and respawns once it has expanded past the visible area. Multiple ripples overlap with additive blending.

## Controls

- `enabled` (bool) — from `EffectBase`
- `count` (uint8_t, default 4, range 1-8) — number of simultaneously active ripples
- `speed` (uint8_t, default 60, range 1-255) — expansion rate
- `thickness` (uint8_t, default 3, range 1-16) — ring thickness in pixels
- `hue_shift` (uint8_t, default 0, range 0-255) — global hue rotation

## Rendering

Per pixel, for each ripple: octagonal distance `dist8(dx, dy)` is compared to the ripple's current radius. Pixels within `thickness` of the ring get an intensity falloff plus an age-based fade so old, wide ripples disappear softly.

No heap allocations. Per-ripple state: position + radius + hue (~40 bytes total for 8 ripples).

## Tests

[Module test](../../../testing.md#stateless-effects) — non-zero output, spatial variation.
