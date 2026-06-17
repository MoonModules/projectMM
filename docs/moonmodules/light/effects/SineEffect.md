# SineEffect

A 3D colour sine field: the red, green, and blue channels each follow a sine wave along one axis (x, y, z) with a 120° phase offset between channels, so the box glows through shifting colours that scroll over time. Every axis drives a channel, so it is a true 3D effect; on a 2D grid the z term is constant ([Layer](../Layer.md) extrudes a lower-dimensional effect across the unused axis).

## Controls

- `frequency` — spatial frequency, how many wave cycles span the box (1–20, default 1).
- `amplitude` — peak brightness, DMX-aligned (0–255, default 255 = full).
- `bpm` — scroll speed of the wave over time (1–255, default 30).

## Rendering

For a light at (x, y, z) the channel value is `sin8(coord·frequency + t + phase) · amplitude / 255`, where `t` is the time phase and `phase` is 0 / 85 / 170 (the 0° / 120° / 240° channel offsets in `uint8_t` angle units, 256 = full turn). All integer: angles are bytes and the project's [`sin8`](../../core/Control.md) LUT replaces `sinf`, so there is no per-light float.

## Prior art

- **projectMM v1 / v2 — SineEffect** — same 3D sine algorithm. Those used float `sinf` and published a normalized brightness via a KvStore for inter-module communication; this version is integer-only (`sin8`) and carries no KvStore.

## Source

[SineEffect.h](../../../../src/light/effects/SineEffect.h)
