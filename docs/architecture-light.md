# Light Domain Architecture

The light domain is built on top of the core (see `architecture.md`). It defines the render pipeline, light types, the Layouts / Layers / Drivers containers, effects, modifiers, mapping, and blending.

In this document, **light** refers to any controllable light source: an addressable LED pixel (WS2812, APA102), a DMX fixture (RGB par, moving head, dimmer), or any other output that receives color/intensity data. The term is used instead of "pixel" to reflect that the system controls both LEDs and conventional lighting fixtures.

## The Problem

Drive 10,000+ addressable LEDs and DMX lighting fixtures (RGB(W) pars, moving heads, dimmers) across multiple synchronized devices at high frame rates. Support LED protocols (WS2812, APA102) and DMX/ArtNet/E1.31/DDP for conventional lighting fixtures. Render effects natively in 3D space. Run the full pipeline on devices ranging from an ESP32 with no PSRAM to a desktop, scaling buffer counts and pipeline complexity to the memory available. Provide a web UI with live 3D preview and network APIs for control.

The light domain is everything specific to that goal. The generic module/runtime machinery it stands on — modules, controls, scheduling, persistence, platform abstraction — is the core (see `architecture.md`).

## The Pipeline

Modules can be added, replaced (e.g. change an effect on a layer), or removed dynamically at runtime.

```
              Layouts (shared by every Layer in Layers)
                ├── GridLayout  ──→ coordinate iterator
                └── WheelLayout ──→ coordinate iterator
                        │
                    Layers
                ┌───────┼───────┐
                ▼       ▼       ▼
            Layer A  Layer B  Layer C
          Effect(s) Effect(s) Effect(s)
        Modifier(s) Modifier(s) Modifier(s)
        Buffer(own) Buffer(own) Buffer(own)
            LUT(own) LUT(own) LUT(own)
                │       │       │
                └── Blend+Map ──┘
                        │
                    Drivers
                ├── WS2812Driver  (consumer buffer / DMA)
                ├── ArtNetDriver  (UDP packets)
                └── PreviewDriver (WebSocket)
```

**Naming convention.** Capital `Layouts`, `Layers`, `Drivers` are class names (always written capitalised when they refer to the class). Lowercase "layouts", "layers", "drivers" is the English plural — used freely when the sentence makes it clear that multiple instances are meant. Singular "layout", "layer", "driver" is an individual instance. The capitalisation alone disambiguates "the Layers container" from "two layers stacked", so this doc reaches for natural English plurals rather than awkward "Layer instances" / "Layer children" phrasing.

## 3D From the Core

The system is natively 3D. Coordinates, effects, layouts, and mappings all operate in 3D space (x, y, z). 2D and 1D are simply the case where one or two dimensions have size 1. There is no separate 2D mode — everything is 3D, and lower dimensions fall out naturally.

**Numeric types use typedefs to minimize memory usage, especially in LUT tables:**

- `nrOfLightsType` — total number of lights, light indices, LUT destinations, width*height*depth products. `uint16_t` on devices without PSRAM (max 65K), `uint32_t` on devices with PSRAM (supports large hub75 panels). Selected at compile time via `platform_config.h`.
- `lengthType` — coordinates (x, y, z) and dimensions (width, height, depth). Always `int16_t` (max 32767 per axis, supports negatives for out-of-bounds effects). Using `int8_t` was considered for no-PSRAM devices but rejected — it can't hold values >= 128 and the memory savings only matter in MappingLUT (which can use its own compact storage when implemented).

The smaller `nrOfLightsType` on no-PSRAM devices reduces LUT memory: each destination entry is 2 bytes instead of 4. For 12K LEDs with a 1:1 LUT, this saves 24KB.

All code uses the typedefs consistently to avoid casting.

## Layouts

**Layouts** (a MoonModule) is the top-level container for one or more layouts, defining the physical topology of the light installation. It is shared by every layer in the Layers container — there is one Layouts describing the physical setup, and every layer renders into it. When a layout changes, every layer rebuilds its LUT.

Note: the term "fixture" is reserved for DMX lighting fixtures (pars, moving heads, etc.) — it is not used for layout grouping.

## Layout

A **layout** (a `LayoutBase` MoonModule, child of Layouts) defines the physical positions of lights in 3D space. It is a **coordinate iterator** — it yields (physicalIndex, x, y, z) for each light it defines. A layout does not own or build any mapping LUT.

Layouts cover both addressable LEDs and DMX fixtures. An LED-strip layout yields one coordinate per LED; a DMX-fixture layout yields one coordinate per fixture (a moving head is one point in 3D space).

