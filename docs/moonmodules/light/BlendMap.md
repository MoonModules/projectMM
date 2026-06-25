# BlendMap

`blendMap` is a free function that reads a Layer's buffer and writes mapped output into a destination buffer, called by the [Drivers](Drivers.md) container for each enabled layer each frame. It takes a blend op (`Overwrite` / `Alpha` / `Additive`), an `opacity` (0–255), and a `clearFirst` flag — the bottom (first-composited) layer passes `clearFirst=true` so physical cells with no source (a sparse layout's lattice gaps) stay black; layers above pass `false` to accumulate onto the frame below.

The combine math is integer-only (the hot-path per-light rule), with one tight specialised loop per op chosen once per layer:

- **Overwrite** (the default / bottom layer): plain copy, no read-back. For a dense grid (no LUT) it's a `memcpy`; for a single-write LUT (mirror, shuffle, sparse box→driver) it copies source→destination per mapped light. A non-overwriting LUT (one that folds several logical lights onto one physical cell) routes through the additive accumulate path so the overlaps sum-with-clamp rather than last-writer-win.
- **Additive**: `dst = clamp(dst + src·opacity)` — sum with saturation at 255, opacity scaling the source.
- **Alpha** (over): `dst = (src·α + dst·(255−α)) / 255` — the textbook 8-bit alpha-over, division by 255 via the fast `(x + (x>>8) + 1) >> 8` reciprocal. Full opacity (255) collapses to a plain overwrite (no blend cost).

A dense-grid layer has no LUT, so its buffer blends 1:1 (source index = physical index, no lookup); a layer with a LUT maps each logical light to its physical destination(s) first. Physical indices come from the LUT, which is built in-range from the shared Layouts, so they address the buffer in bounds by construction. The per-Layer `blendMode`/`opacity` controls that select the op live on [Layer](Layer.md#blendmode--opacity-controls); Drivers reads them and the layer stack order.

## Source

[BlendMap.h](../../../src/light/layers/BlendMap.h)
