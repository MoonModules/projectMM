# Light Domain Architecture

The light domain is built on top of the core (see `architecture.md`). It defines the render pipeline, light types, layouts, layers, effects, modifiers, mapping, blending, and output drivers.

In this document, **light** refers to any controllable light source: an addressable LED pixel (WS2812, APA102), a DMX fixture (RGB par, moving head, dimmer), or any other output that receives color/intensity data. The term is used instead of "pixel" to reflect that the system controls both LEDs and conventional lighting fixtures.

## The Pipeline

Modules can be added, replaced (e.g. change an effect on a layer), or removed dynamically at runtime.

```
              LayoutGroup (shared across layers and drivers)
                ├── GridLayout  ──→ coordinate iterator
                └── WheelLayout ──→ coordinate iterator
                        │
          ┌─────────────┼─────────────┐
          ▼             ▼             ▼
      Layer A        Layer B       Layer C
    Effect(s)      Effect(s)     Effect(s)
    Modifier(s)    Modifier(s)   Modifier(s)
    Buffer (own)   Buffer (own)  Buffer (own)
    LUT (own)      LUT (own)     LUT (own)
          │             │             │
          └──── Blend+Map ────────────┘
                    │
              DriverGroup
                ├── WS2812Driver  (consumer buffer / DMA)
                ├── ArtNetDriver  (UDP packets)
                └── PreviewDriver (WebSocket)
```

## 3D From the Core

The system is natively 3D. Coordinates, effects, layouts, and mappings all operate in 3D space (x, y, z). 2D and 1D are simply the case where one or two dimensions have size 1. There is no separate 2D mode — everything is 3D, and lower dimensions fall out naturally.

**Numeric types use typedefs to minimize memory usage, especially in LUT tables:**

- `nrOfLightsType` — total number of lights, light indices, LUT destinations, width*height*depth products. `uint16_t` on devices without PSRAM (max 65K), `uint32_t` on devices with PSRAM (supports large hub75 panels).
- `lengthType` — coordinates (x, y, z) and dimensions (width, height, depth). `int8_t` on devices without PSRAM (max 127 per axis, supports negatives for out-of-bounds effects), `int16_t` on devices with PSRAM (max 32767 per axis).

The smaller types on no-PSRAM devices significantly reduce LUT memory: each destination entry is 2 bytes instead of 4, each coordinate is 1 byte instead of 2. For 12K LEDs with a 1:1 LUT, this saves 24KB.

The typedef is selected at compile time based on the target platform's memory configuration. All code uses the typedefs consistently to avoid casting.

## LayoutGroup

A **LayoutGroup** (MoonModule) groups layouts and defines the physical topology of the light installation. It is shared across all layers and drivers — there is one LayoutGroup describing the physical setup, and multiple layers render into it. When a layout changes, all layers rebuild their LUTs.

Note: the term "fixture" is reserved for DMX lighting fixtures (pars, moving heads, etc.) — it is not used for layout grouping.

## Layouts

A layout (MoonModule) defines the physical positions of lights in 3D space. It is a **coordinate iterator** — it yields (physicalIndex, x, y, z) for each light it defines. A layout does not own or build any mapping LUT.

Layouts cover both addressable LEDs and DMX fixtures. An LED strip layout yields one coordinate per LED. A DMX fixture layout yields one coordinate per fixture (a moving head is one point in 3D space).

Layout positions are computed algorithmically, not stored in memory. Grid is the most commonly used layout, but any geometry is possible: spheres, rings, cones, spirals, arbitrary point clouds. A grid layout is an example of full-density mapping (every position maps to a light). A wheel layout is an example of sparse mapping (only spoke positions are mapped, gaps are unmapped).

Layouts are grouped inside a LayoutGroup. A device contains the same light type within a layout. Support for mixing light types in a single LayoutGroup (e.g. LED strips + par lights) is planned for later.

## Layers

A **layer** owns:
- A **buffer** — the light data effects write into (logical space)
- A **mapping LUT** — built by the layer from the LayoutGroup's layouts and the layer's static modifiers
- **Effects** (MoonModules, ordered list) — write light values into the buffer
- **Modifiers** (MoonModules, ordered list) — transform the LUT or light values

A layer can have **multiple effects**. Effects are not blended — they write to the buffer sequentially in the order they are listed. Each effect overwrites or adds to what the previous effect wrote. This allows layering patterns (e.g. a base color effect followed by a sparkle effect).

A layer can have **multiple modifiers**. Modifiers run in order, each taking the result of the previous modifier as input. This means the order matters: mirror-then-rotate produces different output than rotate-then-mirror. Static modifiers chain during LUT build, dynamic modifiers chain during rendering.

Each layer references the shared LayoutGroup. The layer builds its own LUT by iterating the LayoutGroup's layout coordinates and applying its static modifiers in order. Different layers can have different modifiers, producing different LUTs from the same LayoutGroup.

