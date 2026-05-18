# EffectBase

Light-domain MoonModule subclass for effects. Adds rendering context.

## Purpose

Effects need to know their buffer, dimensions, and frame number, but
the MoonModule `loop()` interface has no parameters. EffectBase solves
this by providing a `RenderContext` that the Layer sets before calling
`loop()`.

## RenderContext

```
struct RenderContext {
    std::span<RGB> pixels;
    int16_t width, height, depth;
    uint32_t frame;
};
```

## Convenience Accessors

- `pixels()` — the buffer to write into
- `width()`, `height()`, `depth()` — logical dimensions
- `frame()` — current frame number

## What worked

- Clean separation: MoonModule stays domain-neutral, EffectBase adds
  light-domain context.
- Context set per-frame means effects always get current dimensions
  (important after mirror modifier changes logical size).

## What needs improvement

- The Layer does `dynamic_cast<EffectBase*>` to set the context. If
  an effect inherits from MoonModule but not EffectBase, the context
  isn't set and the effect gets stale/empty data. Should enforce
  that light effects always extend EffectBase.
- No access to coordinates or the LUT from effects. Effects compute
  x/y from pixel index (`i % width`). For non-grid layouts this is
  wrong — effects should receive actual coordinates.
