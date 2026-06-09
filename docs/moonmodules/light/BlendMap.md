# BlendMap

`blendMap` is a free function that reads a Layer's buffer and writes mapped output into a destination buffer, called by the Drivers container each frame. The destination is cleared first, so physical cells with no source (a sparse layout's lattice gaps) stay black.

The LUT picks one of two paths: when each physical light is written at most once (every current layout/modifier — mirror, shuffle, sparse box→driver), it overwrite-copies source→destination (no read-back, no clamp, ~4× faster); when sources can overlap a destination (multi-layer composition), it additively blends with clamping. Physical indices are bounds-checked so an out-of-range LUT entry can't overrun the buffer.

Configurable per-layer blend modes (beyond additive) land with multi-layer composition — a [backlog](../../backlog/backlog.md) item.

## Source

[BlendMap.h](../../../src/light/layers/BlendMap.h)
