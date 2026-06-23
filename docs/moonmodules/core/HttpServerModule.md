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
- **Server → client binary frames:** **streamed** with no frame-sized buffer, via `beginBinaryFrame(totalLen)` / `pushBinaryFrame(data,len)` / `endBinaryFrame()`. `begin` sends the WS header (16-bit length, or the 64-bit form above 64 KB) to every client; each `push` fans a payload slice to every client; `end` returns whether every client received the whole frame. The producer ([PreviewDriver](../light/drivers/PreviewDriver.md)) pushes straight from its source data, so neither side holds a copy of the frame. Domain-neutral: the server doesn't interpret the bytes.
- **Client → server:** none. Mutations go through the REST API.

Each push writes to every client via the non-blocking `TcpConnection::writeSome`, spinning a bounded number of times for the lwIP send buffer to drain (no sleep) before giving up on a client that can't keep up and closing it (it reconnects). `endBinaryFrame()` returning `false` is the producer's "the link couldn't take this frame" signal, driving PreviewDriver's adaptive downscale. **The send is synchronous on the caller's loop** (PreviewDriver's rate-limited preview loop, not the LED render tick): a large frame on a slow link briefly occupies that loop. Moving to a resumable cross-tick send (push what fits now, resume next loop) is the follow-up that removes that pause; see PreviewDriver.

## Cross-domain wiring

HttpServerModule is core infrastructure with **no** light-domain dependencies — no `PreviewFrame`, no light types, no light includes. It exposes the `BinaryBroadcaster` interface (`beginBinaryFrame` / `pushBinaryFrame` / `endBinaryFrame` + `clientGeneration`); the light-domain `PreviewDriver` holds a `BinaryBroadcaster*` and streams each frame's bytes through it. `main.cpp` wires `PreviewDriver`'s broadcaster to the HttpServerModule instance — the only file that knows both. The preview's point budget and wire format are PreviewDriver's concern, documented there.

## Prior art

### projectMM v1 — HttpServer + WsServer ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/core/HttpServer.h))

HTTP via cpp-httplib (PC) / ESPAsyncWebServer (ESP32). WebSocket on separate port 81.

### projectMM v2 — HttpServerModule + WebSocketModule ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/network/HttpServerModule.h))

Separate MoonModules for HTTP and WebSocket. projectMM combines them into one module.

## Source

[HttpServerModule.cpp](../../../src/core/HttpServerModule.cpp) · [HttpServerModule.h](../../../src/core/HttpServerModule.h)
