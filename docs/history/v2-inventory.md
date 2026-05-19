# projectMM v2 Inventory

Throwaway reference document. Harvesting proven patterns from v2 for v3.

## Architecture

v2 ("maximize minimalism") unified v1's Module + StatefulModule into a single MoonModule class. Used a proper PAL with separate files per concern (PalHeap, PalRtos, PalUdp, PalWifi, PalFs, PalHttp, PalWs). Introduced DataBuffer/DataRegistry for lock-free producer/consumer communication.

## Source structure

```
src/
  core/          MoonModule, ModuleManager, Scheduler, DataBuffer, DataRegistry
  pal/           PalHeap, PalRtos, PalUdp, PalWifi, PalFs, PalHttp, PalWs, PalSystemInfo
  modules/
    lights/      effects, layouts, drivers, Pixelable, RGB
    network/     HttpServerModule, WebSocketModule
    system/      Logger, MemTracker, StateStoreModule, SystemStatusModule, WifiStaModule
  frontend/      index.html, app.js, style.css (separate files, unlike v1's single HTML)
```

## Gems to harvest

### 1. DataBuffer — lock-free single-slot SPSC buffer

Producer-consumer pixel buffer with atomic revision counter:
- `acquire_write()` — zero branches, zero alloc
- `publish()` — one atomic store (release)
- `try_acquire_read()` — two atomic loads; nullptr if no new frame
- `release_read()` — one atomic store

32-bit atomics are single-instruction on Xtensa. Teardown safety via `invalidate()` (sentinel value in the published_ word). Multiple consumers each have their own `DataBufferReader` tracking independent read positions.

### 2. DataRegistry — string-keyed buffer directory

Producers declare buffers in onAllocateMemory(), consumers resolve by id in setup()/loop20ms(). Hot-path cost: zero (consumers cache the pointer). Type-erased: stores void* + count + elem_size + dimensions. Domain-neutral.

### 3. MoonModule — unified class with port-and-minimize log

The massive comment block in MoonModule.h documents every v1 feature kept, added, generalized, deferred, or dropped — with rationale for each. This deliberation log is the most detailed architectural record of any projectMM version.

Key patterns:
- `onBuildControls()` — all addControl() calls here, not in setup(). Supports rebuild via clearControls().
- `onAllocateMemory()` — single hook for dynamic allocation. Module sets moduleAllocBytes_.
- `onChildrenReady()` — parent notified when all children finish setup.
- `controlAllocBytes()` — pre-check: how much would this control change allocate?
- Field order optimized for minimum padding (8B → 4B → 2B → 1B, saving 24B vs naive order).

### 4. PAL — one file per concern

Unlike v1's monolithic Pal.h, v2 splits into: PalHeap, PalRtos, PalUdp, PalWifi, PalFs, PalHttp, PalWs, PalSystemInfo. Each has ESP32 and PC implementations in the same file via `#ifdef ARDUINO`. Clean separation.

### 5. PixelEffectBase — shared effect spine

Eliminates ~70 lines of boilerplate per effect. A concrete effect implements only:
- `build_effect_controls()` — its own addControl() calls
- `render_(RGB* px, w, h, d)` — fill the buffer

The base handles: layout resolution, PSRAM pixel buffer, DataBuffer declaration/teardown, resize polling, pixelBuffer() interface. This is the pattern v3's EffectBase should follow.

### 6. Multi-core scheduler with per-module core affinity

Scheduler runs N core_loops (default 2 on ESP32). Each module declares its `coreAffinity()` (0 or 1). Effects run on core 0, ArtNet output on core 1. Uses `pal::task_create_pinned` with 8KB stacks.

### 7. Frontend — three separate files + canvas view

v2 separated the frontend into index.html (8KB), app.js (89KB), style.css (28KB). Unlike v1's single HTML.

**Canvas view (new in v2):** alongside the traditional tree view, v2 introduced a node canvas/graph view. Two view modes toggled by buttons:
- Tree view (⎇) — hierarchical list like v1
- Canvas view (⬡) — node-graph style with SVG connections and a world viewport

The canvas view shows modules as draggable nodes with connection lines, giving a visual representation of the pipeline topology. This is powerful for understanding complex setups but adds significant UI complexity.

### 8. AutoWireSpec — declarative input wiring

Modules declare their input dependencies via AutoWireSpec arrays:
```cpp
struct AutoWireSpec {
    const char* inputKey;
    const char* searchType;
    bool allMatches;
    const char* backKey;  // bidirectional wiring
};
```
ModuleManager automatically wires modules at startup based on these specs. Eliminates manual strcmp chains.

### 9. Footprint reporting — zero boilerplate

classSize set once at registration time by `ModuleManager::register_type<T>()` — no CRTP, no macro. dynamicMemorySize() = moduleAllocBytes_ + controls + children overhead. Single source of truth.

### 10. ADR 0005 — teardown liveness

DataBuffer invalidation protocol: producer calls invalidate() before freeing, which stores a sentinel into the published_ atomic. Consumers that see the sentinel return nullptr (skipped frame) instead of UAF. No new atomic, no new hot-path load.

## What NOT to harvest

- **ArduinoJson dependency** — v2 still uses ArduinoJson for controls and state. v3 should use fixed-size types.
- **std::string for module id** — 24 bytes on the heap per module. v3 should use fixed-size char[].
- **89KB app.js** — too large. v3 should keep the frontend smaller and simpler.
- **The stalling problem** — 19 sprints for release 1. The over-engineering of abstractions should not be repeated.

## Module inventory

### Effects
| Module | Description |
|--------|-------------|
| Noise2DEffect | 2D noise field |
| DistortionWavesEffect | Interfering sine waves |
| FlowFluidEffect | Fluid dynamics simulation |
| LinesEffect | BPM-synchronized lines |
| RipplesEffect | Expanding ripples |
| SineEffect | 3D sine wave |

### Layouts
| Module | Description |
|--------|-------------|
| GridLayoutModule | 3D grid with serpentine |
| RingLayoutModule | Circular ring |
| WheelLayoutModule | Bicycle wheel with spokes |

### Drivers
| Module | Description |
|--------|-------------|
| ArtnetOutModule | ArtNet UDP output |
| PreviewModule | WebSocket binary preview |

### System
| Module | Description |
|--------|-------------|
| HttpServerModule | REST API server |
| WebSocketModule | WebSocket server |
| WifiStaModule | WiFi station mode |
| SystemStatusModule | System health display |
| StateStoreModule | JSON state persistence |
| MemTracker | Memory usage tracking |
| Logger | Structured logging |
