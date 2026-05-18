# Plan: First Minimal Working System

## Context

We have CLAUDE.md and docs/architecture.md defining the system design.
No code exists yet. This plan builds the first end-to-end working system
on macOS desktop: effects rendering into layers, mapped and blended into
output, sent via ArtNet, controllable via a web UI in the browser.

## What We're Building

A desktop (macOS) application that:
- Runs a render pipeline: effect → producer buffer → blend+map → consumer buffer → driver
- Has one layer with a switchable effect (2 effects available)
- Has switchable layouts (grid + wheel) with controls for dimensions
- Sends ArtNet UDP output (driver MoonModule)
- Can receive ArtNet UDP as an effect (fills a producer buffer like any effect)
- Provides an embedded web UI (HTTP server MoonModule) to change effects,
  layouts, and control values
- Everything is a MoonModule — effects, layouts, drivers, HTTP server
- Every step includes tests for what was built

## Directory Structure

```
src/
  core/                    ← domain-neutral: MoonModule, controls, scheduler
    modules/               ← system MoonModules (HTTP server, WiFi, mDNS)
  platform/                ← platform abstraction
    desktop/
  light/                   ← light domain: buffers, mapping, blending
    modules/               ← light MoonModules
      effects/
      layouts/
      modifiers/
      drivers/
  app/                     ← entry points
  ui/                      ← web UI assets (index.html, app.js, style.css)
test/
  core/                    ← core tests
  light/                   ← light domain tests
```

## Implementation Steps

### Step 1: Build System + Platform + Test Framework
- `CMakeLists.txt` — root CMake for desktop build, including test target
- Target: `mmv3` executable, C++20, `-Wall -Wextra -Werror`
- `src/platform/Platform.h` — platform detection
- `src/platform/desktop/PlatformDesktop.cpp` — alloc, free, millis, micros
- Test framework setup (doctest or Catch2, header-only)
- `test/core/test_platform.cpp` — verify alloc/free, millis/micros work

### Step 2: MoonModule Base + Controls
The core building block. Domain-neutral — knows nothing about pixels.

- `src/core/MoonModule.h` — base class with setup/loop/teardown, controls
- `src/core/Control.h` — named value with type (uint16, bool, text).
  Fixed-capacity array per MoonModule, no heap per control.
  onChange notifies the owning MoonModule.
- `test/core/test_moonmodule.cpp` — lifecycle (setup/loop/teardown called
  in order), control add/get/set, onChange notification
- `test/core/test_controls.cpp` — control types, value bounds, name lookup

### Step 3: Light Domain Types
Light-specific types, separate from core.

- `src/light/Pixel.h` — `struct RGB { uint8_t r, g, b; }` with constexpr
  blending, scale8. 3 bytes.
- `src/light/Coord3D.h` — `struct Coord3D { uint16_t x, y, z; }`.
- `src/light/Buffer.h` — contiguous pixel array. Allocate/free via
  `platform::alloc`/`platform::free`. Exposes `std::span<RGB>`.
- `src/light/MappingLUT.h` — flat CSR-style lookup table supporting
  1:0, 1:1, 1:N. Allocated outside hot path.
- `src/light/Noise.h` — integer Perlin noise (2D/3D).
- `test/light/test_pixel.cpp` — RGB construction, scale8, blend, HSV
  conversion, static_assert(sizeof == 3)
- `test/light/test_buffer.cpp` — allocate, clear, fill, operator[],
  span access, reallocate on size change
- `test/light/test_mapping.cpp` — 1:0 skip, 1:1 direct, 1:N mirror,
  rebuild on config change
- `test/light/test_noise.cpp` — deterministic output, range bounds

### Step 4: Layouts (MoonModules)
Each layout is a MoonModule that produces a mapping LUT.

- `src/light/modules/layouts/GridLayout.h` — controls: width, height, depth.
  1:1 mapping. onChange rebuilds LUT.
