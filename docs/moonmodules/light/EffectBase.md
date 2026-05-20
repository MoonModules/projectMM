# EffectBase

Light-domain MoonModule subclass for effects. Adds rendering context.

## Design questions

**Does EffectBase need to exist?** The Layer already knows the buffer, dimensions, and elapsed time. Consider having the Layer provide the rendering context directly to effects via their `loop()` call, eliminating EffectBase. The effect would access `layer->buffer()`, `layer->width()`, etc. — exactly what MoonLight does. Pro: smaller codebase, fewer files, cleaner separation of concerns — the layer IS the context. Con: effects need a pointer to their layer, coupling them slightly.

**Do we also need DriverBase and LayoutBase?** Same question. If the parent (DriverGroup, LayoutGroup) provides everything the child needs via a pointer, separate base classes may be unnecessary boilerplate. MoonLight uses a single Node base class for effects, layouts, modifiers, and drivers — the parent (VirtualLayer, PhysicalLayer) provides context.

If base classes are kept, they should be zero-state — just convenience accessors pointing to the parent.

## Animation guidelines

Effects use **BPM** (beats per minute) for speed controls, not abstract 0-255 ranges. This gives users a musical reference: 60 BPM = one beat per second, 120 BPM = two beats per second. Default is 60 BPM.

Animation must be **resolution-independent**: multiply the time offset by the panel dimension (width or height) so the perceived visual speed is the same regardless of display size. Without this, large displays look sluggish and small displays look frantic at the same BPM.

Animation is driven by **elapsed millis**, not frame count. This ensures consistent speed regardless of FPS. The speed slider controls the animation dynamics, never the framerate — FPS should always be maximal for smooth motion.

## Rendering context

Whatever provides it, effects need:
- `buffer()` — the `uint8_t*` buffer to write into
- `width()`, `height()`, `depth()` — logical dimensions
- `channelsPerLight()` — channels per light (3=RGB, 4=RGBW, etc.)
- `elapsed()` — milliseconds for animation (synchronized clock)
- `nrOfLights()` — total lights in this layer's buffer

## Prior art

### MoonLight — Node + VirtualLayer ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonBase/Nodes.h), [source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Layers/VirtualLayer.h))
- Effects access `layer->width()`, `layer->height()`, `layer->depth()` directly via the VirtualLayer pointer. No separate EffectBase.
- Buffer access via `layer->virtualChannels` (raw byte array).
- Time via `timeMicros()`.

### projectMM v1 — ProducerModule ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/core/ProducerModule.h))
Base for effects. Produces into a Channel.

### projectMM v2 — PixelEffectBase ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/effects/PixelEffectBase.h))
Shared spine: concrete effect implements only `build_effect_controls()` + `render_(px, w, h, d)`. Eliminates ~70 lines boilerplate.
