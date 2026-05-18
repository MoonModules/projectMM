# Plan: First Minimal Working System

## Context

We have CLAUDE.md and docs/architecture.md defining the system design.
No code exists yet. This plan builds the first end-to-end working system
on macOS desktop: effects rendering into a layer, mapped to a physical
buffer, output via ArtNet, controllable via a web UI in the browser.

## What We're Building

A desktop (macOS) application that:
- Runs a render pipeline: effect → layer buffer → blend+map → physical buffer → driver
- Has one layer with a switchable effect (2 effects available)
- Has switchable layouts (grid + wheel) with controls for dimensions
- Sends ArtNet UDP output (driver)
- Can receive ArtNet UDP as an effect (fills a layer buffer like any effect)
- Provides an embedded web UI to change effects, layouts, and control values

## Key Design Insight

ArtNet receive is a MoonModule (effect type) — it writes incoming pixel
data into a layer buffer, just like any other effect. ArtNet send is a
MoonModule (driver type) — it reads from the physical buffer and sends UDP.

## Implementation Steps (bottom-up)

### Step 1: Build System
- `CMakeLists.txt` — root CMake for desktop build
- Target: `mmv3` executable
- C++20, `-Wall -Wextra -Werror`
- `src/` as source tree, structured as core + platform libraries

Files:
- `/CMakeLists.txt`
- `/src/core/CMakeLists.txt`
- `/src/platform/CMakeLists.txt`
- `/src/platform/desktop/CMakeLists.txt`
- `/src/app/CMakeLists.txt`

### Step 2: Core Types
Minimal, no dependencies on platform code.

**Pixel (RGB)**
- `src/core/Pixel.h` — `struct RGB { uint8_t r, g, b; }` with `constexpr`
  blending, scale8. 3 bytes, `static_assert(sizeof(RGB) == 3)`.

**Coord3D**
- `src/core/Coord3D.h` — `struct Coord3D { uint16_t x, y, z; }`.
  Used by effects and layouts.

**FrameBuffer**
- `src/core/FrameBuffer.h` — owns a contiguous `RGB*` array. Allocate/free
  via `platform::alloc`/`platform::free`. Exposes `std::span<RGB>`.
  `allocate(size_t count)`, `clear()`, `operator[]`, `count()`, `bytes()`.
  Not templated initially — just RGB. Add RGBW later if needed.

### Step 3: Platform Desktop
Minimal platform implementation for desktop.

- `src/platform/Platform.h` — platform detection macros
- `src/platform/desktop/PlatformDesktop.cpp`:
  - `platform::alloc(size_t)` → `std::malloc`
  - `platform::free(void*)` → `std::free`
  - `platform::millis()` → `std::chrono`
  - `platform::micros()` → `std::chrono`

### Step 4: MoonModule Base + Controls
The core building block.

**Controls**
- `src/core/Control.h` — a control is a named value with a type.
  Minimal initial types: `uint16_t` (slider), `bool` (toggle),
  `char[64]` (text/IP). Controls are stored in a fixed-capacity array
  owned by the MoonModule (no heap per control). When a value changes,
  the MoonModule's `onChange(controlIndex)` is called.

**MoonModule**
- `src/core/MoonModule.h` — base class:
  ```
  class MoonModule {
      virtual const char* name() = 0;
      virtual void setup() {}
      virtual void loop() {}     // called each frame (hot path)
      virtual void teardown() {}
      virtual void addControls() {}
      virtual void onChange(uint8_t controlIndex) {}
      Control controls[8];       // fixed capacity, no heap
      uint8_t controlCount = 0;
  };
  ```
  Helper methods: `addControl(name, type, default, min, max)` returns index.

### Step 5: Mapping LUT
- `src/core/MappingLUT.h` — flat CSR-style lookup table:
  - `uint32_t* offsets` — one per logical pixel + 1 sentinel
  - `uint16_t* destinations` — flat array of physical indices
  - `allocate(logicalCount, totalDestinations)` via `platform::alloc`
  - Supports 1:0 (offset[i] == offset[i+1]), 1:1, 1:N
  - `rebuild()` triggers reallocation if sizes change

### Step 6: Layouts (MoonModules)
Each layout is a MoonModule that produces a mapping LUT.

**GridLayout** — `src/modules/layouts/GridLayout.h`
- Controls: width, height, depth (uint16_t each)
- Produces a 1:1 mapping (logical index == physical index for a simple grid)
- `onChange` rebuilds the LUT

**WheelLayout** — `src/modules/layouts/WheelLayout.h`
- Controls: numSpokes, ledsPerSpoke, radius (uint16_t each)
- Produces a mapping with 1:0 gaps between spokes
- Maps spoke LEDs to positions on a logical grid, leaving gaps unmapped
- `onChange` rebuilds the LUT