Layout positions are computed algorithmically, not stored in memory. Grid is the most commonly used layout, but any geometry is possible: spheres, rings, cones, spirals, arbitrary point clouds. A grid layout is an example of full-density mapping (every position maps to a light). A wheel layout is an example of sparse mapping (only spoke positions are mapped, gaps are unmapped).

Multiple layouts live inside one Layouts container. A device today contains the same light type within each layout. Support for mixing light types in a single Layouts (e.g. LED strips + par lights) is planned for later.

## Layers

**Layers** (a MoonModule) is the top-level container for one or more layers. Each layer renders independently into its own buffer; the Drivers container composes those buffers downstream (today: pass through the first active layer; alpha-blend / additive composition is the follow-up).

Today the boot pipeline creates one layer inside Layers, so the container is a thin pass-through. The number of additional layers a device can run is bounded by memory — a device with PSRAM can run many; a device without may be limited to one.

## Layer

A **Layer** (a MoonModule, child of Layers) owns:
- A **buffer** — the light data effects write into (logical space)
- A **mapping LUT** — built by the layer from the shared Layouts' layouts and the layer's static modifiers
- **Effects** (MoonModules, ordered list) — write light values into the buffer
- **Modifiers** (MoonModules, ordered list) — transform the LUT or light values

A layer can have **multiple effects**. Effects are not blended — they write to the buffer sequentially in the order they are listed. Each effect overwrites or adds to what the previous effect wrote. This allows stacked patterns (e.g. a base-colour effect followed by a sparkle effect).

A layer can have **multiple modifiers**. Modifiers run in order, each taking the result of the previous modifier as input. Order matters: mirror-then-rotate produces different output than rotate-then-mirror. Static modifiers chain during LUT build; dynamic modifiers chain during rendering.

Each layer references the shared Layouts. The layer builds its own LUT by iterating the Layouts container's coordinates and applying its static modifiers in order. Different layers in the Layers container can have different modifiers, producing different LUTs from the same Layouts.

## Effects

Effects produce light colors. They write into the Layer's buffer, which represents a logical grid. The Layer determines the buffer's dimensions (width, height, depth) from the Layouts and its own start/end percentages within the physical layout, and its modifiers. Effects receive these logical dimensions and elapsed time (millis) as their rendering context. They compute light positions from the buffer index (e.g. `x = i % width`, `y = i / width`).

Effects use elapsed time for animation, not frame count. This makes animation speed independent of frame rate — an effect looks the same at 30fps and 60fps. For multi-device sync, the leader synchronizes elapsed time across devices (same approach as syncing a frame counter, but frame-rate independent).

Effects know nothing about hardware, protocols, physical LED layout, or mapping. They only see the logical grid the layer provides.

**Speed convention:** effects that have a speed control use BPM (beats per minute) as the unit. `uint8_t`, default 60 (= 1 beat per second). This is human-readable, musically meaningful, and DMX-compatible. The effect converts BPM to animation rate internally using elapsed millis.

**Dimensionality:** every effect declares its native dimensionality through `EffectBase::dimensions()`, returning `Dim::D1`, `Dim::D2`, or `Dim::D3` (**the default — "I iterate every axis the layer gives me"**). The Layer uses this to **extrude** lower-dimensional output across the unused axes after each effect's `loop()`:

- D1 — effect promises to write only the row at (y=0, z=0). Layer copies that row across every other y in z=0, then copies z=0 across every z.
- D2 — effect promises to write only the z=0 slice. Layer copies z=0 across every other z.
- D3 — effect writes every axis itself. Extrude is a one-comparison no-op.

D1/D2 are **opt-in promises**: declaring them tells the framework it can fill the missing axes for you, saving the per-effect work of iterating z (or y and z). Effects that don't make that promise stay at the D3 default and iterate the whole buffer.

Hot-path cost: extrude pays one comparison and returns for the D3 case. For D1/D2 on a layer whose unused axes are size 1 (a D2 effect on a 2D layer, a D1 effect on a 1D layer) the inner loops are guarded by `depth_ > 1` / `height_ > 1` and never run. The only case where real `memcpy` work happens is a D1 or D2 effect on a layer that has more dimensions than the effect writes — exactly the case where you wanted the framework to do the duplication.

