# Plan: HTTP Server + WebSocket + Web UI (Items 5+6a)

## Context

Add HTTP server MoonModule, WebSocket for real-time state push, and a basic Web UI with tree view and auto-rendered controls. Enables effect/modifier switching from the browser and live scenario testing via HTTP API. Port 80.

## Files

```
src/platform/
  platform.h                          # MODIFY: add TcpServer + TcpConnection
  desktop/
    platform_desktop.cpp              # MODIFY: BSD socket implementations
  esp32/
    platform_esp32.cpp                # MODIFY: lwIP socket implementations (same API)
src/core/
  Scheduler.h                         # MODIFY: add moduleCount()/module(i) accessors
  HttpServerModule.h                  # NEW: HTTP + WebSocket + REST API + JSON state
src/light/
  LayoutGroup.h                       # MODIFY: add layout(i) accessor
  Layer.h                             # MODIFY: add effectCount/effect(i)/modifierCount/modifier(i)
  DriverGroup.h                       # MODIFY: add driverCount/driver(i)
src/ui/
  index.html                          # NEW: minimal HTML5 with sidebar + cards
  app.js                              # NEW: WebSocket, tree render, control render, debounce
  style.css                           # NEW: dark theme per ui-spec
src/main.cpp                          # MODIFY: wire HttpServerModule
test/
  test_http_server.cpp                # NEW: JSON state, control setter, HTTP parse
  CMakeLists.txt                      # MODIFY: add test
```

## Implementation Steps

### Step 1: Platform — TcpServer + TcpConnection

Add to `platform.h`:
```cpp
class TcpConnection {
    explicit TcpConnection(int fd);
    bool valid() const;
    int read(uint8_t* buf, size_t maxLen);  // non-blocking, -1 = nothing, 0 = closed
    bool write(const uint8_t* data, size_t len);
    void close();
    // Move-only
};

class TcpServer {
    bool open(uint16_t port);
    TcpConnection accept();  // non-blocking
    void close();
};
```

Desktop: BSD sockets with `O_NONBLOCK`, `SO_REUSEADDR`, `listen(backlog=8)`.
ESP32: same lwIP socket API.

### Step 2: Scheduler + container accessors

`Scheduler.h`: add `moduleCount()`, `module(i)` — one-liners.

`LayoutGroup.h`: add `layout(i)` accessor.
`Layer.h`: add `effectCount()`, `effect(i)`, `modifierCount()`, `modifier(i)`.
`DriverGroup.h`: add `driverCount()`, `driver(i)`.

### Step 3: HttpServerModule

`src/core/HttpServerModule.h` — single-file MoonModule, ~400 lines.

- Control: `port` (uint16_t, default 80)
- `setup()`: open TcpServer on port
- `loop20ms()`: accept connection, parse HTTP, route, respond, close (or upgrade to WebSocket)
- `loop1s()`: push state JSON to WebSocket clients

**REST API:**
- `GET /` → index.html
- `GET /app.js` → app.js
- `GET /style.css` → style.css
- `GET /api/state` → JSON module tree with controls
- `POST /api/control` → set value: `{"module":"Noise","control":"scale","value":8}`

**JSON state format:**
```json
{"modules": [
  {"name": "LayoutGroup", "controls": [], "children": [
    {"name": "Grid", "controls": [{"name":"width","type":"uint8","value":128,"min":1,"max":127}]}
  ]},
  {"name": "Layer", "controls": [], "children": [
    {"name": "Noise", "controls": [{"name":"scale","type":"uint8","value":4,"min":1,"max":32}]},
    {"name": "Mirror", "controls": [{"name":"mirrorX","type":"bool","value":true}]}
  ]},
  {"name": "DriverGroup", "controls": [], "children": [
    {"name": "ArtNet", "controls": [{"name":"ip","type":"text","value":"192.168.1.70"}]}
  ]}
]}
```

**Tree walking:** HttpServerModule gets explicit pointers (`setLayoutGroup`, `setLayer`, `setDriverGroup`) — concrete, type-safe, no virtual children interface needed.

**WebSocket:** RFC 6455 upgrade on `GET /ws`. SHA-1 + base64 for handshake (~60 lines). Fixed array of 4 `TcpConnection` clients. State push via text frames in `loop1s()`. Server→client only; client mutations via REST POST.

**Static file serving:** `fopen`/`fread` from `uiPath_` (configurable, default `"src/ui"`). Content-type by extension.

### Step 4: Web UI

`src/ui/index.html` (~80 lines):
- Status bar with WebSocket dot (green/gray)
- Side nav listing root modules
- Main area for module cards with controls

`src/ui/app.js` (~200 lines):
- `connectWs()` → `ws://host/ws`, auto-reconnect
- `handleState(data)` → selective DOM update (not full rebuild)
- `renderControl(ctrl)` → slider (uint8 with min/max), checkbox (bool), text input (text), number (uint16)
- `sendControl(module, control, value)` → POST /api/control
- 150ms slider debounce, 500ms text debounce
- `dragTs` per control to prevent WS updates overwriting active drags

`src/ui/style.css` (~100 lines):
- Dark theme: bg `#1a1a2e`, text `#e0e0e0`, accent `#a78bfa`
- Module cards, responsive sidebar, system-ui font

### Step 5: Wire into main.cpp

```cpp
mm::HttpServerModule httpServer;
httpServer.setName("HttpServer");
httpServer.setScheduler(&scheduler);
httpServer.setLayoutGroup(&layoutGroup);
httpServer.setLayer(&layer);
httpServer.setDriverGroup(&driverGroup);
scheduler.addModule(&httpServer);
```

Print `HTTP server → http://localhost:80` at startup.

### Step 6: Tests

`test/test_http_server.cpp`:
- JSON state contains expected module names and control values
- Control setter: set via name, verify bound variable changed
- HTTP request line parsing
- WebSocket accept key computation (SHA-1 + base64)

## What's NOT in this commit

- 3D WebGL preview (5+6b)
- Type picker / module creation from UI
- Drag reorder
- Config persistence
- Module add/remove from UI
- ESP32 asset embedding (serve from disk only)

## Verification

1. `cmake --build build` — zero warnings
2. `ctest --output-on-failure` — all tests pass
3. `./build/mmv3` → open http://localhost:80 → see module tree with controls
4. Change effect control (e.g. Noise scale slider) → ArtNet output changes
5. WebSocket connection dot is green
6. Platform boundary check passes
7. ESP32 build still compiles (TcpServer added to esp32 platform too)