### Step 7: Effects (MoonModules)
Each effect is a MoonModule that writes pixels into a layer buffer.

**RainbowEffect** — `src/modules/effects/RainbowEffect.h`
- Controls: speed (uint16_t)
- `loop()`: writes rainbow pattern based on frame count + coordinate
- Pure integer math (hue-to-RGB via 6-sector)

**NoiseEffect** — `src/modules/effects/NoiseEffect.h`
- Controls: speed (uint16_t), scale (uint16_t)
- `loop()`: 2D/3D Perlin noise pattern using integer noise functions
- Needs `src/core/Noise.h` with integer Perlin implementation

**ArtNetReceiveEffect** — `src/modules/effects/ArtNetReceiveEffect.h`
- Controls: listenUniverse (uint16_t), listenPort (uint16_t)
- Opens a UDP socket, reads ArtNet packets
- `loop()`: copies received pixel data into the layer buffer
- Processed synchronously — checks for pending packets, doesn't block

### Step 8: Layer + Pipeline
- `src/core/Layer.h` — owns a FrameBuffer (layer buffer), a MoonModule*
  (current effect), and a MappingLUT. Holds a pointer to available layouts.
  `render()` calls effect's `loop()`.

- `src/core/Pipeline.h` — owns the physical FrameBuffer and an array of
  Layers. `frame()` method:
  1. For each layer: call `layer.render()` (effect fills layer buffer)
  2. Blend+map: walk each layer, apply its LUT, blend into physical buffer
  3. For each driver: call `driver.loop()` (reads physical buffer)

### Step 9: Drivers (MoonModules)

**ArtNetSendDriver** — `src/modules/drivers/ArtNetSendDriver.h`
- Controls: destIP (text), universe (uint16_t)
- `loop()`: reads from physical buffer, packs into ArtNet DMX packets
  (512 bytes per universe, multiple universes for >170 pixels), sends UDP
- Uses BSD sockets (platform-independent on desktop/RPi/ESP32-lwIP)

### Step 10: Web UI
Embedded HTTP server serving a simple SPA.

**HTTP Server** — `src/net/HttpServer.h/.cpp`
- Lightweight HTTP server on a configurable port (default 80)
- Desktop: BSD sockets, single-threaded, non-blocking poll in frame loop
- Serves static HTML/JS/CSS from compiled-in assets
- REST API endpoints:
  - `GET /api/state` — current layers, effects, layouts, controls
  - `POST /api/layer/{id}/effect` — switch effect
  - `POST /api/layer/{id}/layout` — switch layout
  - `POST /api/control/{module}/{control}` — set control value

**Web UI Assets** — `src/ui/`
- Single HTML file with inline JS/CSS (keep it minimal)
- Shows: current effect (dropdown to switch), current layout (dropdown),
  controls for active effect and layout (auto-rendered from control types)
- Polls `/api/state` periodically or uses simple refresh

### Step 11: Application Entry Point
- `src/app/main_desktop.cpp`:
  1. Create pipeline with physical buffer
  2. Register available effects, layouts, drivers
  3. Create one layer with default effect (Rainbow) and layout (Grid 16x16x1)
  4. Create ArtNet send driver
  5. Start HTTP server
  6. Main loop: process network → pipeline.frame() → sleep to target FPS

## File Tree (what gets created)

```
CMakeLists.txt
src/
  core/
    Pixel.h
    Coord3D.h
    FrameBuffer.h
    Control.h
    MoonModule.h
    Noise.h
    MappingLUT.h
    Layer.h
    Pipeline.h
    CMakeLists.txt
  platform/
    Platform.h
    desktop/
      PlatformDesktop.cpp
      CMakeLists.txt
    CMakeLists.txt
  modules/
    effects/
      RainbowEffect.h
      NoiseEffect.h
      ArtNetReceiveEffect.h
    layouts/
      GridLayout.h
      WheelLayout.h
    drivers/
      ArtNetSendDriver.h
    CMakeLists.txt
  net/
    HttpServer.h
    HttpServer.cpp
    CMakeLists.txt
  ui/
    index.html
    CMakeLists.txt
  app/
    main_desktop.cpp
    CMakeLists.txt
```

## Verification

1. `cmake -B build && cmake --build build` — compiles without warnings
2. `./build/mmv3` — runs, starts HTTP server
3. Open browser → see UI with controls
4. Switch effect between Rainbow and Noise → visual change in ArtNet output
5. Switch layout between Grid and Wheel → mapping changes
6. Change grid dimensions → LUT rebuilds
7. Verify ArtNet output with a receiver (e.g. another ArtNet node, or
   Wireshark to see UDP packets on port 6454)
8. ArtNet receive effect: send ArtNet to the app, see it appear as a layer