Today's declarations:
- **NoiseEffect**, **PlasmaEffect** — D3. Their math has real z variation (trilinear value noise / a fifth z-driven sine).
- **RainbowEffect**, **CheckerboardEffect**, **SpiralEffect**, **RipplesEffect**, **GlowParticlesEffect**, **LavaLampEffect**, **MetaballsEffect**, **PlasmaPaletteEffect**, **FireEffect**, **ParticlesEffect** — D2. Their loops iterate y and x only; extrude fills z. The two stateful ones (Fire, Particles) size their dynamic buffers to `w × h × cpl` (z=0 plane) rather than `w × h × d × cpl` (full 3D buffer), saving heap on 3D Layers.

Each effect's `dimensions()` is a claim about which axes its loop iterates — not which axes its math could in principle vary along. A "D2 fire" could in future be promoted to D3 by adding z-aware heat propagation; until then declaring it D2 is the honest description of what the loop does today.

The `dim` int (1/2/3 for effects, 1/2/3 for modifiers, 0 for layouts/drivers/generics) is also emitted in `/api/types`, and the UI derives the dimensional emoji (📏/🟦/🧊) from it — modules don't put dimensional emoji in their own `tags()` string.

## Modifiers

A modifier (MoonModule) lives inside a layer alongside its effects. Modifiers expose a virtual interface — the Layer calls modifier methods without knowing the concrete type (no dynamic_cast).

A modifier can:
- Transform the mapping LUT via a virtual `transformCoord()` method (rebuilt on the cold path, zero render cost).
- Transform light values via a virtual `transformLights()` method on the hot path (per-light cost, enables dynamic animations like rotation).

**Dimensionality:** modifiers also declare `dimensions()`, defaulting to `Dim::D3` (a modifier that touches the LUT is assumed to work in all three axes unless it declares otherwise). Unlike for effects, this is purely advisory — the Layer doesn't extrude modifier output. It exists so the UI can render the 📏/🟦/🧊 chip on the card and in the type picker, letting users see at a glance whether a modifier will do anything along z. **MirrorModifier** is D3 (it has independent mirrorX/Y/Z toggles).

## Mapping and Blending

The blend+map step walks each layer in turn: it reads each logical light, uses that layer's LUT to find the physical position(s), and blends the color into the physical output buffer. This is where logical space meets physical space.

Each mapping LUT is a flat, contiguous lookup table allocated outside the hot path (at startup or when the layout configuration changes).

The LUT supports four mapping types:
- **1:1 identical** — logical index equals physical index. No table needed (`hasLUT()` returns false, `setIdentity()` mode). Grid without serpentine, no modifiers.
- **1:1 shuffled** — logical maps to one physical, but reordered. Table needed. Grid with serpentine.
- **1:0 unmapped** — logical light has no physical output. Table needed. Sparse layouts (wheel).
- **1:N multimap** — logical maps to multiple physical positions. Table needed (CSR format). Mirror/clone modifier.

Because mapping and blending happen together in a single pass over each layer, there is no intermediate "mapped but unblended" buffer. The physical buffer is the only output-side allocation.

## Drivers

**Drivers** (a MoonModule) is the top-level container for one or more drivers. It is the consumer side of the pipeline. The Drivers container owns a shared output buffer and performs blend+map from every layer's buffer into it each frame. Individual drivers then read from this buffer to push to hardware/network.

The shared output buffer is necessary because blend+map writes to arbitrary physical positions (via LUT) — the output is not filled sequentially. A driver cannot read chunk-by-chunk until the full buffer is populated. Direct-to-DMA/packet optimization would only work for the trivial case (1:1 identical mapping with no modifiers).

Each driver (MoonModule) speaks one protocol:
- **LED drivers:** WS2812 via RMT, APA102 via SPI. Platform-specific.
- **DMX/ArtNet:** sends DMX data over UDP. Supports both addressable LEDs and conventional DMX fixtures (pars, moving heads, dimmers).
- **Preview:** streams light data to the web UI via WebSocket.
- **Desktop output:** SDL2 or terminal for visual preview. Desktop also serves as a high-speed processing node, driving lights via ArtNet/DDP over the network.

Each driver child reads from the Drivers container's output buffer. Everything before the Drivers container is platform-independent.

Network-based drivers (ArtNet, E1.31, DDP) must pace their packet output — never blast all universe packets in a tight loop. Requires both FPS limiting (skip frames if called too fast) and inter-packet delay (microsecond pause between universe packets within a frame). Without pacing, receivers drop packets and the output appears broken.

## UI integration (light domain)

The web UI (specified in [moonmodules/core/ui.md](moonmodules/core/ui.md)) is domain-neutral — it renders any MoonModule tree generically. The light domain plugs into that UI in three places.

### Fixed top-level tree

