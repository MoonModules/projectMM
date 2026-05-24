# BlendMap

Free function that reads from one or more Layer buffers and writes blended+mapped output into a destination buffer. Called by the Drivers container each frame.

## Operation

1. Clears destination buffer (memset to 0)
2. For each Layer in the Layers container:
   - For each logical light in the Layer's LUT:
     - Read source colour from the Layer's buffer
     - Look up physical destination(s) via LUT
     - Additively blend into destination (clamp to 255)

## What worked

- Correctly handles 1:0 (skip), 1:1 (shuffled and unshuffled), 1:N (mirror) mappings.
- Additive blending with clamping works for multiple layers.
- Bounds checking on physical indices prevents buffer overflows.

## What needs improvement

- Only additive blending. Need configurable blend modes per layer.
- Clears entire destination buffer every frame. For 16K+ lights this is ~50KB memset. Could track dirty regions instead.
- The function is `inline` in a header. For large installations this might cause code bloat if included in multiple translation units.
