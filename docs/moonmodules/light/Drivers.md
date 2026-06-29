# Drivers

![Drivers controls](../../assets/screenshots/Drivers.png)

Top-level container for one or more drivers. The consumer side of the pipeline — owns the shared output buffer (when memory allows) and performs blend+map from every layer's buffer into it each frame.

> **Naming convention.** Capital `Drivers` is the container class; lowercase "driver"/"drivers" is the English singular/plural for individual `DriverBase` children. Capitalisation disambiguates "the Drivers container" from "two drivers running". Same rule for `Layouts`/layout and `Layers`/layer.

## Shared output buffer

The shared output buffer is necessary because blend+map writes to arbitrary physical positions (via LUT) — the output is not filled sequentially. A driver cannot read chunk-by-chunk until the full buffer is populated.

Exception: when exactly one layer is enabled AND its mapping is 1:1 unshuffled (no LUT — grid layout, no serpentine), Drivers skips its own buffer and lets drivers read directly from the layer's buffer (the zero-copy fast path, at the cost of parallelism). See [architecture.md § Parallelism](../../architecture.md#parallelism).

It uses the same `Buffer` type a Layer does, sized by the Layouts container.

## Multi-layer composition

When two or more layers are enabled, Drivers composites them into the shared output buffer each frame, in [Layers](Layers.md) container order (bottom→top, via `forEachEnabledLayer`). The bottom layer clears and overwrites the buffer; each layer above blends onto the accumulated frame per its own `blendMode` and `opacity` (the inert per-Layer controls — see [Layer](Layer.md#blendmode--opacity-controls)). Drivers owns the orchestration because only it sees the stack order and the output buffer; the layers carry only the parameters. The per-pixel blend math lives in [BlendMap](BlendMap.md) (integer-only, per the hot-path rule). A full-opacity overwrite/additive layer pays no alpha arithmetic, so the per-frame cost scales with the enabled-layer count. With a single enabled layer there is no composite: the fast path above applies (no-LUT → zero-copy; with a LUT → one blend+map pass into the output buffer).

## Output correction

The Drivers container owns the shared output-correction state and exposes two controls; each *physical* driver child (ArtNet today, future LED drivers) applies it per-light as it reads the source buffer. Preview ignores it (shows the raw logical buffer).

| Control | Type | Description |
|---|---|---|
| `brightness` | uint8 (0–255) | Global brightness. Scales every channel through a 256-entry LUT (`(v × brightness) / 255`). Changing it rebuilds only the LUT on the cheap `onUpdate` tier — no pipeline realloc, so the slider is fluent. Gamma / white-balance fold into this LUT later as a per-channel R/G/B split. |
| `lightPreset` | select | The physical wire format: channel order and whether the light is RGBW. Options: `RGB`, `RBG`, `GRB`, `GBR`, `BRG`, `BGR`, `RGBW`, `GRBW`. Defaults to `GRB` — the WS2812/SK6812 wire order, so a strip shows correct colours out of the box (PreviewDriver reads the RGB source buffer directly and is unaffected). RGBW presets make each driver emit 4 channels per light with white derived as `min(R,G,B)` from the (brightness-scaled) RGB. |

The state lives on `Correction` (`src/light/drivers/Correction.h`): a brightness LUT, channel-order table, output channel count, derive-white flag. `Drivers::onUpdate` rebuilds it on a `brightness`/`lightPreset` change and hands each child a `const Correction*`. Every driver sees the same composited output; per-driver layer assignment (different drivers reading different layers) is a [backlog](../../backlog/README.md) item.

## Per-driver source window (`start` / `count`)

Every driver reads the **same** shared source buffer and outputs a contiguous slice of it — its *window*. Two controls on `DriverBase`, shared by every driver child (the LED drivers, the network sink):

| Control | Type | Description |
|---|---|---|
| `start` | uint16 | First source-buffer light this driver outputs. Default `0`. |
| `count` | uint16 | Number of lights to output from `start`. Default `0` = **to the end of the buffer**. The slice is `[start, start+count)`, clamped to the buffer (a `start` past the end yields an empty slice — the driver idles, no out-of-bounds read). |

This makes light distribution **explicit and order-independent**: each driver names its own slice, so reordering drivers does not change which lights each outputs (it only changes tick order). It is the alternative to a "split the buffer by sibling order" model some controllers use — here the user (or catalog) says which slice goes where.

The motivating case: an **onboard status LED** and a **main strip** as two driver instances on the same buffer — one with window `[0, 1)` (the single onboard LED on its own pin), the other with window `[1, …)` (the strip on its pin, starting one light in). Neither steals the other's lights. Within a driver's window, the LED drivers' `pins` / `ledsPerPin` distribute *that slice* across the pins; the network sink maps its slice onto `universe_start` (the protocol offset is separate from the buffer `start`).

## Prior art

### MoonLight — PhysicalLayer ([source](https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Layers/PhysicalLayer.h))

Owns `channelsD` (display buffer). `compositeLayers()` maps virtualChannels → channelsD. Parallelism via semaphore: driver signals completion, compositor writes.

### projectMM v1 — DriverLayer ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/modules/layers/DriverLayer.h))

Container for driver modules. Receives pixel data from EffectsLayer.

### projectMM v2 — DataRegistry ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/core/DataRegistry.h))

Type-erased buffer directory. Producers declare, consumers resolve by id. Decouples effects from drivers.

## Source

[Drivers.h](../../../src/light/drivers/Drivers.h)
