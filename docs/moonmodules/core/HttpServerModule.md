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
- **Server → client binary frames:** two paths, both with no frame-sized buffer.
  - **Synchronous stream** — `beginBinaryFrame(totalLen)` / `pushBinaryFrame(data,len)` / `endBinaryFrame()`: `begin` sends the WS header (16-bit, or the 64-bit form above 64 KB) to every client; each `push` fans a payload slice to every client; `end` returns whether every client got the whole frame. For a forward-only producer that builds the payload as it goes (PreviewDriver's coordinate table and downsampled colour frames, walked from `forEachCoord`). Each push spins `writeSome` a bounded number of times for the lwIP buffer to drain, then closes a client that can't keep up. These frames are small/infrequent, so the bounded spin is fine.
  - **Resumable buffered send** — `sendBufferedFrame(header, headerLen, body, bodyLen)`: for a payload that lives in a **stable caller-owned buffer** (PreviewDriver's full-res colour frame, whose body is the driver buffer). The header is copied; `body` is a pointer the caller keeps stable. One WS message is then **drained a memory-adaptive chunk per client per `loop20ms`** via `writeSome` — so a large frame is delivered over wall-clock ticks **without spinning any loop**, yet stays one atomic WS message to the browser. One send in flight at a time: a new `sendBufferedFrame` while one is active is **dropped** (newest-wins backpressure → the producer reads "link busy"). `bufferedSendIdle()` reports when the previous frame finished draining; `cancelBufferedSend()` abandons an in-flight send before its `body` is freed (a geometry rebuild). The chunk size comes from `maxAllocBlock()` so a tight board takes small bites (bounded tick cost) and a roomy board drains fast.
- **Client → server:** none. Mutations go through the REST API.

Both paths are domain-neutral (the server doesn't interpret the bytes). The resumable drain runs on **`loop20ms` (the 20 ms transport-poll), deliberately NOT the per-render-tick `loop()`** — pushing preview bytes to the socket must not be charged to the LED render hot path. The LED path (the driver output) is never delayed by the preview; the preview frame rate is instead bounded by the 20 ms drain cadence (a few fps at large full-res frames, higher for small grids), which is the right trade since the preview is a *view* and the LEDs are not. The resumable path lets a 128²+ full-res frame stream on a slow link without stalling the device: the effective frame rate self-limits (the next frame waits for `bufferedSendIdle()`), so the link sheds frame rate gracefully instead of freezing. When the two-core render/transport split lands ([architecture.md § Parallelism](../../architecture.md#parallelism)) the drain moves to the transport core and the cadence limit lifts — `loop20ms` is already that seam.

## Cross-domain wiring

HttpServerModule is core infrastructure with **no** light-domain dependencies — no `PreviewFrame`, no light types, no light includes. It exposes the `BinaryBroadcaster` interface (the synchronous `beginBinaryFrame` / `pushBinaryFrame` / `endBinaryFrame`, the resumable `sendBufferedFrame` / `bufferedSendIdle` / `cancelBufferedSend`, and `clientGeneration`); the light-domain `PreviewDriver` holds a `BinaryBroadcaster*` and streams each frame's bytes through it. `main.cpp` wires `PreviewDriver`'s broadcaster to the HttpServerModule instance — the only file that knows both. The preview's point budget and wire format are PreviewDriver's concern, documented there.

## Prior art

### projectMM v1 — HttpServer + WsServer ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/core/HttpServer.h))

HTTP via cpp-httplib (PC) / ESPAsyncWebServer (ESP32). WebSocket on separate port 81.

### projectMM v2 — HttpServerModule + WebSocketModule ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/network/HttpServerModule.h))

Separate MoonModules for HTTP and WebSocket. projectMM combines them into one module.

## Source

[HttpServerModule.cpp](../../../src/core/HttpServerModule.cpp) · [HttpServerModule.h](../../../src/core/HttpServerModule.h)
