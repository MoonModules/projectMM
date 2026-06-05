# HttpServerModule

Embedded HTTP server + WebSocket. Serves the web UI and the REST API that backs it.

> This page is the end-user / API-integrator view of the module. The C++ interface lives in [`src/core/HttpServerModule.h`](../../../src/core/HttpServerModule.h) (+ `.cpp`); facts visible there (private helpers, member layout, lifecycle methods) aren't repeated here. See [CLAUDE.md § Documentation](../../../CLAUDE.md) for the rule.

Controls: `port` (uint16_t, default 8080 on desktop / 80 on ESP32).

## REST API

```text
GET    /                          → index.html
GET    /app.js, /style.css        → UI assets
GET    /moonlight-logo.png        → header logo + favicon

GET    /api/state                 → full module tree JSON: each entry carries
                                    name, type, role, enabled, loopTimeUs,
                                    classSize, dynamicBytes, controls[],
                                    status + severity (only when set by the
                                    module; severity ∈ status/warning/error)
GET    /api/system                → fps, tickTimeUs, freeHeap, freeInternal,
                                    maxBlock, uptime
GET    /api/types                 → {types:[{name, displayName, role,
                                              docPath, tags, dim, defaults}]}
                                    name is the stable factory key
                                    ("RainbowEffect"); displayName is the
                                    role-suffix-stripped UI label ("Rainbow")

POST   /api/control               → {module, control, value}
POST   /api/modules               → {type, id?, parent_id?} — create
POST   /api/modules/{name}/move   → {to: N} — reorder to absolute index N
                                    within parent. Strict-suffix match;
                                    /movex → 404. Triggers Scheduler::buildState()
                                    so LUT-affecting reorders rebuild.
POST   /api/modules/{name}/replace → {type} — swap at the same position.
                                    Strict-suffix. Replacement starts with
                                    factory defaults; siblings + order kept.
POST   /api/reboot                → calls platform::reboot()
                                    (esp_restart on ESP32, std::exit(0) on desktop)

DELETE /api/modules/{name}        → remove module by name, teardown, rebuild
```

All JSON responses stream through a `JsonSink` — no fixed-buffer ceiling, so a tree of any size serialises correctly.

## WebSocket

`GET /ws` with `Upgrade: websocket` → RFC 6455 handshake (SHA-1 + base64). Up to 4 concurrent clients.

- **Server → client text frames:** full state JSON, pushed by `loop1s()`.
- **Server → client binary frames:** `broadcastBinary(chunks)` sends one binary WS message (FIN+binary opcode) to every connected client — it prepends the WS frame header and writes; the payload bytes are the caller's. Domain-neutral: the server doesn't know what the bytes mean. Today the only caller is the light domain's [PreviewDriver](../light/drivers/PreviewDriver.md), whose frame format (leading byte `0x02`, 13-byte header, RGB triples) lives in the driver, not here.
- **Client → server:** none. Mutations go through the REST API.

`broadcastBinary` uses a single non-blocking scatter-gather write (`TcpConnection::writeChunks` — one `writev`/`sendmsg`) so the render task never blocks on a slow browser. `Complete` and `WouldBlock` both keep the connection open; `Partial` or socket error drops the connection and the browser auto-reconnects.

## Cross-domain wiring

HttpServerModule is core infrastructure with **no** light-domain dependencies — no `PreviewFrame`, no light types, no light includes. It exposes the `BinaryBroadcaster` interface (`broadcastBinary`); the light-domain `PreviewDriver` holds a `BinaryBroadcaster*` and pushes each downsampled frame's bytes to it. `main.cpp` wires `PreviewDriver`'s broadcaster to the HttpServerModule instance — the only file that knows both. The preview's voxel budget (≤1849, fitting lwIP's TCP send buffer) and wire format are PreviewDriver's concern, documented there.

## Prior art

### projectMM v1 — HttpServer + WsServer ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/core/HttpServer.h))

HTTP via cpp-httplib (PC) / ESPAsyncWebServer (ESP32). WebSocket on separate port 81.

### projectMM v2 — HttpServerModule + WebSocketModule ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/network/HttpServerModule.h))

Separate MoonModules for HTTP and WebSocket. projectMM combines them into one module.
