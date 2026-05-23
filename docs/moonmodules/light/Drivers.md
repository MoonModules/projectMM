# Drivers

Top-level container for one or more drivers. The consumer side of the pipeline — owns the shared output buffer (when memory allows) and performs blend+map from every layer's buffer into it each frame.

> **Naming convention.** Capital `Drivers` is the container class; lowercase "driver"/"drivers" is the English singular/plural for individual `DriverBase` children. Capitalisation disambiguates "the Drivers container" from "two drivers running". Same rule for `Layouts`/layout and `Layers`/layer.

## Shared output buffer

The shared output buffer is necessary because blend+map writes to arbitrary physical positions (via LUT) — the output is not filled sequentially. A driver cannot read chunk-by-chunk until the full buffer is populated.

Exception: when memory is tight AND mapping is 1:1 unshuffled (single layer, grid layout, no serpentine), Drivers can skip its own buffer and let drivers read directly from the layer's buffer at the cost of parallelism. See [architecture-light.md § Parallelism](../../architecture-light.md#parallelism-light-domain).

## Buffer type

Same `Buffer` as a Layer uses — `uint8_t*` sized by `channelsPerLight × nrOfLights` (from the Layouts container). Allocated via `platform::alloc`.

## API

- `addChild(driver)` — no hard-coded max, dynamic list.
- `setLayer(layer)` — the active layer to blend from. Today (single-layer pipeline) wired directly; the composition follow-up will read from the Layers container and blend across every layer.
- `onAllocateMemory()` — allocates the shared output buffer if any active layer has a LUT.
- `loop()` — blendMap the active layer's buffer into the output buffer, then call each driver's `loop()`.

## Layer-to-driver assignment

Currently every driver sees the same output (the active layer's buffer, blended via LUT). Assigning specific layers to specific drivers is a possible future extension once composition lands.

## Prior art

### MoonLight — PhysicalLayer ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Layers/PhysicalLayer.h))

Owns `channelsD` (display buffer). `compositeLayers()` maps virtualChannels → channelsD. Parallelism via semaphore: driver signals completion, compositor writes.

### projectMM v1 — DriverLayer ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/modules/layers/DriverLayer.h))

Container for driver modules. Receives pixel data from EffectsLayer.

### projectMM v2 — DataRegistry ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/core/DataRegistry.h))

Type-erased buffer directory. Producers declare, consumers resolve by id. Decouples effects from drivers.
