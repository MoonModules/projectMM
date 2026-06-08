# Plasma Effect

![PlasmaEffect controls](../../../assets/screenshots/PlasmaEffect.png)

![PlasmaEffect preview](../../../assets/screenshots/PlasmaEffect.gif)

Animated plasma pattern from summed sine waves on orthogonal and diagonal axes. Default effect in the desktop and ESP32 pipeline. 2D on flat (`depth == 1`) layouts, 3D on volumetric (`depth > 1`) layouts so a cube renders as a varied volume.

## Controls

- `bpm` (uint8_t, default 30, range 1-255) — animation speed in beats per minute
- `scale_x` (uint8_t, default 16, range 1-64) — horizontal wave length in grid cells (`step = 256 / scale_x`)
- `scale_y` (uint8_t, default 16, range 1-64) — vertical wave length in grid cells
- `hue_shift` (uint8_t, default 0, range 0-255) — rotates the entire color wheel

## Design notes

Sums orthogonal + diagonal `sin8` waves (256-byte LUT, no float, no heap), adding a fifth depth sine on `depth > 1`. `hue_shift` rotates the result before `hsvToRgb`. A phase accumulator (matching NoiseEffect) means a `bpm` change doesn't jump the animation. The default effect in both pipelines, paired with MultiplyModifier (see `src/main.cpp`).

## Tests

[Unit tests: PlasmaEffect](../../../tests/unit-tests.md#plasmaeffect) — non-zero output, spatial variation, differs from NoiseEffect.

Default pipeline uses Plasma + MultiplyModifier (see `src/main.cpp`).

## Prior art

Classic demoscene plasma effect (sum of sines). Integer sin8 LUT approach matches FastLED-style tables. No direct v1/v2 module port — simpler than NoiseEffect (no hash/bilinear).

## Source

[PlasmaEffect.h](../../../../src/light/effects/PlasmaEffect.h)
