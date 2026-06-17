# RotateModifier

A **dynamic modifier** that rotates the 2D image around its centre, turning continuously over time. Like [RandomMapModifier](RandomMapModifier.md), the rotation is a coordinate remap baked into the [Layer](../Layer.md)'s LUT; the angle advances on a `speed` timer and the LUT rebuilds when the angle crosses to a new step — not every frame.

## Controls

- `speed` — rotation speed (1–255, default 1). Higher turns faster and rebuilds the LUT more often (still bounded — a rebuild fires only on an angle-step change, not per frame).

## How it works

The angle is a `uint8_t` (256 steps per turn). Each destination light at (dx, dy) from the centre samples the **source** at the inverse rotation — `sx = dx·cosθ + dy·sinθ`, `sy = −dx·sinθ + dy·cosθ` — using the project's [`cos8`/`sin8`](../../core/Control.md) integer LUT (signed component `val − 128`, divided back by 128), with nearest-neighbour sampling (no float, no bilinear). A source that falls outside the grid is dropped (that light goes dark at this angle), the same `outCount=0` path [CheckerboardModifier](CheckerboardModifier.md) uses. The per-frame tick (`loop()`, the dynamic-modifier hook also used by RandomMapModifier) advances the angle and, on a step change, calls the Layer's `onBuildState()` to rebuild the LUT — no per-frame allocation, no `Layer::render` coupling.

2D only: the z axis passes through unchanged.

## Prior art

- **MoonLight — M_MoonLight.h Rotate / PinWheel** — a per-light `modifyXYZ()` coordinate transform on the hot path. This version carries the transform in the LUT instead, reusing the dynamic-modifier `loop()` hook and the existing rebuild path rather than a render-time transform.

## Source

[RotateModifier.h](../../../../src/light/modifiers/RotateModifier.h)
