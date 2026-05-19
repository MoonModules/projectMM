# Buffer

Contiguous light data buffer. Used by both layers (effects write into it) and driver groups (drivers read from it). When memory allows, layers and driver groups each have their own buffer (enabling parallelism). When memory is tight, a single buffer is shared.

## Storage

Raw `uint8_t*` buffer, sized by `channelsPerLight * nrOfLights`. This supports RGB (3 channels), RGBW (4 channels), and multi-channel DMX fixtures (up to 32 channels per light) via configurable channel count and offsets (see MoonLight's LightsHeader pattern).

Memory allocated via `platform::alloc` (PSRAM when available). Allocated outside the hot path, reused every frame.

`std::span<uint8_t>` provides a safe, zero-cost view into the raw buffer. Using `uint8_t*` rather than `RGB*` keeps the buffer flexible for any channel configuration. For the common RGB case, convenience accessors can cast to RGB on the fly.

## Locking

Semaphores are expensive (~150 bytes on ESP32). Prefer:
- Atomic pointer swap for double-buffering (producer/consumer)
- Lock-free single-slot SPSC pattern (proven in v2's DataBuffer)
- Share a single semaphore across multiple layers rather than one per layer

## API

- `allocate(nrOfLights, channelsPerLight)` — allocate via `platform::alloc`
- `free()` — safe to call multiple times
- `clear()` — memset to 0
- `data()` — raw `uint8_t*` pointer
- `span()` — `std::span<uint8_t>` view
- `count()` — number of lights
- `channelsPerLight()` — channels per light
- `bytes()` — total byte count

Move-constructible, not copyable.

## Prior art

### MoonLight — VirtualLayer.virtualChannels ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Layers/VirtualLayer.h))
Raw `uint8_t*` buffer, sized by `channelsPerLight * nrOfLights`. Supports RGB, RGBW, and multi-channel DMX fixtures via LightsHeader offsets.

### projectMM v1 — Channel ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/modules/layers/Channel.h))
3D array of RGB with width/height/depth metadata. `checksum()` method for verification in tests and health reporting.

### projectMM v2 — DataBuffer ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/core/DataBuffer.h))
Lock-free single-slot SPSC buffer with atomic revision counter. Teardown-safe via invalidate() sentinel. Multiple consumers each track independent read positions.
