# EffectBase

Light-domain MoonModule subclass for effects. Adds rendering context.

## Purpose

Effects need to know their buffer, dimensions, and elapsed time, but the MoonModule `loop()` interface has no parameters. EffectBase solves this by providing a `RenderContext` that the Layer sets before calling `loop()`.

## RenderContext

- `lights()` — the buffer to write into (span of light values)
- `width()`, `height()`, `depth()` — logical dimensions
- `elapsed()` — milliseconds since startup (for frame-rate independent animation)

## What worked

- Clean separation: MoonModule stays domain-neutral, EffectBase adds light-domain context.
- Context set per-frame means effects always get current dimensions.

## What needs improvement

- Use elapsed time (millis), not frame count, for animation (architecture decision).
- Follow v2's PixelEffectBase pattern: concrete effect implements only controls + render. Base handles everything else.
- Effects compute x/y from light index (`i % width`). The layer provides the logical grid dimensions for this.

## Prior art

### MoonLight — Node + VirtualLayer ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonBase/Nodes.h), [source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Layers/VirtualLayer.h))
- Effects access `layer->width()`, `layer->height()`, `layer->depth()` directly via the VirtualLayer pointer.
- Buffer access via `layer->virtualChannels` (raw byte array indexed by `channelsPerLight`).
- Time via `timeMicros()` — microsecond precision, used for animation.

### projectMM v2 — PixelEffectBase ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/effects/PixelEffectBase.h))
- Shared spine: concrete effect implements only `build_effect_controls()` + `render_(px, w, h, d)`.
- Layout resolution, buffer management, DataBuffer declaration/teardown, resize polling all in the base.
- Eliminates ~70 lines of boilerplate per effect.

### projectMM v1 — ProducerModule ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/core/ProducerModule.h))
Base for effects. Produces into a Channel (pixel buffer). Effects access layer dimensions via the EffectsLayer pointer.
