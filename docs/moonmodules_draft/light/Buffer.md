# Buffer

Contiguous light data array. The data structure effects write into and
drivers read from.

## API

- `allocate(count)` — allocate via `platform::alloc`, clears to black
- `free()` — safe to call multiple times
- `clear()` — memset to 0
- `fill(RGB)` — fill all lights with one color
- `operator[]` — read/write access
- `lights()` — `std::span<RGB>` view
- Move-constructible, not copyable.

## What worked

- Uses `platform::alloc`/`platform::free` — portable across platforms.
- Reallocating (calling allocate with different size) frees old buffer
  first and clears new buffer.
- Move semantics work correctly.

## What needs improvement

- `allocate()` is called on the hot path when layout/modifier changes
  trigger `rebuildLUT()`. This is cold-path by design but happens
  during frame processing if triggered by HTTP API. Could cause a
  frame stutter.
- No RGBW variant. Would need templating or a separate class.

## Prior art

### MoonLight — VirtualLayer.virtualChannels ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Layers/VirtualLayer.h))
Raw `uint8_t*` buffer, sized by `channelsPerLight * nrOfLights`. Supports RGB, RGBW, and multi-channel DMX fixtures via LightsHeader offsets.

### projectMM v2 — DataBuffer ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/core/DataBuffer.h))
Lock-free single-slot SPSC buffer with atomic revision counter. Teardown-safe via invalidate() sentinel. Multiple consumers each track independent read positions.

### projectMM v1 — Channel ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/modules/layers/Channel.h))
3D array of RGB with width/height/depth metadata. `checksum()` method for verification in tests and health reporting.
