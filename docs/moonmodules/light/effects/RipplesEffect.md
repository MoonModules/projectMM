# Ripples 2D Effect

![RipplesEffect controls](../../../assets/screenshots/RipplesEffect.png)

![RipplesEffect preview](../../../assets/screenshots/RipplesEffect.gif)

Expanding concentric rings from random centre points. Each ripple grows outward and respawns once it has expanded past the visible area. Multiple ripples overlap with additive blending.

## Controls

- `count` (uint8_t, default 4, range 1-8) — number of simultaneously active ripples
- `speed` (uint8_t, default 60, range 1-255) — expansion rate
- `thickness` (uint8_t, default 3, range 1-16) — ring thickness in pixels
- `hue_shift` (uint8_t, default 0, range 0-255) — global hue rotation

An age-based fade makes old, wide ripples disappear softly. Per-ripple state (position + radius + hue) lives in a fixed array — no heap.

## Tests

[Unit tests: CheckerboardEffect](../../../tests/unit-tests.md#checkerboardeffect) — shared rendering/smoke coverage: non-zero output, spatial variation. (RipplesEffect carries per-ripple mutable state — position, radius, hue — with random respawn; that behaviour isn't unit-tested today.)

## Source

[RipplesEffect.h](../../../../src/light/effects/RipplesEffect.h)
