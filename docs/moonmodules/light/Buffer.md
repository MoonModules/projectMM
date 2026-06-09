# Buffer

Contiguous light data buffer. Used by both layers (effects write into it) and driver groups (drivers read from it). When memory allows, layers and driver groups each have their own buffer (enabling parallelism). When memory is tight, a single buffer is shared.

## Storage

A raw `uint8_t*` (not `RGB*`) keeps the buffer flexible for any channel layout — RGB, RGBW, or multi-channel DMX fixtures — addressed by channel count + offset. Allocated via `platform::alloc` (PSRAM when available) outside the hot path and reused every frame; a `std::span<uint8_t>` view is the zero-cost safe accessor.

## Locking

Semaphores are expensive (~150 bytes on ESP32), so prefer lock-free patterns: an atomic pointer swap for double-buffering, a single-slot SPSC handoff, or one shared semaphore across layers rather than one per layer.

## Tests

[Unit tests: Buffer](../../tests/unit-tests.md#buffer) — allocate, clear, move semantics, double-free safety, zero-size edge case.

## Prior art

### MoonLight — VirtualLayer.virtualChannels ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Layers/VirtualLayer.h))

Raw `uint8_t*` buffer, sized by `channelsPerLight * nrOfLights`. Supports RGB, RGBW, and multi-channel DMX fixtures via LightsHeader offsets.

### projectMM v1 — Channel ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/modules/layers/Channel.h))

3D array of RGB with width/height/depth metadata. `checksum()` method for verification in tests and health reporting.

### projectMM v2 — DataBuffer ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/core/DataBuffer.h))

Lock-free single-slot SPSC buffer with atomic revision counter. Teardown-safe via invalidate() sentinel. Multiple consumers each track independent read positions.

## Source

[Buffer.h](../../../src/light/layers/Buffer.h)
