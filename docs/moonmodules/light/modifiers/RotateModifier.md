# RotateModifier

A **dynamic modifier** that rotates the 2D image around its centre, turning continuously over time. The one modifier that overrides `modifyLive` (per-frame, no mapping rebuild) — so the rotation is smooth, and the Layer runs its live pass only because this modifier is present (a static-only chain pays nothing per frame; see [ModifierBase](../ModifierBase.md)). Also the codebase's **transform-matrix reference**.

## Controls

- `speed` — rotation speed (1–255, default 1). `loop()` advances the angle on the timer; `modifyLive` applies it on the next frame (no rebuild).

## How it works

`loop()` advances the angle on the `speed` timer; rotation is applied each frame in the Layer's live pass (`modifyLive`), not baked into the mapping — so a `speed` change is a cheap live edit, no rebuild. A source that rotates outside the box leaves that destination dark. 2D only: the z axis passes through. The integer 2×2-matrix backward map is in the header.

## Prior art

- **MoonLight — M_MoonLight.h Rotate / PinWheel** — a per-light `modifyXYZ()` coordinate transform. Our `modifyLive` is the same per-frame hook; we carry an explicit rotation matrix.

## Source

[RotateModifier.h](../../../../src/light/modifiers/RotateModifier.h)