The light pipeline pins its top-level shape in `main.cpp` — the UI shows it but cannot reorder these roots. The order matches the data flow (input → render → output):

```text
Layouts
  └─ GridLayout (or other layouts)
Layers
  └─ Layer
       └─ effects (NoiseEffect, RainbowEffect, …)
       └─ modifiers (MirrorModifier, …)
Drivers
  └─ ArtNetSendDriver
  └─ PreviewDriver
```

System modules (Filesystem, System, Network, HttpServer) sit alongside the light pipeline at the same level — they're independent of the domain but always present. Child reorder *within* a parent (an effect within a Layer, a driver within Drivers, a Layer within Layers) is supported via drag-and-drop; root reorder is not.

### 3D preview channel

[PreviewDriver](moonmodules/light/drivers/PreviewDriver.md) streams the rendered grid to the UI over the WebSocket binary channel that `core/ui.md` defines (leading type byte + domain payload). The light-domain frame format is:

```
[0x02] [dw16] [dh16] [dd16] [ow16] [oh16] [od16] [R G B …]
```

A 13-byte header: type byte `0x02`, three downsampled grid dims `dw/dh/dd`, three original grid dims `ow/oh/od` (all little-endian uint16). The RGB triples that follow describe the `dw × dh × dd` downsampled grid in `(z, y, x)` order. The original dims drive the UI's `decompress` hint — block-replicate back to the full grid for the WebGL renderer.

The UI renders this frame as a WebGL point cloud: one point per non-black voxel, interleaved float vertex buffer `[x, y, z, r, g, b]`, depth-corrected `gl_PointSize`. Orbit camera (mouse-drag, wheel-zoom, single-finger touch, two-finger pinch). Sticky position below the status bar, scroll-shrink 0→50% over 300px of page scroll. Transparent WebGL clear so the canvas blends into either theme.

The header carrying *both* downsampled and original dims is what makes this light-specific: a domain with no spatial grid (e.g. an audio synth domain) would use a different type byte and a different header. The UI dispatches by leading byte.

### Emoji-key assignments

The UI's chip filter (see [core/ui.md § Type picker](moonmodules/core/ui.md#type-picker)) treats `tags()` as opaque graphemes — each domain assigns its own meanings. The light-domain assignments follow the [MoonLight emoji key](https://moonmodules.org/MoonLight/moonlight/overview/#emoji-key) with two projectMM-specific additions (layer role, creator):

| Category | Emoji | Meaning | Source |
|---|---|---|---|
| Role | 🔥 / 💎 / 🚥 / ☸️ / 🥞 / ⚙️ | effect / modifier / layout / driver / layer / generic | derived in UI from `role` |
| Dimensional | 📏 / 🟦 / 🧊 | 1D / 2D / 3D — for effects: which axes it iterates; for modifiers: which axes it can transform | derived in UI from `dim` |
| Origin | 🐙 / 💫 / ⚡️ | WLED / MoonLight / FastLED | `tags()` |
| Creator | 🦅 | David Jupijn / Rising Step | `tags()` |
| Audio reactivity | ♫ / ♪ | FFT / volume | `tags()` |
| Moving-head | 🚨 / 🗼 | colour / movement | `tags()` |

Role and dimensional emoji are derived (not stored in `tags()`) so the same character isn't repeated in every module's header. Origin, creator, audio, and moving-head emoji live in the module's `tags()` flash string literal.

## Rebuild Propagation

When a control value changes on a layout, the pipeline must rebuild: every Layer rebuilds its LUT, and the Drivers container reallocates its output buffer. When a modifier control changes, only the affected Layer's LUT is rebuilt (the output buffer size doesn't change). This propagation is built into the framework — not handled by ad-hoc dirty flag checks in the application entry point.

The current mechanism: `Scheduler::rebuild()` re-runs `onAllocateMemory()` across the module tree, which re-evaluates layout-derived dimensions, LUTs, and buffer sizes. The HTTP handlers that change layout/modifier order or structure trigger `rebuild()` after the mutation.

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

When memory is sufficient, the system uses double buffering with a separate physical buffer, mapping, blending, and producer/consumer parallelism — same as devices with PSRAM. ESP32 without PSRAM has shown to handle this configuration up to 16K LEDs (128×128 measured live; see [performance.md](performance.md)).

When memory is too tight for double buffering, the system degrades to: one Layer with 1:1 unshuffled mapping, effects write directly into that Layer's buffer, drivers read from that same buffer to fill DMA/UDP packets directly. No blend+map step, no mapping, no blending, no parallelism. This degraded path is what would let an even larger installation fit on an ESP32 without PSRAM; the full-pipeline path already covers the 16K range.

