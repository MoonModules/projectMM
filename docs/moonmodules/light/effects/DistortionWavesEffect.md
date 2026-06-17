# DistortionWavesEffect

Two interfering sine waves whose sum drives the hue — a flowing, moiré-like colour field. A horizontal and a vertical wave run at independent frequencies and slightly different time rates, so they beat against each other and the pattern drifts. 2D ([Layer](../Layer.md) extrudes it onto a 3D grid). Ported from WLED's "Distortion Waves".

## Controls

- `freq_x` — horizontal wave frequency (1–8, default 3).
- `freq_y` — vertical wave frequency (1–8, default 3).
- `speed` — animation speed (0–100, default 50; `0` = frozen).

## Rendering

For a light at (x, y): `hue = (sin8(x·freq_x + t) + sin8(y·freq_y + t_y)) / 2`, where `t` is the time phase and `t_y ≈ 1.3·t` (WLED runs the vertical wave's time faster; approximated as `(t·333) >> 8` to stay integer). The hue feeds `hsvToRgb(hue, 240, 255)`, keeping WLED's slightly-desaturated look. Integer-only: angles are `uint8_t` and the [`sin8`](../../core/Control.md) LUT replaces `sinf`.

## Prior art

- **MoonLight — E_WLED.h** — the WLED port of Distortion Waves.
- **projectMM v1 / v2 — DistortionWaves** — same two-interfering-sines algorithm; those used float `sinf`, this is the integer-`sin8` equivalent.

## Source

[DistortionWavesEffect.h](../../../../src/light/effects/DistortionWavesEffect.h)
