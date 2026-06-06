# Layers

![Layers controls](../../assets/screenshots/Layers.png)

Top-level container for one or more layers. Each layer renders independently into its own buffer; the Drivers container composes those buffers downstream.

> **Naming convention.** Capital `Layers` is the container class; lowercase "layer"/"layers" is the English singular/plural for individual `Layer` children. Capitalisation disambiguates "the Layers container" from "two layers stacked". Same rule for `Layouts`/layout and `Drivers`/driver.

## Why a container

Multi-layer composition (alpha-blend, additive, layered overlays) needs a place to walk every layer in order and merge their buffers before drivers consume the result. Layers is that place. Today the boot pipeline creates **one layer inside Layers**, so the container is a thin pass-through: `loop()` runs the single child and returns; behaviour is byte-identical to the previous single-layer pipeline.

The container itself owns no buffer. Each layer owns its own buffer; the Drivers container owns the composed output buffer (today: blend+map from the active layer; tomorrow: blend from every layer).

## API

- `addChild(layer)` — add a layer (heap-allocated list, grows on demand).
- `setLayouts(Layouts*)` — wire the shared Layouts and propagate it to every layer so each can size its buffer. Idempotent — call again after adding a layer to wire the new one.
- `layouts()` — accessor for the wired Layouts pointer.
- `activeLayer()` — the single-layer pipeline: returns the first enabled layer (or the first child if none are enabled). The Drivers container reads it for buffer + dimensions.
- `loop()` — walks every enabled layer in order, timing each one. Per-layer timing surfaces in the card's stats line.

Today a single layer is active: `activeLayer()` returns the first enabled layer and the Drivers container reads its buffer. Multi-layer blending (the producer/consumer split where Layers iterates and Drivers composites) is not yet built.

## Prior art

### MoonLight — VirtualLayer / PhysicalLayer ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight))

MoonLight's `PhysicalLayer` runs N `VirtualLayer`s and composites their buffers into the display channel. Same idea, different shape: Drivers (not Layers) does the compositing here.

### projectMM v1/v2

Single-layer designs. No prior container for multiple layers.
