# EffectBase

Light-domain MoonModule subclass for effects. Adds rendering context.

## Design

`EffectBase` is a zero-state convenience layer: it holds no data of its own, just accessors (`buffer()`, `width()`, `height()`, `depth()`, `elapsed()`, …) that forward to the parent `Layer`. An effect reads its rendering context through these instead of caching a `Layer*` and the dimensions itself. `DriverBase` plays the same role for drivers against `Drivers`.

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

## Dimensions and auto-extrusion

Each effect declares which axes it iterates through `virtual Dim dimensions() const` (default `Dim::D3`):

- `Dim::D3` — effect iterates x, y, z itself. The framework does no extrusion.
- `Dim::D2` — effect promises to write only the `z = 0` slice. `Layer::extrude` copies that slice across every other z on a 3D layer.
- `Dim::D1` — effect promises to write only the `y = 0, z = 0` row. `Layer::extrude` fills y then z.

Two contracts to honour in `loop()`:

1. **Use `width()`, `height()`, `depth()` at frame time.** Never hardcode a maximum (no `for z < SOMETHING`). A D3 effect may run on a D1 or D2 layer; its loop must iterate whatever the layer provides. Writing past `width × height × depth × channels` is a buffer overrun.
2. **A D2/D1 effect is an opt-in promise.** Declaring D2 tells the framework it can `memcpy` your z = 0 slice across z; declaring D1 lets it fill y and z. Stateful effects (own dynamic buffers) should size those to the same slice the loop writes — `w × h × cpl` for D2, `w × cpl` for D1 — not the full 3D buffer.

The `dim` int (1/2/3) is emitted in `/api/types`; the UI derives the 📏/🟦/🧊 chip from it. See [architecture-light.md § Effects](../../architecture-light.md#effects) for the live declarations per shipped effect, and [test_extrude](../../testing.md#extrude) for the pinned contract tests.

## Prior art

### MoonLight — Node + VirtualLayer ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonBase/Nodes.h), [source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Layers/VirtualLayer.h))

- Effects access `layer->width()`, `layer->height()`, `layer->depth()` directly via the VirtualLayer pointer. No separate EffectBase.
- Buffer access via `layer->virtualChannels` (raw byte array).
- Time via `timeMicros()`.

### projectMM v1 — ProducerModule ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/core/ProducerModule.h))

Base for effects. Produces into a Channel.

### projectMM v2 — PixelEffectBase ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/effects/PixelEffectBase.h))

Shared spine: concrete effect implements only `build_effect_controls()` + `render_(px, w, h, d)`. Eliminates ~70 lines boilerplate.
