# DriverGroup

Groups output drivers. Owns the shared output buffer when memory allows. Performs blend+map from all layers into the output buffer each frame.

## Shared output buffer

The shared output buffer is necessary because blend+map writes to arbitrary physical positions (via LUT) — the output is not filled sequentially. A driver cannot read chunk-by-chunk until the full buffer is populated.

Exception: when memory is tight AND mapping is 1:1 unshuffled (single layer, grid layout, no serpentine), the driver group can skip its own buffer and read directly from the layer buffer at the cost of parallelism. See architecture-light.md (Parallelism section).

## Buffer type

Same Buffer type as layers — `uint8_t*` sized by `channelsPerLight * nrOfLights` from the LayoutGroup. Allocated via `platform::alloc`.

## API

- `addDriver(driver)` — no hard-coded max, dynamic list
- `setLayers(layers)` — layers to blend from
- `allocateOutput(totalLights, channelsPerLight)` — called when layout changes
- `loop()` — blendMap all layers into output buffer, then call each driver's loop()

## Layer-to-driver assignment

Currently all drivers see all layers. Assigning specific layers to specific drivers may be added in the future if needed.

## Prior art

### MoonLight — PhysicalLayer ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Layers/PhysicalLayer.h))

Owns `channelsD` (display buffer). `compositeLayers()` maps virtualChannels → channelsD. Parallelism via semaphore: driver signals completion, compositor writes.

### projectMM v1 — DriverLayer ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/modules/layers/DriverLayer.h))

Container for driver modules. Receives pixel data from EffectsLayer.

### projectMM v2 — DataRegistry ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/core/DataRegistry.h))

Type-erased buffer directory. Producers declare, consumers resolve by id. Decouples effects from drivers.
