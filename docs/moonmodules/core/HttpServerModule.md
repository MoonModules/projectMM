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
                                    module; severity ∈ status/warning/error),
                                    plus userEditable:false ONLY when the module
                                    opts out of UI delete/replace (omitted = editable)
GET    /api/system                → fps, tickTimeUs, freeHeap, freeInternal,
                                    maxBlock, uptime
GET    /api/types                 → {types:[{name, displayName, role, docPath,
                                              tags, dim, acceptsChildRoles, defaults}]}
                                    name is the stable factory key
                                    ("RainbowEffect"); displayName is the
                                    role-suffix-stripped UI label ("Rainbow");
                                    acceptsChildRoles is the comma-separated child
                                    roles this type accepts (""=none); defaults is
                                    captured from a fresh probe instance per type

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
- **Server → client binary frames:** `broadcastBinary(chunks)` stages one binary WS message (FIN+binary opcode) for non-blocking fan-out to every connected client — it prepends the WS frame header (16-bit length, or the 64-bit form above 64 KB) and copies the bytes; the meaning is the caller's. Domain-neutral: the server doesn't know what the bytes mean. Today the only caller is the light domain's [PreviewDriver](../light/drivers/PreviewDriver.md), whose frame format lives in the driver, not here.
- **Client → server:** none. Mutations go through the REST API.

`broadcastBinary` **stages the frame and returns** — it never blocks the render task on the socket. The frame is held in one buffer with a per-client byte offset; `drainWsSends()` (called from `loop20ms`, the transport poll) flushes each client's remaining bytes via the non-blocking `TcpConnection::writeSome`, a slice per tick, so a frame larger than the lwIP send buffer streams across ticks instead of dropping. **Backpressure:** `broadcastBinary` returns `false` and drops a new frame while the previous one is still draining (one buffer, newest-wins) — the producer reads that as "the link can't keep up". `lastDrainTicks()` reports how long the last frame took to drain (the producer's adaptive-resolution signal). A genuinely stuck client (no progress for ~3 s) is closed so it can't freeze the stream for the others; the browser auto-reconnects. This producer (stage) → consumer (drain) split is the seam the [two-task render/transport split](../../architecture.md) will later host on separate cores.

## Cross-domain wiring

HttpServerModule is core infrastructure with **no** light-domain dependencies — no `PreviewFrame`, no light types, no light includes. It exposes the `BinaryBroadcaster` interface (`broadcastBinary` + `lastDrainTicks` + `clientGeneration`); the light-domain `PreviewDriver` holds a `BinaryBroadcaster*` and pushes each frame's bytes to it. `main.cpp` wires `PreviewDriver`'s broadcaster to the HttpServerModule instance — the only file that knows both. The preview's point budget (RAM-derived) and wire format are PreviewDriver's concern, documented there.

## Prior art

### projectMM v1 — HttpServer + WsServer ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/core/HttpServer.h))

HTTP via cpp-httplib (PC) / ESPAsyncWebServer (ESP32). WebSocket on separate port 81.

### projectMM v2 — HttpServerModule + WebSocketModule ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/network/HttpServerModule.h))

Separate MoonModules for HTTP and WebSocket. projectMM combines them into one module.

## Source

[HttpServerModule.cpp](../../../src/core/HttpServerModule.cpp) · [HttpServerModule.h](../../../src/core/HttpServerModule.h)
