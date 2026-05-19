# projectMM v1 Inventory (commit 54b50bc, release 1.4.0)

This is a throwaway reference document — not committed. Used to decide what to harvest for v3.

## Core

| File | Purpose | Harvest for v3? |
|------|---------|-----------------|
| Module.h | Base class, name/id/type | Pattern reference |
| StatefulModule.h | Lifecycle + controls (addControl binds to class variable by reference) | **Yes** — controls pattern is proven |
| ModuleManager.h | Registry, REST adapter, state persistence, dirty-flag debouncer | Overloaded (v1 lesson) — split concerns |
| Scheduler.h | Timing, loop dispatch | Pattern reference |
| Coord3D.h | 3D coordinate type | **Yes** — simple, keep |
| HttpServer.h | HTTP server wrapper | Pattern reference |
| WsServer.h | WebSocket server | **Yes** — v3 needs WebSocket |
| KvStore.h | Key-value persistence | Pattern reference |
| Logger.h | Logging | Pattern reference |
| TypeRegistry.h | Module type registration for dynamic creation | **Yes** — needed for UI type picker |
| ProducerModule.h | Base for effects (produces into Channel) | **Yes** — maps to EffectBase |
| ConsumerModule.h | Base for drivers (consumes from Channel) | **Yes** — maps to DriverBase |
| AppRoutes.h | REST API route definitions | Pattern reference |

## PAL

| File | Purpose | Harvest? |
|------|---------|----------|
| Pal.h | Platform abstraction (alloc, timing, GPIO) | **Yes** — same concept |
| FileSystem.h | File abstraction | Pattern reference |
| MemoryStats.h | Memory reporting | Pattern reference |

## Effects (light domain)

| Module | Controls | Description | Harvest? |
|--------|----------|-------------|----------|
| NoiseEffect2D | scale(1-32), speed(0-255) | 2D Perlin noise field | **Yes** |
| DistortionWaves2DEffect | freq_x(1-8), freq_y(1-8), speed(0-100) | Wavy distortion pattern | **Yes** |
| GameOfLifeEffect | seed(0-255), wraparound(toggle), hue(0-255) | Conway's Game of Life | **Yes** |
| LinesEffect | bpm(1-240) | Moving lines | **Yes** |
| RipplesEffect | speed(0-99), interval(1-254) | Ripple animation | **Yes** |
| SineEffect | frequency(1-20), amplitude(0-255) | Sine wave pattern | **Yes** |
| ArtNetInModule | universe_start(0-255) | ArtNet receive as effect | **Yes** |

## Drivers

| Module | Controls | Description | Harvest? |
|--------|----------|-------------|----------|
| ArtNetOutModule | universe_start(0-255), ip(text) | ArtNet UDP send | **Yes** |
| PreviewModule | logEveryN(1-1000) | WebSocket preview to UI | **Yes** |

## Modifiers

| Module | Controls | Description | Harvest? |
|--------|----------|-------------|----------|
| BrightnessModifier | frequency(1-20) | Brightness pulsing | **Yes** |

## Layouts

| Module | Controls | Description | Harvest? |
|--------|----------|-------------|----------|
| Layout.h | (abstract base) | Coordinate iterator base | Pattern reference |
| GridLayout | width(1-1024), height(1-1024), depth(1-32), serpentine(toggle) | 3D grid with serpentine support | **Yes** — serpentine is important |

## Layers (pipeline structure)

| File | Purpose | Harvest? |
|------|---------|----------|
| Channel.h | 3D RGB array with w/h/d metadata | **Yes** — maps to Buffer |
| EffectsLayer.h | Container for effects in a layer | Pattern reference |
| DriverLayer.h | Container for drivers | Pattern reference |
| RGB.h | Plain RGB struct, no FastLED | **Yes** — same as our Pixel/light value |

## System modules

| Module | Description | Harvest? |
|--------|-------------|----------|
| Network.h | WiFi mode selection | **Yes** |
| WifiSta.h | WiFi station mode | **Yes** |
| WifiAp.h | WiFi access point mode | **Yes** |
| Ethernet.h | Ethernet placeholder | Later |
| FileManagerModule.h | File browser | Later |
| SystemStatus.h | Memory, uptime, version display | **Yes** |
| TasksModule.h | FreeRTOS task monitoring | Later |
| DeviceDiscovery.h | UDP broadcast discovery | **Yes** |

## Frontend (single index.html, 1909 lines)

### UI structure
- Fixed status bar: brand, device name, WebSocket status dot, reconnect button
- Hamburger menu → side nav with module tree
- Module cards with controls, add/remove buttons
- **3D WebGL preview** with orbit camera (matrix math, vertex/fragment shaders)
- Log panel
- System status (memory, timing)
- Light/dark theme toggle
- Responsive (side nav auto-opens on wide screens)

### Key UI patterns
- **WebSocket-driven** (not polling) — real-time state updates
- **Type picker** — dropdown to add new modules by type
- **Dynamic control rendering** — `buildControl()` renders slider/toggle/text/color by type
- **Module tree** — hierarchical navigation with parent/child relationships
- **Drag reorder** — modules can be reordered within their parent
- **Reset-to-default** buttons per control
- **Preview canvas** — WebGL 3D rendering of light output in real-time

### Control types supported
- slider (float range)
- toggle (bool)
- text (string, used for IP addresses)
- (color picker exists in UI code but may not be fully wired)

### API shape (REST + WebSocket)
- `GET /api/types` — list available module types
- `GET /api/modules` — list all module instances with state
- `POST /api/modules` — create new module
- `DELETE /api/modules/:id` — remove module
- `PATCH /api/modules/:id/props/:key` — set control value
- WebSocket: pushes full state on change, receives light preview frames
