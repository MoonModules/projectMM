# Architecture

## The Problem

Drive 10,000+ addressable LEDs across multiple synchronized devices at high
frame rates. Support multiple LED protocols (WS2812, APA102, DMX/ArtNet).
Run the same core logic on ESP32, desktop, and Raspberry Pi. Provide a web
UI and network APIs for control.

## The Pipeline

The system is a render pipeline with three stages:

```
Generate → Map → Output
```

**Generate.** Effects produce pixel colors. An effect is a function from
(frame number, pixel coordinate, parameters) to color. Effects know nothing
about hardware, protocols, or physical LED layout.

**Map.** A mapping translates logical coordinates (what the effect sees) to
physical positions (where LEDs actually are). This handles grids, rings,
serpentine wiring, sparse 3D point clouds — any geometry. The mapping is
data (a lookup table), not code.

**Output.** Drivers push pixel data to hardware. Each driver speaks one
protocol (WS2812 via RMT, APA102 via SPI, ArtNet via UDP, simulated via
SDL2 or terminal). Drivers are platform-specific; everything before them
is not.

### Why a pipeline?

- Each stage is independently testable.
- Effects don't know about hardware → they're portable and composable.
- Mappings are just data → they can be loaded, saved, edited, shared.
- Drivers are thin → new protocols are easy to add.
- The pipeline runs with zero allocations in steady state. Buffers are
  allocated once at setup, then reused every frame.

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

### Boundary rule

`src/platform/` contains all platform-specific code. Everything outside it
compiles cleanly on every target. This is the one non-negotiable structural
rule.

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

CMake is the sole build system.

- When `IDF_PATH` is set, the build targets ESP32 via ESP-IDF's CMake
  integration (`idf.py build`).
- Otherwise, it builds a native desktop executable.
- Raspberry Pi: cross-compile or build natively on the device.

The project is structured as a set of CMake libraries:
- A core library (platform-independent logic)
- A platform library (selected at configure time)
- An application target (links both, provides the entry point)

Further decomposition (effects, networking, drivers as separate libraries)
will happen when the codebase is large enough to justify it.

## What We Leave Undesigned

These will be designed when we build them:

- **Effect API.** We know effects are functions from (frame, coord, params)
  to color. The exact interface (concept? base class? function pointer?)
  will emerge when we write the first few effects.
- **Config system.** We know we need persistent settings. The format and
  API will emerge when we have something to configure.
- **Web UI.** We know it's embedded in the firmware. The framework, asset
  pipeline, and communication protocol will be designed when we build it.
- **File/directory structure.** We know the platform boundary. The rest of
  the directory tree will grow organically as we add code.

This is deliberate. Designing these now would mean guessing. We'd rather
build, learn, and then design with real information.
