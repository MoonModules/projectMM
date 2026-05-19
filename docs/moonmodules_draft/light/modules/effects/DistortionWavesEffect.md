# Distortion Waves 2D Effect

Two interfering sine waves mapped to hue. Ported from WLED.

## Controls

- `freq_x` (slider, default 3, range 1-8) — horizontal wave frequency
- `freq_y` (slider, default 3, range 1-8) — vertical wave frequency
- `speed` (slider, default 50, range 0-100) — animation speed (0 = frozen)

## Rendering

For each light at (x, y): `val = sin(x * freq_x * 2π/W + t) + sin(y * freq_y * 2π/H + t * 1.3)`. val ∈ [-2, 2] mapped to hue ∈ [0, 255]. Uses `hsvToRgb(hue, 240, 255)`.

Time-based animation via elapsed millis.

## Prior art

### MoonLight — E_WLED.h ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h))
Original WLED port of distortion waves among many WLED effects.

### projectMM v2 — DistortionWavesEffect ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/effects/DistortionWavesEffect.h))
Uses PixelEffectBase spine. Same algorithm as v1.

### projectMM v1 — DistortionWaves2DEffect ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/modules/effects/DistortionWaves2DEffect.h))
Two interfering sine waves. Uses `sinf()`. Controls: freq_x, freq_y, speed.
v1 DistortionWaves2DEffect (commit 54b50bc). Uses `sinf()` — floating point, but only on cold-ish path (per-light per-frame). Acceptable on ESP32 with FPU (LX7) but consider integer approximation for LX6.
