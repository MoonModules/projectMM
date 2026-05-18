# Architecture

## The Problem

Drive 10,000+ addressable LEDs and DMX lighting fixtures (RGB(W) pars,
moving heads, dimmers) across multiple synchronized devices at high frame
rates. Support LED protocols (WS2812, APA102) and DMX/ArtNet for
conventional lighting fixtures. Run the same core logic on ESP32, desktop,
and Raspberry Pi. Provide a web UI and network APIs for control.

## Core vs Domain

The system has two layers:
- **Core** — MoonModule base, controls, scheduling, platform abstraction,
  system services (HTTP, WiFi, filesystem). Domain-neutral.
- **Light domain** — pixels, layers, mapping, blending, effects, layouts,
  modifiers, LED drivers, ArtNet. Built on top of the core.

These are separated as much as practical. When mixing is needed (for
performance or simplicity), it must be an explicit decision — consciously
choosing minimalism over separation, not accidentally blurring the
boundary. Use domain-neutral naming in those cases ("producer buffer"
not "LED buffer", "output driver" not "LED driver" in core interfaces)
to keep the door open for future separation.

## The Pipeline

The system is a render pipeline:

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

### 3D From the Core

The system is natively 3D. Coordinates, effects, layouts, and mappings all
operate in 3D space (x, y, z). 2D and 1D are simply the case where one or
two dimensions have size 1. There is no separate 2D mode — everything is
3D, and lower dimensions fall out naturally.

**Numeric types:**
- Coordinates (x, y, z) and dimensions (width, height, depth): `int16_t`.
  Supports negatives for effects that run out of bounds. Max 32767 per axis.
- Pixel indices in storage (LUT destinations): `uint16_t`. Covers up to
  65K pixels per LUT. Saves memory on constrained devices.
- Pixel counts and iteration indices: `uint32_t`. Stack variables only,
  no storage cost. Supports >65K when needed.

Use these types consistently to avoid casting. If >65K LUT destinations
are needed (large hub75 panels), introduce a `PixelIndex` typedef.

### LayoutGroup

A **LayoutGroup** (MoonModule) groups layouts and defines the physical
topology of the light installation. It is shared across all layers and
drivers — there is one LayoutGroup describing the physical setup, and
multiple layers render into it. When a layout changes, all layers rebuild
their LUTs.

Note: the term "fixture" is reserved for DMX lighting fixtures (pars,
moving heads, etc.) — it is not used for layout grouping.

### Layouts

A layout (MoonModule) defines the physical positions of lights in 3D
space. It is a **coordinate iterator** — it yields (physicalIndex, x, y, z)
for each light it defines. A layout does not own or build any mapping LUT.

Layouts cover both addressable LEDs and DMX fixtures. An LED strip layout
yields one coordinate per LED. A DMX fixture layout yields one coordinate
per fixture (a moving head is one point in 3D space).

Layout positions are computed algorithmically, not stored in memory.
The default is a grid, but any geometry is possible: spheres, rings,
cones, spirals, arbitrary point clouds.

Layouts are grouped inside a LayoutGroup. A LayoutGroup can contain
multiple layouts (e.g. a LED strip section + a row of par lights).

### Layers

A **layer** owns:
- A **buffer** — the pixel data effects write into (logical space)
- A **mapping LUT** — built by the layer from the LayoutGroup's layouts
  and the layer's static modifiers
- **Effects** (MoonModules, ordered list) — write pixels into the buffer
- **Modifiers** (MoonModules, ordered list) — transform the LUT or pixels

A layer can have **multiple effects**. Effects are not blended — they
write to the buffer sequentially in the order they are listed. Each
effect overwrites or adds to what the previous effect wrote. This allows
layering patterns (e.g. a base color effect followed by a sparkle effect).

A layer can have **multiple modifiers**. Modifiers run in order, each
taking the result of the previous modifier as input. This means the order
matters: mirror-then-rotate produces different output than rotate-then-mirror.
Static modifiers chain during LUT build, dynamic modifiers chain during
rendering.

Each layer references the shared LayoutGroup. The layer builds its own LUT
by iterating the LayoutGroup's layout coordinates and applying its static
modifiers in order. Different layers can have different modifiers, producing
different LUTs from the same LayoutGroup.

The number of active layers depends on available memory — a device with
PSRAM can run many layers; a device without may be limited to one.

### Effects

