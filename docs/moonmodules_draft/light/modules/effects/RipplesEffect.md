# Ripples Effect

Expanding circular ripples from random centre points on the XY plane.

## Controls

- `speed` (slider, default 50, range 0-99) — ripple expansion speed
- `interval` (slider, default 128, range 1-254) — time between new ripple spawns

## Prior art

### projectMM v1 — RipplesEffect ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/modules/effects/RipplesEffect.h))
v1 RipplesEffect (commit 54b50bc). Spawns ripples at random positions, each expanding outward as a ring. Older ripples fade out. Multiple ripples can overlap.
Proven implementation with speed and interval controls.

### projectMM v2 — RipplesEffect ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/RipplesEffect.h))
Same effect, uses v2 module base.
