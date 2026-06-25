# Layers

![Layers controls](../../assets/screenshots/Layers.png)

Top-level container for one or more layers. Each layer renders independently into its own buffer; the Drivers container composes those buffers downstream.

> **Naming convention.** Capital `Layers` is the container class; lowercase "layer"/"layers" is the English singular/plural for individual `Layer` children. Capitalisation disambiguates "the Layers container" from "two layers stacked". Same rule for `Layouts`/layout and `Drivers`/driver.

## Why a container

Multi-layer composition (alpha-blend, additive, layered overlays) needs a place to walk every layer in order so drivers can merge their buffers before consuming the result. Layers is that place. With one layer inside it the container is a thin pass-through: `loop()` runs the single child and returns; behaviour is byte-identical to the single-layer pipeline.

The container owns no buffer: each layer owns its own, and the Drivers container owns the composited output. It wires the shared Layouts into every child so each can size its buffer. Two queries serve the Drivers compositor: `activeLayer()` (the first enabled child) answers physical dimensions and is the source for the single-layer fast path, and `forEachEnabledLayer(cb)` walks the enabled children in container order (bottom→top) — the order Drivers blends them, with `cb(layer, isFirst)` marking the bottom layer that clears the buffer. `enabledLayerCount()` lets Drivers pick the fast path (one enabled layer → hand its buffer straight to the driver) versus the composite path (≥2 → blend into the output buffer). The blend modes and the value-on-Layer / logic-in-Drivers split are documented on [Layer](Layer.md#blendmode--opacity-controls) and [Drivers](Drivers.md).

## Prior art

### MoonLight — VirtualLayer / PhysicalLayer ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight))

MoonLight's `PhysicalLayer` runs N `VirtualLayer`s and composites their buffers into the display channel. Same idea, different shape: Drivers (not Layers) does the compositing here.

### projectMM v1/v2

Single-layer designs. No prior container for multiple layers.

## Source

[Layers.h](../../../src/light/layers/Layers.h)
