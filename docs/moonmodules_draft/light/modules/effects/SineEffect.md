# Sine Effect

3D sine wave pattern. R/G/B channels respond to x/y/z axes respectively, creating a colorful 3D wave.

## Controls

- `frequency` (slider, default 1, range 1-20) — wave frequency
- `amplitude` (slider, default 255, range 0-255) — brightness (DMX-aligned, 255 = full)

## Rendering

For each light at (x, y, z):
- R = amplitude * (sin(freq * (x + tick)) + 1) / 2
- G = amplitude * (sin(freq * (y + tick) + 2π/3) + 1) / 2
- B = amplitude * (sin(freq * (z + tick) + 4π/3) + 1) / 2

True 3D effect — uses all three axes. Time-based animation.

## Prior art

### projectMM v2 — SineEffect ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/effects/SineEffect.h))
Same 3D sine algorithm. Uses PixelEffectBase spine. Published brightness to KvStore for inter-module communication.

### projectMM v1 — SineEffect ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/modules/effects/SineEffect.h))
3D sine wave with frequency/amplitude controls. Published brightness to KvStore.
v1 SineEffect (commit 54b50bc). Also published `brightness` as a normalized float (0-1) via KvStore for downstream modules — a simple inter-module communication pattern.