## Memory Strategy

All buffers are allocated as single contiguous blocks outside the hot path — at startup or when configuration changes (e.g. LED count, layout size, layer count). They are then reused every frame with zero allocations in steady state. Measured per-module timing and memory for each platform: [performance.md](performance.md).

### Buffer types

- **Layer buffers.** One per active effect layer. Each holds the logical light data for one effect. Allocated in PSRAM when available. On memory-constrained devices, consumers may read from the layer buffer directly (no mapping, no blending, no physical buffer needed).
- **Physical buffer.** When present, holds the blended+mapped output. The logical and physical buffers together form the double buffer for producer/consumer parallelism.
- **Mapping LUT.** The flat lookup table for logical→physical coordinate mapping. Read-only during rendering. PSRAM is fine — sequential reads are cache-friendly.

All buffers are raw `uint8_t*` arrays sized by `channelsPerLight * nrOfLights`. This supports RGB (3 channels), RGBW (4 channels), and multi-channel DMX fixtures (up to 32 channels per light) without separate code paths. Channel layout is configured via offsets (see MoonLight's [LightsHeader](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Layers/LightsHeader.h) pattern).

Network input (ArtNet receive, WebSocket) is processed synchronously at a defined point in the frame loop. This means zero extra buffers and no race conditions. The trade-off is up to one frame of latency (~16ms at 60fps), which is imperceptible for LEDs.

### Adaptive allocation rules

The system checks available heap before each allocation and degrades gracefully when memory is insufficient. A minimum reserve (`HEAP_RESERVE = 32KB`) is preserved for stack, HTTP, WiFi, and overhead.

**Mapping LUT** is created only if ALL of:
- Modifiers exist on the layer
- Layout is not a simple non-serpentine grid (where physical == logical)
- Enough heap available after reserving HEAP_RESERVE

**Driver output buffer** is created only if:
- At least one layer has a mapping LUT actually allocated (not per-layer — if ANY layer has a LUT, the driver needs the output buffer)
- Enough heap available

**Degradation cascade** (from best to worst):
1. **Full pipeline** — LUT + driver output buffer. Modifier applied, clean separation.
2. **Skip LUT + driver buffer** — modifier not applied, forced 1:1 mapping. No intermediate buffers. (A LUT without a driver buffer to map into is useless — they are always skipped together.)
3. **Reduce layer dimensions** — halve width/height until buffer fits, minimum 8×8.

Each degradation is observable via `lutSkipped()` and reported in `/api/system` per-module metrics.

**Invariants** (non-negotiable):
- Effects ALWAYS write to their layer's logical buffer. Never to output, never to physical coordinates.
- Drivers ALWAYS owns the output path (blending, mapping, brightness correction, channel reordering).
- Layer buffer is mandatory — if it doesn't fit, reduce dimensions until it does ("at least see something").

### Per-module memory reporting

Every MoonModule reports `classSize()` (sizeof the class instance) and `dynamicBytes()` (heap allocated during `onAllocateMemory`). These are visible in `/api/system`, console output, and scenario tests. Memory scenarios verify that 1:1 pipelines use zero intermediate buffers and that the degradation cascade triggers at correct thresholds.

### Scaling to available memory

| Device | Memory | Typical capability |
|--------|--------|--------------------|
| ESP32 + OPI PSRAM | 2-8 MB | Many layers, 10K+ LEDs |
| ESP32, no PSRAM | ~320 KB internal | Full pipeline: double buffering, mapping, blending, parallelism. Proven up to 16K lights (128×128 measured live on Olimex; see [performance.md](performance.md)). The degraded-fallback path (single Layer, 1:1 direct, no blending) is reserved for installations that grow beyond what the full pipeline fits. |
| Teensy 4.x | 1 MB internal, no PSRAM | Comfortable headroom for several layers; excellent DMA-based LED output (OctoWS2811). Ethernet optional (4.1 has it built-in; 4.0 doesn't). |
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
- MappingLUT (1:N CSR, oneToOne fast path) — [testing.md#mappinglut](testing.md#mappinglut)
- BlendMap (logical→physical via LUT) — [testing.md#blendmap](testing.md#blendmap)
- NoiseEffect (spatial variation, differs from rainbow) — [testing.md#noise](testing.md#noise)
- MirrorModifier (logical dims, corner/centre mapping, dedup) — [testing.md#mirror](testing.md#mirror)

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
