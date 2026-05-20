# Plan: 3D WebGL Preview (Item 5b)

## Context

Add a PreviewDriver that streams binary light data via WebSocket, and a 3D point-cloud renderer in the browser UI. This gives visual feedback without needing hardware — see the noise/rainbow/mirror pattern in 3D in the browser.

## Design

### How the PreviewDriver sends binary frames

The PreviewDriver is a DriverBase (like ArtNetSendDriver) — it reads from the source buffer in `loop()`. But it needs to send data to WebSocket clients, which are owned by HttpServerModule.

Options: (a) PreviewDriver gets a pointer to HttpServerModule, (b) a shared broadcast function, (c) PreviewDriver builds the frame and a callback sends it.

Cleanest: **HttpServerModule exposes a `broadcastBinary(data, len)` method.** The PreviewDriver gets a pointer to HttpServerModule (set in main.cpp wiring). This is similar to how DriverGroup gets a Layer pointer. HttpServerModule is a system service — drivers that need network output reference it.

But wait — this couples a light-domain driver to a core module. That's the same issue we just fixed. Better: add a generic broadcast interface. But that's over-engineering for one use case.

Pragmatic approach: **PreviewDriver stores a function pointer** `void(*)(const uint8_t*, size_t)` set by the caller. HttpServerModule provides the function. No #include needed between them — just a function pointer set in main.cpp.

Actually even simpler: **HttpServerModule already runs loop1s() for state push. Add binary preview push to the same loop.** HttpServerModule already has access to the Scheduler, which has the Layer. It can read the output buffer directly and send binary frames. No PreviewDriver needed — just a toggle control on HttpServerModule.

Wait — that violates the architecture. The DriverGroup owns the output buffer and the blend+map step. HttpServerModule reading the buffer directly bypasses the pipeline.

Best approach: **PreviewDriver as a real driver in DriverGroup.** It builds the binary frame in its `loop()` and stores it in a member buffer. HttpServerModule checks for this buffer in its `loop20ms()` (or a faster rate) and broadcasts it. The connection: HttpServerModule finds the PreviewDriver via the Scheduler's generic `childCount()`/`child()` tree — no light domain includes needed.

Actually this is too complex. Let me go with the simplest thing that works:

**PreviewDriver builds frames. HttpServerModule broadcasts them.** They're connected via a shared pointer to a frame buffer. Main.cpp sets it up.

Simplest concrete approach:
1. A global/shared `struct PreviewFrame { uint8_t* data; size_t len; bool ready; }` 
2. PreviewDriver writes to it in `loop()`
3. HttpServerModule reads from it in `loop20ms()` and broadcasts

This is essentially a single-slot producer/consumer with no lock (single-threaded scheduler).

## Files

```
src/light/PreviewDriver.h         # NEW: builds binary preview frames
src/core/HttpServerModule.h       # MODIFY: add binary frame broadcast
src/ui/app.js                     # MODIFY: add WebGL 3D renderer
src/ui/style.css                  # MODIFY: add canvas styling
src/main.cpp                      # MODIFY: wire PreviewDriver
```

## Implementation Steps

### Step 1: PreviewFrame shared struct

Add to a small header or inline in PreviewDriver:
```cpp
struct PreviewFrame {
    uint8_t* data = nullptr;
    size_t len = 0;
    bool ready = false;
};
```

Allocated once at setup, reused every frame. Single writer (PreviewDriver), single reader (HttpServerModule).

### Step 2: PreviewDriver

`src/light/PreviewDriver.h` — single-file MoonModule, DriverBase.

- Control: `fps` (uint8_t, default 20, range 1-60)
- `setup()`: allocate frame buffer (7 header + w*h*d*3 data)
- `loop()`: FPS-limited. Build frame: header `[0x02][w16][h16][d16]` + RGB data from source buffer. Set `ready = true`.
- Frame format matches v1: 7-byte header + flat RGB.
- Gets grid dimensions from Layer (via DriverGroup → Layer → width/height/depth). But PreviewDriver only has the source buffer, not the Layer. Solution: store width/height/depth in the PreviewDriver, set when buffer is passed.

Actually, the driver needs the dimensions to build the header. Options:
- Pass dimensions when setting source buffer (add to DriverBase interface? No, that changes existing drivers)
- PreviewDriver gets a pointer to the Layer (like DriverGroup does)
- Store dimensions alongside the frame buffer

Simplest: PreviewDriver stores `w`, `h`, `d` set by the caller in main.cpp or by DriverGroup. DriverGroup already knows the Layer's dimensions. Add a `setDimensions(w, h, d)` method on PreviewDriver, called from DriverGroup::onAllocateMemory().

But that requires DriverGroup to know about PreviewDriver specifically... No. Better: add dimensions to the DriverBase interface or pass them generically.

Cleanest: `PreviewDriver` has public `lengthType width, height, depth` fields set in main.cpp. When grid changes, the HttpServerModule's `onAllocateMemory` rebuild (which calls all modules) will handle it. Actually main.cpp can just set them once and they match the grid.

Even simpler: **PreviewDriver reads from the physical output buffer (same as ArtNet driver).** The physical buffer IS the grid layout. PreviewDriver knows the grid size because it's set in main.cpp. For this commit, hardcode or pass as constructor args.

Actually — let me just make it work: PreviewDriver stores a PreviewFrame pointer, dimensions, and an fps control. Main.cpp sets up the shared frame and passes it to both PreviewDriver and HttpServerModule.

### Step 3: HttpServerModule — binary broadcast

Add `sendWsBinaryFrame()` (same as `sendWsTextFrame` but opcode `0x82`).

Add `setPreviewFrame(PreviewFrame*)`. In `loop20ms()`, if `frame->ready`, broadcast to all WebSocket clients and set `ready = false`.

### Step 4: WebGL 3D renderer in app.js

Add to `src/ui/app.js`:
- Detect binary WebSocket messages (`evt.data instanceof ArrayBuffer`)
- Parse 7-byte header for dimensions
- Build WebGL point cloud: interleaved [x,y,z,r,g,b] float array
- Orbit camera with mouse drag + wheel zoom
- Auto-sized point rendering

Add canvas element to `src/ui/index.html`.
Add canvas styling to `src/ui/style.css`.

### Step 5: Wire in main.cpp

```cpp
PreviewFrame previewFrame;
previewDriver.setPreviewFrame(&previewFrame);
httpServer.setPreviewFrame(&previewFrame);
```

## Verification

1. `cmake --build build` — zero warnings
2. `ctest` — all tests pass
3. `./build/mmv3` → open http://localhost:8080 → see 3D preview canvas
4. Noise effect visible as colored point cloud, mirror creates kaleidoscope pattern
5. Mouse drag orbits, wheel zooms
6. ESP32 build compiles
7. Platform boundary check passes
