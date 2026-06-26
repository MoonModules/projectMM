# RotateModifier

A **dynamic modifier** that rotates the 2D image around its centre, turning continuously over time. The one modifier that overrides `modifyLive` (per-frame, no mapping rebuild) — so the rotation is smooth, and the Layer runs its live pass only because this modifier is present (a static-only chain pays nothing per frame; see [ModifierBase](../ModifierBase.md)). Also the codebase's **transform-matrix reference**.

## Controls

- `speed` — rotation speed (1–255, default 1). `loop()` advances the angle on the timer; `modifyLive` applies it on the next frame (no rebuild).

## How it works

The angle is a `uint8_t` (256 steps per turn). `modifyLive` is a **backward** map: for each destination cell it computes the **source** it samples, via the inverse rotation `R(-θ)` written as an explicit integer 2×2 matrix `[[c, s], [-s, c]]` applied to the centred coordinate. `c`/`s` come from the project's [`cos8`/`sin8`](../../core/Control.md) integer LUT (signed component `val − 128`, scaled by 128; the `>>7` divides back out), nearest-neighbour (no float). A source outside the box leaves that destination dark — the Layer's live pass clears it. Rotation is the canonical affine transform, which is why it's the matrix example; the non-affine modifiers (mask, tile) fold directly instead.

2D only: the z axis passes through unchanged.

## Prior art

- **MoonLight — M_MoonLight.h Rotate / PinWheel** — a per-light `modifyXYZ()` coordinate transform. Our `modifyLive` is the same per-frame hook; we carry an explicit rotation matrix.

## Source

[RotateModifier.h](../../../../src/light/modifiers/RotateModifier.h)