Effects produce pixel colors. They write into the layer's buffer, which
represents a logical grid. The layer determines the buffer's dimensions
(width, height, depth) from the LayoutGroup and its own start/end
position and modifiers. Effects receive these logical dimensions and
elapsed time (millis) as their rendering context. They compute pixel
positions from the buffer index (e.g. `x = i % width`, `y = i / width`).

Effects use elapsed time for animation, not frame count. This makes
animation speed independent of frame rate — an effect looks the same at
30fps and 60fps. For multi-device sync, the leader synchronizes elapsed
time across devices (same approach as syncing a frame counter, but
frame-rate independent).

Effects know nothing about hardware, protocols, physical LED layout, or
mapping. They only see the logical grid the layer provides.

### Modifiers

A modifier (MoonModule) lives inside a layer alongside its effects.
Modifiers expose a virtual interface — the Layer calls modifier methods
without knowing the concrete type (no dynamic_cast).

A modifier can:
- Transform the mapping LUT via a virtual `transformCoord()` method
  (rebuilt on the cold path, zero render cost).
- Transform pixels via a virtual `transformPixels()` method on the hot
  path (per-pixel cost, enables dynamic animations like rotation).

### Mapping and Blending

The blend+map step walks each layer in turn: it reads each logical pixel,
uses that layer's LUT to find the physical position(s), and blends the
color into the physical output buffer. This is where logical space meets
physical space.

Each mapping LUT is a flat, contiguous lookup table allocated outside the
hot path (at startup or when the layout configuration changes).

The LUT supports three mapping types without dynamic allocation:
- **1:0** — logical pixel is unmapped (skipped).
- **1:1** — logical pixel maps to one physical position (direct or shuffled).
- **1:N** — logical pixel maps to multiple physical positions (mirroring,
  cloning). Stored as a flat index array, not nested vectors.

Because mapping and blending happen together in a single pass over each
layer, there is no intermediate "mapped but unblended" buffer. The physical
buffer is the only output-side allocation.

### DriverGroup

A **DriverGroup** (MoonModule) groups output drivers. It is the consumer
side of the pipeline. The DriverGroup owns a shared output buffer and
performs blend+map from all layers into it each frame. Individual drivers
then read from this buffer to push to hardware/network.

The shared output buffer is necessary because blend+map writes to
arbitrary physical positions (via LUT) — the output is not filled
sequentially. A driver cannot read chunk-by-chunk until the full buffer
is populated. Direct-to-DMA/packet optimization would only work for the
trivial case (1:1 sequential mapping with no modifiers), which is rare.

Each driver (MoonModule) speaks one protocol:
- **LED drivers:** WS2812 via RMT, APA102 via SPI. Platform-specific.
- **DMX/ArtNet:** sends DMX data over UDP. Supports both addressable LEDs
  and conventional DMX fixtures (pars, moving heads, dimmers).
- **Preview:** streams pixel data to the web UI via WebSocket.
- **Simulation:** SDL2 or terminal output for desktop development.

Drivers read from the DriverGroup's output buffer. Everything before
the DriverGroup is platform-independent.

Network-based drivers (ArtNet, E1.31) must pace their packet output —
never blast all universe packets in a tight loop. Requires both FPS
limiting (skip frames if called too fast) and inter-packet delay
(microsecond pause between universe packets within a frame). Without
pacing, receivers drop packets and the output appears broken.

### Controls

Every MoonModule (effect, modifier, layout, driver) exposes **controls** —
runtime-configurable parameters visible in the web UI. Examples:
- A grid layout exposes width, height, depth.
- An ArtNet driver exposes destination IP and universe.
- A fire effect exposes speed, cooling, sparking.

Controls are linked to MoonModule class variables. The default value of
the variable is the default value of the control when the module is
created. When a control value changes, the system notifies the owning
MoonModule so it can react (e.g. a layout size change triggers a LUT
rebuild).

Controls are shown dynamically: when a control value changes, the
control set can be rebuilt. For example, if a control selects a mode,
the controls belonging to that mode are shown while others are hidden.

Controls are the bridge between the UI and the engine. The web UI renders
them automatically based on what MoonModules declare. The exact control
types (slider, toggle, color picker, text input, dropdown) are defined
in the UI spec (`docs/modules_draft/core/ui-spec.md`). The principle is:
MoonModules declare what they need, the UI renders it.

### Rebuild Propagation

