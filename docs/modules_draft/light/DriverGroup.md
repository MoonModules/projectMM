# DriverGroup

Groups output drivers. Owns the shared output buffer. Performs
blend+map from all layers into the output buffer each frame.

## Pipeline

1. `blendMap()` — reads all layers' buffers, applies their LUTs,
   additively blends into the shared output buffer
2. Passes output buffer (read-only span) to each driver
3. Each driver reads from the output buffer and pushes to hardware

## Why the shared output buffer is needed

BlendMap writes to arbitrary physical positions (via LUT). The output
is not filled sequentially — pixel at physical position 16383 might be
written before position 0. A driver cannot read chunk-by-chunk until
the full buffer is populated.

## What worked

- Shared output buffer with blendMap is correct and complete.
- Additive blending with clamp to 255 works.
- Passing read-only span to drivers prevents accidental modification.

## What needs improvement

- Only additive blending. Need per-layer blend modes (alpha, multiply,
  screen, etc.).
- `blendMap` clears the entire output buffer each frame (memset). For
  large buffers (16K+ pixels) this is measurable. Could use dirty
  tracking instead.
- No way to assign specific layers to specific drivers (e.g. driver A
  gets layers 0-1, driver B gets layer 2). All drivers see all layers.
