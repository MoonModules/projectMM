# Layers

![Layers controls](../../assets/screenshots/Layers.png)

Top-level container for one or more layers. Each layer renders independently into its own buffer; the Drivers container composes those buffers downstream.

> **Naming convention.** Capital `Layers` is the container class; lowercase "layer"/"layers" is the English singular/plural for individual `Layer` children. Capitalisation disambiguates "the Layers container" from "two layers stacked". Same rule for `Layouts`/layout and `Drivers`/driver.

## Why a container

Multi-layer composition (alpha-blend, additive, layered overlays) needs a place to walk every layer in order and merge their buffers before drivers consume the result. Layers is that place. Today the boot pipeline creates **one layer inside Layers**, so the container is a thin pass-through: `loop()` runs the single child and returns; behaviour is byte-identical to the previous single-layer pipeline.

The container owns no buffer: each layer owns its own, and the Drivers container owns the composed output. It wires the shared Layouts into every child so each can size its buffer. While a single layer is active, `activeLayer()` (the first enabled child) is what Drivers reads; multi-layer blending — where Layers iterates and Drivers composites across all of them — is a [backlog](../../backlog/README.md) item.

## Prior art

### MoonLight — VirtualLayer / PhysicalLayer ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight))

MoonLight's `PhysicalLayer` runs N `VirtualLayer`s and composites their buffers into the display channel. Same idea, different shape: Drivers (not Layers) does the compositing here.

### projectMM v1/v2

Single-layer designs. No prior container for multiple layers.

## Source

[Layers.h](../../../src/light/layers/Layers.h)
