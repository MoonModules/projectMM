# EffectBase

Light-domain MoonModule subclass for effects. Adds rendering context.

## Design

`EffectBase` is a zero-state convenience layer: it holds no data of its own, just accessors (`buffer()`, `width()`, `height()`, `depth()`, `elapsed()`, …) that forward to the parent `Layer`. An effect reads its rendering context through these instead of caching a `Layer*` and the dimensions itself. `DriverBase` plays the same role for drivers against `Drivers`.

## Animation guidelines

Effects use **BPM** (beats per minute) for speed controls, not abstract 0-255 ranges. This gives users a musical reference: 60 BPM = one beat per second, 120 BPM = two beats per second. Default is 60 BPM.

Animation must be **resolution-independent**: multiply the time offset by the panel dimension (width or height) so the perceived visual speed is the same regardless of display size. Without this, large displays look sluggish and small displays look frantic at the same BPM.

Animation is driven by **elapsed millis**, not frame count. This ensures consistent speed regardless of FPS. The speed slider controls the animation dynamics, never the framerate — FPS should always be maximal for smooth motion.

## Dimensions and auto-extrusion

`dimensions()` (D3 default; the `.h` documents the per-axis contract) is a claim about which axes the effect *iterates*, not what the layer has — so `loop()` must read `width()`/`height()`/`depth()` at frame time and never hardcode a bound. The `dim` int (1/2/3) is emitted in `/api/types`; the UI derives the 📏/🟦/🧊 chip from it, so it isn't repeated in each module's `tags()`. See [architecture.md § Effects](../../architecture.md#effects) for the live declaration per shipped effect, and `unit_Layer_extrude.cpp` for the pinned contract tests.

## Prior art

### MoonLight — Node + VirtualLayer ([source](https://github.com/ewowi/MoonLight/blob/main/src/MoonBase/Nodes.h), [source](https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Layers/VirtualLayer.h))

- Effects access `layer->width()`, `layer->height()`, `layer->depth()` directly via the VirtualLayer pointer. No separate EffectBase.
- Buffer access via `layer->virtualChannels` (raw byte array).
- Time via `timeMicros()`.

### projectMM v1 — ProducerModule ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/core/ProducerModule.h))

Base for effects. Produces into a Channel.

### projectMM v2 — PixelEffectBase ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/effects/PixelEffectBase.h))

Shared spine: concrete effect implements only `build_effect_controls()` + `render_(px, w, h, d)`. Eliminates ~70 lines boilerplate.

## Source

[EffectBase.h](../../../src/light/effects/EffectBase.h)