The number of active layers depends on available memory — a device with PSRAM can run many layers; a device without may be limited to one.

## Effects

Effects produce light colors. They write into the layer's buffer, which represents a logical grid. The layer determines the buffer's dimensions (width, height, depth) from the LayoutGroup and its own start/end position within the physical layout, and its modifiers. Effects receive these logical dimensions and elapsed time (millis) as their rendering context. They compute light positions from the buffer index (e.g. `x = i % width`, `y = i / width`).

Effects use elapsed time for animation, not frame count. This makes animation speed independent of frame rate — an effect looks the same at 30fps and 60fps. For multi-device sync, the leader synchronizes elapsed time across devices (same approach as syncing a frame counter, but frame-rate independent).

Effects know nothing about hardware, protocols, physical LED layout, or mapping. They only see the logical grid the layer provides.

**Speed convention:** effects that have a speed control use BPM (beats per minute) as the unit. `uint8_t`, default 60 (= 1 beat per second). This is human-readable, musically meaningful, and DMX-compatible. The effect converts BPM to animation rate internally using elapsed millis.

**Dimensionality:** all effects must be at least 2D (use both x and y). 3D effects (using z) are preferred. 1D-only effects are not accepted — the system is natively 3D and effects should take advantage of that.

## Modifiers

A modifier (MoonModule) lives inside a layer alongside its effects. Modifiers expose a virtual interface — the Layer calls modifier methods without knowing the concrete type (no dynamic_cast).

A modifier can:
- Transform the mapping LUT via a virtual `transformCoord()` method (rebuilt on the cold path, zero render cost).
- Transform light values via a virtual `transformLights()` method on the hot path (per-light cost, enables dynamic animations like rotation).

## Mapping and Blending

The blend+map step walks each layer in turn: it reads each logical light, uses that layer's LUT to find the physical position(s), and blends the color into the physical output buffer. This is where logical space meets physical space.

Each mapping LUT is a flat, contiguous lookup table allocated outside the hot path (at startup or when the layout configuration changes).

The LUT supports three mapping types without dynamic allocation:
- **1:0** — logical light is unmapped (skipped). This is how sparse layouts like wheel produce gaps in the physical output.
- **1:1** — logical light maps to one physical position (direct or shuffled).
- **1:N** — logical light maps to multiple physical positions (mirroring, cloning). Stored as a flat index array, not nested vectors.

Because mapping and blending happen together in a single pass over each layer, there is no intermediate "mapped but unblended" buffer. The physical buffer is the only output-side allocation.

## DriverGroup

A **DriverGroup** (MoonModule) groups output drivers. It is the consumer side of the pipeline. The DriverGroup owns a shared output buffer and performs blend+map from all layers into it each frame. Individual drivers then read from this buffer to push to hardware/network.

The shared output buffer is necessary because blend+map writes to arbitrary physical positions (via LUT) — the output is not filled sequentially. A driver cannot read chunk-by-chunk until the full buffer is populated. Direct-to-DMA/packet optimization would only work for the trivial case (1:1 sequential mapping with no modifiers).

Each driver (MoonModule) speaks one protocol:
- **LED drivers:** WS2812 via RMT, APA102 via SPI. Platform-specific.
- **DMX/ArtNet:** sends DMX data over UDP. Supports both addressable LEDs and conventional DMX fixtures (pars, moving heads, dimmers).
- **Preview:** streams light data to the web UI via WebSocket.
- **Desktop output:** SDL2 or terminal for visual preview. Desktop also serves as a high-speed processing node, driving lights via ArtNet/DDP over the network.

Drivers read from the DriverGroup's output buffer. Everything before the DriverGroup is platform-independent.

Network-based drivers (ArtNet, E1.31, DDP) must pace their packet output — never blast all universe packets in a tight loop. Requires both FPS limiting (skip frames if called too fast) and inter-packet delay (microsecond pause between universe packets within a frame). Without pacing, receivers drop packets and the output appears broken.

## Why this design?

- Effects don't know about hardware → portable and composable.
- Modifiers can transform mappings (LUT rebuild, zero per-light cost) or transform light values on the hot path (e.g. dynamic rotation — slower but supported when needed).
- Layouts are algorithmic → memory-efficient for large installations.
- Mappings are data (a LUT) → loadable, savable, editable, shareable.
- Map-on-the-fly → no intermediate buffer, half the memory, better cache.
- Drivers are thin → new protocols are easy to add.
- MoonModule model → low barrier for contributors.
- Zero allocations in steady state. Buffers are (re)allocated outside the hot path — at startup or when configuration changes.

## Parallelism (light domain)

The core parallelism model (see `architecture.md`) applies to the light pipeline as follows:

