# Noise 2D Effect

Smooth animated noise on the XY plane.

## Controls

- `scale` (slider, default 4, range 1-32) — spatial frequency (higher = finer detail)
- `bpm` (slider, default 60, range 1-255) — animation speed in beats per minute

## Rendering

Uses value noise with bilinear interpolation and smoothstep. Maps noise value to hue via `hsvToRgb(n, 200, 255)` — fixed saturation, full brightness.

Animation: time scrolls the noise coordinate space (smooth drift). The scroll speed is scaled by panel width so the perceived speed is the same on any display size — a 16-wide and 128-wide panel look equally fast at the same BPM.

## Design notes

- The noise value drives hue, not brightness — lights render at full brightness (v=255) so the field stays visible. Driving brightness from the noise value leaves most lights near-black.
- `scale` defaults to 4 (low spatial frequency) so the pattern reads well on small grids; higher values suit larger panels.
- Time is applied as a coordinate offset into the noise field, not as a hash seed — this scrolls the field smoothly instead of producing random per-frame jumps.

## Tests

[Module test: NoiseEffect](../../../testing.md#noise) — non-zero output, spatial variation, differs from rainbow.

[Scenario: mirror](../../../testing.md#scenario-mirror) — full pipeline with noise + mirror, performance bounds.

## Prior art

### MoonLight — E_MoonLight.h ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h))
Multiple noise effects (Noise2D, Noise3D variants). Uses FastLED noise functions. Time via `millis()`.

### projectMM v2 — Noise2DEffect ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/effects/Noise2DEffect.h))
Same hash-based value noise as v1. Uses PixelEffectBase spine.

### projectMM v1 — NoiseEffect2D ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/modules/effects/NoiseEffect2D.h))
Hash-based value noise with trilinear interpolation. Controls: scale (1-32), speed (0-255). Uses `timeMicros()` for animation. v1 ran scale 4 with a 0.1x multiplier (effective 0.4) — v3's default of 4 is informed by this.
