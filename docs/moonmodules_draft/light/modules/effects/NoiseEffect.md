# Noise 2D Effect

Smooth animated noise on the XY plane.

## Controls

- `scale` (slider, default 4, range 1-32) — spatial frequency (higher = finer detail)
- `speed` (slider, default 50, range 0-255) — animation speed (0 = frozen)

## Rendering

Uses value noise with bilinear interpolation and smoothstep. Time-based animation via elapsed millis, not frame count. Maps noise value to hue via `hsvToRgb(n, 200, 255)` — fixed saturation, full brightness.

## Lessons from v3 prototype

- Using noise value as brightness (`fromHSV(n, 255, n)`) made most lights nearly black. Always use full brightness (v=255) and vary hue or use a palette.
- Scale default of 30 in v3 prototype was too high for small grids. v1 used 4 with a 0.1x multiplier, giving effective scale of 0.4. Needs tuning based on grid size.

## Prior art

### MoonLight — E_MoonLight.h ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h))
Multiple noise effects (Noise2D, Noise3D variants). Uses FastLED noise functions. Time via `millis()`.

### projectMM v2 — Noise2DEffect ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/effects/Noise2DEffect.h))
Same hash-based value noise as v1. Uses PixelEffectBase spine.

### projectMM v1 — NoiseEffect2D ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/modules/effects/NoiseEffect2D.h))
Hash-based value noise with trilinear interpolation. Controls: scale (1-32), speed (0-255). Uses `timeMicros()` for animation.
v1 NoiseEffect2D used a hash-based value noise (platform-independent, no FastLED dependency). Hash function: `x * 1619 + y * 31337 + t * 6271` with bit mixing. Trilinear interpolation (bilinear in XY + linear in time). v3 prototype used Perlin noise instead — either approach works.
