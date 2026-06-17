# WheelLayout

A bicycle-wheel arrangement: a number of straight spokes radiate from a centre hub, each carrying a row of LEDs spaced one unit apart from the centre outward. Spoke *k* points at angle *k / spokes* of a full turn; the LEDs along it sit at increasing radius.

## Controls

- `spokes` — number of spokes (2–64, default 8).
- `ledsPerSpoke` — LEDs along each spoke (1–256, default 10).

`lightCount()` = `spokes × ledsPerSpoke`.

## Coordinate iterator

`forEachCoord` emits `(index, x, y, 0)` for each LED, walking spoke by spoke. A spoke's angle is `spoke · 256 / spokes` in `uint8_t` turn units; the LED at radius *r* sits at `(maxR + r·cos, maxR + r·sin)`, where cos/sin come from the project's [`cos8`/`sin8`](../../core/Control.md) integer LUT (signed component `val − 128`, divided back by 128). The whole wheel is shifted by `+ledsPerSpoke` so every coordinate is ≥ 0 within a `(2·ledsPerSpoke)`-wide bounding box. Integer-only (same discipline as [SphereLayout](SphereLayout.md)) — no `double` cos/sin/round.

## Prior art

- **MoonLight — ring/spoke layouts** (L_MoonLight.h): Ring, Rings241, and other radial layouts.
- **projectMM v2 — WheelLayoutModule** — spoke-based coordinate generation; that used `double` trig, this is the integer-LUT equivalent.

## Source

[WheelLayout.h](../../../../src/light/layouts/WheelLayout.h)