- `src/light/modules/layouts/WheelLayout.h` — controls: numSpokes, ledsPerSpoke.
  Mapping with 1:0 gaps between spokes. onChange rebuilds LUT.
- `test/light/test_grid_layout.cpp` — correct LUT for various dimensions,
  LUT rebuild on control change
- `test/light/test_wheel_layout.cpp` — correct gaps between spokes,
  correct spoke pixel positions

### Step 5: Effects (MoonModules)
Each effect is a MoonModule that writes pixels into a producer buffer.

- `src/light/modules/effects/RainbowEffect.h` — controls: speed.
  Integer hue-to-RGB. Writes based on frame count + coordinate.
- `src/light/modules/effects/NoiseEffect.h` — controls: speed, scale.
  Uses Noise.h for 2D/3D Perlin patterns.
- `src/light/modules/effects/ArtNetReceiveEffect.h` — controls: universe, port.
  Reads ArtNet UDP packets synchronously, writes pixel data into
  producer buffer like any other effect.
- `test/light/test_rainbow.cpp` — produces non-zero pixels, deterministic
  for same frame number, respects speed control
- `test/light/test_noise_effect.cpp` — produces varied output, respects
  scale control

### Step 6: Pipeline + Scheduler
Wires layers together. Manages the render loop.

- `src/light/Layer.h` — owns a producer buffer, a current effect
  MoonModule, a current layout MoonModule, and a MappingLUT.
- `src/core/Scheduler.h` — runs MoonModule lifecycle. Calls setup,
  loop, teardown. Manages frame timing. Domain-neutral.
- Blend+map is performed by consumers: each driver MoonModule walks the
  layers, applies their LUTs, and blends into its own output.
- `test/core/test_scheduler.cpp` — correct lifecycle ordering, frame
  timing, multiple MoonModules
- `test/light/test_layer.cpp` — effect writes to buffer, layout builds
  LUT, layer wires them together
- `test/light/test_blend_map.cpp` — blend+map produces correct output
  for 1:0/1:1/1:N, multiple layers blend correctly

### Step 7: Drivers (MoonModules)
Consumers that read from layers and push to output.

- `src/light/modules/drivers/ArtNetSendDriver.h` — controls: destIP, universe.
  Blend+maps from layers into ArtNet packets. Sends UDP.
  Multiple universes for >170 pixels.
- `test/light/test_artnet_send.cpp` — correct packet format, correct
  universe splitting, correct pixel data in packets

### Step 8: System MoonModules + Web UI
System services as MoonModules.

- `src/core/modules/HttpServerModule.h` — controls: port.
  Embedded HTTP server, polls in loop(). Serves UI assets.
  REST API: GET /api/state, POST to switch effects/layouts/controls.
- `src/ui/index.html`, `src/ui/app.js`, `src/ui/style.css` —
  MoonModule-driven UI. Auto-renders controls. No frameworks.
  Zero changes needed when adding new MoonModules.
- `test/core/test_http_server.cpp` — starts, responds to GET, serves
  correct content type, API returns valid JSON

### Step 9: Application Entry Point
- `src/app/main_desktop.cpp`:
  1. Create scheduler
  2. Load system MoonModules (HTTP server)
  3. Create one layer with default effect (Rainbow) and layout (Grid 16x16x1)
  4. Create ArtNet send driver
  5. Main loop: scheduler.tick() at target FPS

## Verification

1. `cmake -B build && cmake --build build` — compiles without warnings
2. `cmake --build build --target test` — all tests pass
3. `./build/mmv3` — runs, starts HTTP server
4. Open browser → see UI with controls
5. Switch effect between Rainbow and Noise → visual change in ArtNet output
6. Switch layout between Grid and Wheel → mapping changes
7. Change grid dimensions → LUT rebuilds
8. Verify ArtNet output with Wireshark (UDP packets on port 6454)
9. ArtNet receive effect: send ArtNet to the app, see it appear as a layer
10. `uv run scripts/check/check_platform_boundary.py` — passes
