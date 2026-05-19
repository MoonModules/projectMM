# HttpServerModule

System MoonModule. Embedded HTTP server serving the web UI and REST API.

## Controls

- `port` (Uint16, default 8080)

## Endpoints

- `GET /` — serves index.html
- `GET /app.js`, `/style.css` — serves UI assets from disk
- `GET /api/state` — JSON of full module tree (layers, effects,
  modifiers, layouts, drivers with their controls)
- `POST /api/control/{type}/{moduleIndex}/{controlIndex}` — set a
  control value. Body: `{"value": ...}`
- `POST /api/effect/{layerIndex}/{effectIndex}` — switch active effect
- `POST /api/modifier/add/{layerIndex}/{modifierIndex}` — add modifier
- `POST /api/modifier/remove/{layerIndex}` — clear all modifiers

## Implementation

- Uses platform `TcpServer` abstraction (non-blocking BSD sockets).
- Accepts one connection per `loop()` call.
- Reads full request (headers + body via Content-Length).
- Sends `Cache-Control: no-cache` to prevent stale assets.
- Serves UI files from disk (desktop). For ESP32, would need embedded
  assets (not implemented).

## What worked

- MoonModule pattern works for system services (controls for free).
- Non-blocking accept/read means loop() returns quickly when idle.
- REST API is simple and functional.
- `findModule` with type/index path works across all module types.

## What needs improvement

- HTTP parsing is minimal — no chunked encoding, no keep-alive, no
  multipart. Sufficient for local control UI but fragile.
- POST body reading requires multiple read attempts with retries.
  The body often arrives in a separate TCP packet from headers.
- JSON building uses raw snprintf into a fixed buffer (4096 bytes).
  Will overflow with many modules. Needs a proper JSON builder or
  streaming approach.
- `AppState` struct is a grab-bag of pointers. The HTTP server needs
  to know the full module tree, but AppState couples it tightly to
  the app's wiring.
- No WebSocket support. The UI polls every 1 second. WebSocket would
  enable real-time updates and reduce latency.
- No authentication. Fine for local network, not for internet.
- UI assets served from disk path — not portable to ESP32 without
  embedding assets as C arrays.

## Prior art

### projectMM v1 — HttpServer + WsServer ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/core/HttpServer.h))
HTTP via cpp-httplib (PC) / ESPAsyncWebServer (ESP32). WebSocket separate.

### projectMM v2 — HttpServerModule + WebSocketModule ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/network/HttpServerModule.h), [source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/network/WebSocketModule.h))
Separate MoonModules for HTTP and WebSocket. HTTP uses PalHttp ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/pal/PalHttp.h)), WebSocket uses PalWs ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/pal/PalWs.h)). PC: cpp-httplib + raw POSIX WebSocket. ESP32: ESPAsyncWebServer + AsyncWebSocket.
