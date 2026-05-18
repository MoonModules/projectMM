# Architecture

## The Problem

Drive 10,000+ addressable LEDs across multiple synchronized devices at high
frame rates. Support multiple LED protocols (WS2812, APA102, DMX/ArtNet).
Run the same core logic on ESP32, desktop, and Raspberry Pi. Provide a web
UI and network APIs for control.

## The Pipeline

The system is a render pipeline:

```
            Layer A: Effect + Modifiers → Buffer A (logical) ─ LUT A ─┐
            Layer B: Effect + Modifiers → Buffer B (logical) ─ LUT B ─┼→ Blend+Map → Physical Buffer → Drivers
            Layer C: Effect + Modifiers → Buffer C (logical) ─ LUT C ─┘
                                                                 ▲
                                                                 │
                                                           Layout (algorithmic)
                                                         defines physical positions,
                                                          shapes the mapping LUTs
```

### 3D From the Core

The system is natively 3D. Coordinates, effects, layouts, and mappings all
operate in 3D space (x, y, z). 2D and 1D are simply the case where one or
two dimensions have size 1. There is no separate 2D mode — everything is
3D, and lower dimensions fall out naturally.

### Effects and Layers

Effects produce pixel colors. An effect is a function from (frame number,
3D coordinate, parameters) to color. Effects know nothing about hardware,
protocols, physical LED layout, or mapping.

Multiple effects can run simultaneously, each writing into its own **layer
buffer** in **logical coordinate space**. Each layer also owns its own
**mapping LUT**, so different layers can have different geometries (e.g.
one layer maps to a grid, another to a ring on the same physical strip).

The number of active layers depends on available memory — a device with
PSRAM can run many layers; a device without may be limited to one.

### Modifiers

A modifier is an effect on an effect. Modifiers live inside a layer
alongside its effect. A modifier can transform the mapping LUT (rebuilt
on the cold path, zero render cost), and optionally override a pixel
function to transform coordinates or colors on the hot path (per-pixel
cost, but enables dynamic animations like continuous rotation).

### Layouts

A layout defines the physical positions of LEDs in 3D space. The default
is a grid, but any geometry is possible: spheres, rings, cones, spirals,
arbitrary point clouds.

Layout positions are **computed algorithmically**, not stored in memory.
A layout is a function from (physical index) → (x, y, z). This saves
memory — a 10K LED sphere doesn't need 30KB of stored coordinates.

When a layout changes, the mapping LUT is rebuilt. The layout function
runs only during LUT construction (cold path), never during rendering.

### Mapping and Blending

The blend+map step walks each layer in turn: it reads each logical pixel,
uses that layer's LUT to find the physical position(s), and blends the
color into the physical output buffer. This is where logical space meets
physical space.

Each mapping LUT is a flat, contiguous lookup table allocated outside the
hot path (at startup or when the fixture configuration changes).

The LUT supports three mapping types without dynamic allocation:
- **1:0** — logical pixel is unmapped (skipped).
- **1:1** — logical pixel maps to one physical position (direct or shuffled).
- **1:N** — logical pixel maps to multiple physical positions (mirroring,
  cloning). Stored as a flat index array, not nested vectors.

Because mapping and blending happen together in a single pass over each
layer, there is no intermediate "mapped but unblended" buffer. The physical
buffer is the only output-side allocation.

### Output

Drivers push pixel data from the physical buffer to hardware. Each driver
speaks one protocol (WS2812 via RMT, APA102 via SPI, ArtNet via UDP,
simulated via SDL2 or terminal). Drivers are platform-specific; everything
before them is not.

### Controls

Every MoonModule (effect, modifier, layout, driver) exposes **controls** —
runtime-configurable parameters visible in the web UI. Examples:
- A grid layout exposes width, height, depth.
- An ArtNet driver exposes destination IP and universe.
- A fire effect exposes speed, cooling, sparking.

Controls are part of the MoonModule interface — every MoonModule declares
its controls the same way. When a control value changes, the system
notifies the owning MoonModule so it can react (e.g. a layout size change
triggers a LUT rebuild).

