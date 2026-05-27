# Plasma Palette 2D Effect

![PlasmaPaletteEffect controls](../../../assets/screenshots/PlasmaPaletteEffect.png)

![PlasmaPaletteEffect preview](../../../assets/screenshots/PlasmaPaletteEffect.gif)

Same four-sine plasma field as `PlasmaEffect`, but colours come from a 256-entry fire-ocean RGB palette in flash instead of `hsvToRgb`.

## Controls

- `enabled` (bool) — from `EffectBase`
- `bpm` (uint8_t, default 30, range 1-255)
- `scale_x` (uint8_t, default 16, range 1-64)
- `scale_y` (uint8_t, default 16, range 1-64)

## Rendering

Plasma index `((s1+s2+s3+s4)>>2)` maps through `palette_[256]` (768 bytes flash). `dynamicBytes()` = 0.

## Tests

[Module test](../../../testing.md#stateless-effects) — non-zero output, spatial variation.