- **Producers** = effects. Generate light data into layer buffers (logical space). Pinned to one core.
- **Consumers** = drivers (LED output, ArtNet send). Perform blend+map from logical buffers into their output, push to hardware/network. Pinned to another core. Consumers own the blend+map step because they know what they consume from.

The logical and physical buffers **are** the double buffer: producers write into logical buffers while consumers read from the physical buffer. At the frame boundary, roles swap via atomic pointer swap — the driver transmits frame N while effects render frame N+1.

When memory is sufficient (even on ESP32 without PSRAM for small layouts up to ~4K lights), the system uses double buffering with a separate physical buffer, mapping, blending, and producer/consumer parallelism — same as devices with PSRAM.

When memory is too tight for double buffering (large layouts on devices without PSRAM), the system is limited to: one layer with 1:1 unshuffled mapping, effects write directly into their layer buffer, drivers read from that same buffer to fill DMA/UDP packets directly. No blend+map step, no mapping, no blending, no parallelism. This is how the 12K LED stretch goal is achieved on ESP32 without PSRAM.

## Memory Strategy

All buffers are allocated as single contiguous blocks outside the hot path — at startup or when configuration changes (e.g. LED count, layout size, layer count). They are then reused every frame with zero allocations in steady state.

### Buffer types

- **Layer buffers.** One per active effect layer. Each holds the logical light data for one effect. Allocated in PSRAM when available. On memory-constrained devices, consumers may read from the layer buffer directly (no mapping, no blending, no physical buffer needed).
- **Physical buffer.** When present, holds the blended+mapped output. The logical and physical buffers together form the double buffer for producer/consumer parallelism.
- **Mapping LUT.** The flat lookup table for logical→physical coordinate mapping. Read-only during rendering. PSRAM is fine — sequential reads are cache-friendly.

All buffers are raw `uint8_t*` arrays sized by `channelsPerLight * nrOfLights`. This supports RGB (3 channels), RGBW (4 channels), and multi-channel DMX fixtures (up to 32 channels per light) without separate code paths. Channel layout is configured via offsets (see MoonLight's [LightsHeader](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Layers/LightsHeader.h) pattern).

Network input (ArtNet receive, WebSocket) is processed synchronously at a defined point in the frame loop. This means zero extra buffers and no race conditions. The trade-off is up to one frame of latency (~16ms at 60fps), which is imperceptible for LEDs.

### Scaling to available memory

The system adapts to what the device has:

| Device | Memory | Typical capability |
|--------|--------|--------------------|
| ESP32 + OPI PSRAM | 2-8 MB | Many layers, 10K+ LEDs |
| ESP32, no PSRAM, small layout | ~320 KB internal | Full pipeline: double buffering, mapping, blending, parallelism. Up to ~4K lights. |
| ESP32, no PSRAM, large layout | ~320 KB internal | Fallback: single layer, 1:1 direct, no blending/parallelism. 4K-12K lights (12K stretch goal). |
| Teensy 4.x | 1 MB internal, no PSRAM | Single/few layers, excellent DMA-based LED output (OctoWS2811), no WiFi (USB or serial control) |
| Desktop / RPi | Abundant | No constraints |

The architecture does not assume PSRAM is present. Buffer counts and sizes are determined at runtime based on available memory. They are reallocated when configuration changes.

## Testing (light domain)

Full test inventory: [docs/testing.md](testing.md). Module specs link to their test sections.

### Module tests (desktop)

- Color math (HSV→RGB, scale8) — [testing.md#color](testing.md#color)
- Buffer operations (allocate, clear, move, bounds) — [testing.md#buffer](testing.md#buffer)
- GridLayout (coordinate iteration, row-major, 3D) — [testing.md#gridlayout](testing.md#gridlayout)
- RainbowEffect (output correctness, spatial variation) — [testing.md#rainbow](testing.md#rainbow)
- ArtNet packet (header format, byte order, universe splitting) — [testing.md#artnet](testing.md#artnet)
- Mapping LUT (1:0, 1:1, 1:N, rebuild on config change) — when implemented
- Blend+map pass (correct physical output from logical layers) — when implemented

### Scenario tests (desktop)

- Base pipeline (full render pipeline, performance bounds) — [testing.md#scenario-pipeline](testing.md#scenario-pipeline)

### Live system tests (on-device)

- Light output produces correct signal (protocol-level, verified with logic analyzer or known-good reference).
- Multi-device sync achieves sub-millisecond accuracy.

## Multi-Device Sync

For installations spanning multiple controllers:

1. **Discovery.** Devices find each other via mDNS.
2. **Time sync.** One leader broadcasts its elapsed time (millis). Followers compute their offset. Target: sub-millisecond accuracy. Since effects use elapsed time for animation, synced time means synced visuals — regardless of each device's frame rate.
3. **Light distribution.** When one device needs to send light data to another, use ArtNet/E1.31/DDP — these are the standards, no need to invent a protocol.
