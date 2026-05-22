# HttpServerModule

System MoonModule. Embedded HTTP server + WebSocket, serving the web UI and REST API. Lives in `src/core/` — domain-neutral.

## Domain boundary

HttpServerModule is core infrastructure with no light domain dependencies. It walks the module tree via the generic `MoonModule::childCount()`/`child()` interface. For the 3D preview, it uses `PreviewFrame` (`src/core/PreviewFrame.h`) — a plain struct with a `const uint8_t*` pointer and dimensions, no light types. The light-domain `PreviewDriver` writes to `PreviewFrame`; HttpServerModule reads it. The wiring happens in `main.cpp` which knows both domains.

## Controls

- `port` (uint16_t, default 8080 desktop / 80 ESP32)

## REST API

- `GET /` — serves index.html (disk on desktop, embedded C array on ESP32)
- `GET /app.js`, `/style.css` — serves UI assets
- `GET /api/state` — JSON module tree with controls. Each module entry includes `name`, `type`, `role`, `enabled`, `loopTimeUs`, and `controls[]`. Controls with `hidden` set are still emitted (UI skips rendering them).
- `GET /api/system` — system metrics: FPS, freeHeap, maxBlock, uptime
- `GET /api/types` — registered module types with their roles and factory defaults: `{"types":[{"name":"NoiseEffect","role":"effect","defaults":{"scale":4,"bpm":60}}, ...]}`. Defaults are captured by factory-creating a fresh probe instance per type and reading each bound variable's value-at-rest. The UI uses these for the reset-to-default ↺ button.
- `POST /api/control` — set control value: `{"module":"name","control":"name","value":...}`
- `POST /api/modules` — create module: `{"type":"NoiseEffect","id":"noise","parent_id":"layer"}`. Runs the new module's lifecycle in `onBuildControls()` → `setup()` → `onAllocateMemory()` order (matching `Scheduler::setup()`), then marks the parent dirty so the tree-shape change is persisted.
- `POST /api/modules/{name}/move` — reorder within parent: `{"to":N}` where N is the new absolute index in the parent's children array. Strict-suffix match — `/movex` is rejected with 404. Triggers `Scheduler::rebuild()` so LUTs depending on modifier/layout order are rebuilt; marks the parent dirty for persistence.
- `POST /api/reboot` — restart the device. Flushes pending FilesystemModule writes first (`flushPending()`) so an add-then-immediate-reboot isn't lost to the save debounce. ESP32: `esp_restart()`. Desktop: `std::exit(0)`. The 200 response races the actual restart; the UI's WS-reconnect logic handles the resulting disconnect.
- `DELETE /api/modules/{name}` — remove module by name, teardown, rebuild pipeline, mark the parent dirty so the removal is persisted

## WebSocket

- `GET /ws` with `Upgrade: websocket` → RFC 6455 handshake (SHA-1 + base64)
- Server→client text frames: JSON state push every 1 second via `loop1s()`
- Server→client binary frames: 3D preview data via `loop20ms()` when PreviewFrame is ready
- Client→server: none (mutations via REST POST)
- Up to 4 concurrent WebSocket clients

### Preview broadcast — non-blocking

The 3D preview frame is downsampled by [PreviewDriver](../light/drivers/PreviewDriver.md) (its `detail` control, ≤1849 voxels) so the whole WebSocket message fits lwIP's TCP send buffer. It is sent with a single non-blocking scatter-gather write (`TcpConnection::writeChunks` — one `writev`/`sendmsg` for the WS header + 13-byte preview header + payload). `Complete` and `WouldBlock` both keep the connection open (a backpressured browser just misses that frame — the render task never blocks). A truncated send (`Partial`) or socket error closes the connection; the browser auto-reconnects. With the small downsampled payload `Partial` is rare. The preview rate is bounded by `PreviewDriver`'s `fps` control (default 12). The 1 Hz JSON state push still uses the plain blocking write — it is small and infrequent.

## JSON state buffer

Static 4KB buffer for serializing the module tree. Current state JSON is ~700 bytes. Grows with each module and control: ~50 bytes per module, ~50 bytes per control. Estimated max with a full system (20 modules, 50 controls): ~5KB. When the buffer is outgrown, switch to streaming JSON directly to the socket (write each module as we iterate, no full buffer needed).

## Implementation

- Platform `TcpServer` abstraction (non-blocking accept, blocking read with timeout on desktop, retries on ESP32)
- JSON state builder: walks Scheduler's module list via `childCount()`/`child()`, serializes controls by type
- Control setter: finds module by name via recursive tree search, writes value through control `void* ptr`
- After control change: triggers `onAllocateMemory()` on all modules (handles LUT rebuild, buffer reallocation)
- UI assets: served from disk first (desktop development), falls back to embedded C arrays (ESP32)
- Embedded UI generated at build time via CMake (`src/ui/embed_ui.cmake`)

## Prior art

### projectMM v1 — HttpServer + WsServer ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/core/HttpServer.h))
HTTP via cpp-httplib (PC) / ESPAsyncWebServer (ESP32). WebSocket on separate port 81.

### projectMM v2 — HttpServerModule + WebSocketModule ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/network/HttpServerModule.h))
Separate MoonModules for HTTP and WebSocket. v3 combines them into one module.