When a control value changes on a layout, the pipeline must rebuild:
layers rebuild their LUTs, the DriverGroup reallocates its output buffer.
When a modifier control changes, only the affected layer's LUT is
rebuilt (the output buffer size doesn't change). This propagation must be built into the framework — not
handled by ad-hoc dirty flag checks in the application entry point.

The mechanism (observer pattern, signal/slot, or centralized pipeline
manager) will be defined in the module spec before implementation.

### MoonModules

The core building block is a **MoonModule**. Everything is a MoonModule
— not just effects, modifiers, layouts, and drivers, but also system
services: HTTP server, WebSocket server, file server, WiFi, mDNS, OTA
updates. The core itself is minimal: MoonModule base, buffer management,
and a scheduler. Everything else is loaded as a MoonModule.

This means:
- All MoonModules share the same class structure, lifecycle (setup, loop,
  teardown), and controls. Learn the pattern once, apply it everywhere.
- System services get controls for free — HTTP port, WiFi SSID, mDNS
  hostname are all configurable through the same UI as effect parameters.
- Capabilities are modular — don't need WiFi? Don't load the WiFi
  MoonModule. No `#ifdef`s needed.
- System MoonModules that listen (HTTP, WebSocket) poll in their `loop()`
  — the standard pattern for embedded servers.
- The scheduler handles init-order dependencies between system MoonModules
  (e.g. WiFi before HTTP, HTTP before WebSocket).

Each MoonModule is documented in `docs/modules/` as it is built.

### Why this design?

- Effects don't know about hardware → portable and composable.
- Modifiers can transform mappings (LUT rebuild, zero per-pixel cost) or
  transform pixels on the hot path (e.g. dynamic rotation — slower but
  supported when needed).
- Layouts are algorithmic → memory-efficient for large installations.
- Mappings are data (a LUT) → loadable, savable, editable, shareable.
- Map-on-the-fly → no intermediate buffer, half the memory, better cache.
- Drivers are thin → new protocols are easy to add.
- MoonModule model → low barrier for contributors.
- Zero allocations in steady state. Buffers are (re)allocated outside the
  hot path — at startup or when configuration changes.

## Parallelism

On multi-core systems (ESP32 has 2 cores, desktop/RPi have many), the
pipeline exploits parallelism by assigning MoonModules to specific cores.

The key split is **producers vs consumers**:
- **Producers** (effects) generate pixel data into layer buffers (logical
  space). Pinned to one core.
- **Consumers** (drivers — LED output, ArtNet send) perform blend+map from
  logical buffers into their output, and push to hardware/network. Pinned
  to another core. Consumers own the blend+map step because they know
  what they consume from.

The logical and physical buffers **are** the double buffer: producers
write into logical buffers while consumers read from the physical buffer.
No additional buffers are needed. At the frame boundary, roles swap via
atomic pointer swap — the driver transmits frame N while effects render
frame N+1.

When memory is sufficient (even on ESP32 without PSRAM for small
layouts up to ~4K pixels), the system uses double buffering with a
separate physical buffer, mapping, blending, and producer/consumer
parallelism — same as devices with PSRAM.

Only when memory is too tight for double buffering (large layouts on
devices without PSRAM) does the system fall back: one layer with 1:1
unshuffled mapping, effects write directly into their layer buffer,
drivers read from that same buffer to fill DMA/UDP packets directly.
No blend+map step, no mapping, no blending, no parallelism. This is
how the 12K LED stretch goal is achieved on ESP32 without PSRAM.

Each MoonModule can declare a core affinity. The scheduler respects this
when pinning tasks. On single-core or desktop systems, core affinity is
ignored and everything runs on available threads.

## Memory Strategy

All buffers are allocated as single contiguous blocks outside the hot path
— at startup or when configuration changes (e.g. LED count, layout size,
layer count). They are then reused every frame with zero allocations in
steady state.

### Buffer types

- **Layer buffers.** One per active effect layer. Each holds the logical
  pixel data for one effect. Allocated in PSRAM when available. On
  memory-constrained devices, consumers may read from the layer buffer
  directly (no mapping, no blending, no physical buffer needed).
- **Physical buffer.** When present, holds the blended+mapped output.
  Consumers may blend+map directly into DMA regions or network packet
  buffers instead, in which case a full physical buffer is not needed.
  The logical and physical buffers together form the double buffer for
  producer/consumer parallelism.
- **Mapping LUT.** The flat lookup table for logical→physical coordinate
  mapping. Read-only during rendering. PSRAM is fine — sequential reads
  are cache-friendly.

Network input (ArtNet receive, WebSocket) is processed synchronously at a
defined point in the frame loop. This means zero extra buffers and no race
conditions. The trade-off is up to one frame of latency (~16ms at 60fps),
which is imperceptible for LEDs.

### Scaling to available memory

The system adapts to what the device has:

| Device | Memory | Typical capability |
|--------|--------|--------------------|
| ESP32 + OPI PSRAM | 2-8 MB | Many layers, 10K+ LEDs |
| ESP32, no PSRAM, small layout | ~320 KB internal | Full pipeline: double buffering, mapping, blending, parallelism. Up to ~4K pixels. |
| ESP32, no PSRAM, large layout | ~320 KB internal | Fallback: single layer, 1:1 direct, no blending/parallelism. 4K-12K pixels (12K stretch goal). |
| Teensy 4.x | 1 MB internal, no PSRAM | Single/few layers, excellent DMA-based LED output (OctoWS2811), no WiFi (USB or serial control) |
| Desktop / RPi | Abundant | No constraints |

The architecture does not assume PSRAM is present. Buffer counts and sizes
are determined at runtime based on available memory. They are reallocated
when configuration changes.

## Platform Abstraction

Only abstract what you actually need. Currently that means:

- **Time.** Microsecond-resolution monotonic clock. (`esp_timer` / `std::chrono`)
- **Memory.** Allocator that prefers PSRAM on ESP32, falls back to regular
  heap. (`heap_caps_malloc` / `std::malloc`)
- **Threads.** Create a thread pinned to a specific core (ESP32 has 2),
  with mutex/semaphore primitives. (`FreeRTOS` / `std::thread`)
- **LED drivers.** Per-protocol, per-platform. RMT on ESP32, DMA/OctoWS2811
  on Teensy, SPI on RPi, SDL2 or terminal on desktop.
- **Networking.** HTTP server, WebSocket, UDP sockets. (`esp_http_server` /
  BSD sockets / platform library). Teensy: USB serial or Ethernet shield.
- **Filesystem.** Read/write config and UI assets. (`LittleFS` /
  `std::filesystem`). Teensy: SD card or flash.

Abstractions are added when needed by a concrete implementation, not
pre-designed. All platform-specific code lives in `src/platform/`.
Everything outside it compiles cleanly on every target.

## Multi-Device Sync

For installations spanning multiple controllers:

1. **Discovery.** Devices find each other via mDNS.
2. **Time sync.** One leader broadcasts its elapsed time (millis).
   Followers compute their offset. Target: sub-millisecond accuracy.
   Since effects use elapsed time for animation, synced time means
   synced visuals — regardless of each device's frame rate.
3. **Pixel distribution.** When one device needs to send pixel data to
   another, use ArtNet/E1.31 — it's the standard, no need to invent a
   protocol.

## Build System

CMake is the sole build system. The source tree is shared across all
platforms, but build entry points are separate because ESP-IDF wraps CMake
with its own conventions (`idf_component_register()` instead of
`add_library()`).

```
CMakeLists.txt              ← standard CMake: desktop/RPi build + tests
esp32/
  CMakeLists.txt            ← ESP-IDF project root (thin wrapper)
  main/
    CMakeLists.txt          ← idf_component_register() pointing at src/
```

- **Desktop/RPi:** `cmake -B build && cmake --build build` from the root.
- **ESP32:** `cd esp32 && idf.py build` — the wrapper pulls in `src/`
  from the parent directory.
- **Raspberry Pi:** cross-compile using the root CMakeLists.txt, or build
  natively on the device.

The project is structured as a set of CMake libraries:
- A core library (platform-independent logic)
- A platform library (selected at configure time)
- An application target (links both, provides the entry point)

Further decomposition (effects, networking, drivers as separate libraries)
will happen when the codebase is large enough to justify it.

## Developer Tooling

**MoonDeck** (`scripts/moondeck.py`) is a Python web server providing a
browser-based console for all project scripts. Run with `uv run scripts/moondeck.py`,
open `http://localhost:8420`.

Three tabs:
- **PC** — desktop build, run, test. Fast iteration.
- **ESP32** — chip type and USB port selection. Build, flash, monitor.
- **Live** — device discovery and monitoring. Select devices for operations.

Each script is a small card with a `?` help link and a Run button. Output
streams to a shared log window. Scripts are individual Python files in
`scripts/` — always runnable standalone via `uv run scripts/<name>.py`
as well as from MoonDeck.

Script definitions and configuration live in `scripts/moondeck_config.json`
(committed). Script documentation lives in `scripts/MoonDeck.md`, one
section per script. Runtime state (selected devices, ports) persists in
`scripts/moondeck.json` (gitignored).

## Testing

### Unit tests (desktop)

Core logic runs on desktop, so all non-hardware code is testable via
`ctest`. Priority areas:
- Color math (HSV→RGB, blending, scale8)
- Buffer operations (allocate, fill, clear, bounds)
- Mapping LUT (1:0, 1:1, 1:N, rebuild on config change)
- Blend+map pass (correct physical output from logical layers)

Test framework will be chosen when we write the first test. Preference
for something header-only and lightweight (doctest, Catch2, or plain
`assert`).

### Performance tests (desktop)

Automated checks that verify architectural rules at runtime:
- **Zero-allocation render loop.** Run N frames, intercept `malloc`/`free`
  (via overriding or platform allocator hooks), fail if any allocation
  occurs during steady-state rendering.
- **Frame time regression.** Measure render time for a known workload
  (e.g. 10K pixels, 3 layers, rainbow effect). Fail if it exceeds a
  threshold. Not a hard real-time guarantee, but catches gross regressions.

### Live system tests (on-device)

For ESP32 targets, test what desktop can't:
- LED output produces correct signal (protocol-level, verified with logic
  analyzer or known-good reference).
- Multi-device sync achieves sub-millisecond accuracy.
- Memory stays within bounds over long runs (no leaks, no fragmentation
  drift).

These are manual or semi-automated for now. Automation strategy will emerge
with the hardware.

## Linting and Static Analysis

### Compiler warnings

All targets build with `-Wall -Wextra -Werror`. No warnings allowed in CI.

### Platform boundary enforcement

An automated check (script or CI step) scans all files outside
`src/platform/` for:
- `#ifdef` / `#if defined` with platform macros
- `#include` of platform-specific headers (`esp_*`, `freertos/*`,
  `driver/*`, `SDL.h`, `wiringPi.h`, etc.)

Fails the build if any are found.

### Hot path lint

A custom check (clang-tidy plugin, script, or code review rule) flags
allocation calls (`new`, `malloc`, `make_unique`, `make_shared`,
`push_back`, `std::string` constructors) inside functions identified as
hot path (render loop and its callees). This can start as a code review
convention and become automated as the codebase grows.

### Code formatting

clang-format with a project `.clang-format` file. Applied in CI — code
that doesn't match the format fails the check. Developers can run
`clang-format` locally or via editor integration.

### When checks run

- **Pre-commit (optional):** clang-format, platform boundary check.
- **CI (mandatory):** all of the above plus full test suite.
- Exact CI setup will be configured when we set up the repository's CI
  pipeline.

## Web UI

The UI is three hand-maintained files: `index.html`, `app.js`, `style.css`.
No frameworks, no build tools, no npm. These files are served directly by
the embedded HTTP server.

The UI is **MoonModule-driven**. It does not contain hard-coded knowledge
of specific effects, layouts, or drivers. Instead, it queries the system
for the current MoonModule tree (layers, their effects, modifiers, layouts,
drivers — each with their controls) and renders them generically:

- Each MoonModule is shown with its name and its declared controls.
- Controls are auto-rendered based on type (slider, toggle, color picker,
  text input, dropdown).
- MoonModules can be switched (e.g. change which effect a layer uses)
  and linked together (e.g. assign a layout to a layer).

Adding a new MoonModule with controls requires **zero changes** to the UI
files. The UI discovers and renders whatever MoonModules the system reports.

## Documentation

Documentation describes the system as it is, not its history or future.

```
docs/
  architecture.md          ← this file (system design)
  decisions.md             ← approaches tried and rejected, lessons learned
  modules/                 ← one page per MoonModule (specs before code)
  modules_draft/           ← draft specs from prototype cycle, to be reviewed
```

Module specs are written before implementation. They define: purpose,
controls, behavior, edge cases, interactions with other modules. Doc
pages are kept current with the code. Git commits are the history.

## What We Leave Undesigned

These will be specified in module docs before implementation:

- **MoonModule interface.** Draft exists in `docs/modules_draft/core/`.
  Needs review: virtual modifier interface, rebuild propagation.
- **Config persistence.** Controls need to be saved/loaded. The storage
  format and mechanism will be specified when we build that module.
- **Web UI.** Draft spec exists in `docs/modules_draft/core/ui-spec.md`.
  Needs review: multiple layers, live preview, WebSocket.
- **File/directory structure.** The platform boundary is fixed. The rest
  will grow as modules are specified and implemented.