Controls are the bridge between the UI and the engine. The web UI renders
them automatically based on what MoonModules declare. The exact control
types (slider, toggle, color picker, text input, dropdown) will emerge
from implementation, but the principle is: MoonModules declare what they
need, the UI renders it.

### MoonModules

The core building block is a **MoonModule**. Effects, Modifiers, Layouts,
and Drivers are all MoonModules — they share the same class structure,
including how they declare controls. Learn the pattern once, apply it
everywhere. The exact interface will emerge when we implement the first
MoonModules, but the principle is: all four are peers with a common
shape, not special cases of each other.

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
- **Producers** (effects, blend+map) generate pixel data into the physical
  buffer. Pinned to one core.
- **Consumers** (drivers — LED output, ArtNet send) read from the physical
  buffer and push to hardware/network. Pinned to another core.

To run producers and consumers simultaneously without blocking each other,
the physical buffer is **double-buffered** when memory allows: producers
write into one buffer while consumers read from the other, then the
buffers swap at the frame boundary (atomic pointer swap). This way the
driver can be transmitting frame N while effects are already rendering
frame N+1.

On memory-constrained devices (ESP32 without PSRAM), double buffering may
not be affordable. In that case, producers and consumers run sequentially
on the same buffer — correct but slower.

Each MoonModule can declare a core affinity. The scheduler respects this
when pinning tasks. On single-core or desktop systems, core affinity is
ignored and everything runs on available threads.

## Memory Strategy

All buffers are allocated as single contiguous blocks outside the hot path
— at startup or when configuration changes (e.g. LED count, fixture size,
layer count). They are then reused every frame with zero allocations in
steady state.

### Buffer types

- **Layer buffers.** One per active effect layer. Each holds the logical
  pixel data for one effect. Allocated in PSRAM when available. When
  memory is tight, a layer buffer may share the physical buffer directly
  (single-layer mode, no blending overhead).
- **Physical buffer.** The final output buffer matching the physical LED
  layout. Layer buffers are blended into this. Drivers (ArtNet, LED
  output, etc.) read directly from the physical buffer — no separate
  staging or driver buffers.
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
| ESP32, no PSRAM | ~320 KB internal | Single layer (shared with physical buffer), 4K+ LEDs (12K stretch goal with minimal other processes) |
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
- **LED drivers.** Per-protocol, per-platform. RMT on ESP32, SPI on RPi,
  SDL2 or terminal on desktop.
- **Networking.** HTTP server, WebSocket, UDP sockets. (`esp_http_server` /
  BSD sockets / platform library)
- **Filesystem.** Read/write config and UI assets. (`LittleFS` / `std::filesystem`)

Don't create an abstraction until you have two platforms that need it.
Don't pre-design headers for abstractions you haven't written yet.

All platform-specific code lives in `src/platform/`. Everything outside it
compiles cleanly on every target.

## Multi-Device Sync

For installations spanning multiple controllers:

1. **Discovery.** Devices find each other via mDNS.
2. **Clock sync.** One leader broadcasts timestamps. Followers compute their
   offset. Target: sub-millisecond accuracy.
3. **Frame sync.** Leader broadcasts a frame counter at each tick. Effects
   use this counter (not wall-clock time) for animation, so all devices
   produce identical output for the same frame number.
4. **Pixel distribution.** When one device needs to send pixel data to
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

## What We Leave Undesigned

These will be designed when we build them:

- **MoonModule interface.** We know effects, modifiers, layouts, and drivers
  are all MoonModules with controls. The exact base class / concept will
  emerge when we implement the first few.
- **Config persistence.** Controls need to be saved/loaded. The storage
  format and mechanism will emerge when we have controls to persist.
- **Web UI details.** Communication protocol, exact API shape, and
  interaction patterns will emerge from implementation.
- **File/directory structure.** We know the platform boundary. The rest of
  the directory tree will grow organically as we add code.

This is deliberate. Designing these now would mean guessing. We'd rather
build, learn, and then design with real information.
