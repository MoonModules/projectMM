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
                                    classSize, dynamicBytes, controls[]
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
                                    /movex → 404. Triggers Scheduler::rebuild()
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
- **Server → client binary frames:** a domain-defined preview channel. Today only the light domain emits frames — leading byte `0x02`, 13-byte header (`dw/dh/dd/ow/oh/od`, little-endian uint16), RGB triples. Pushed by `loop20ms()` when `PreviewFrame::ready` is set by [PreviewDriver](../light/drivers/PreviewDriver.md). See [architecture.md § Web UI](../../architecture.md#web-ui).
- **Client → server:** none. Mutations go through the REST API.

The preview broadcast uses a single non-blocking scatter-gather write (`TcpConnection::writeChunks` — one `writev`/`sendmsg`) so the render task never blocks on a slow browser. `Complete` and `WouldBlock` both keep the connection open; `Partial` or socket error drops the connection and the browser auto-reconnects. PreviewDriver downsamples the frame to ≤1849 voxels so the whole WebSocket message fits lwIP's TCP send buffer (`CONFIG_LWIP_TCP_SND_BUF_DEFAULT` = 11520 B on ESP32).

## Cross-domain wiring

HttpServerModule is core infrastructure with no light-domain dependencies. It walks the module tree via the generic `MoonModule::childCount()` / `child()` interface. For the 3D preview it reads from a `PreviewFrame` (`src/core/PreviewFrame.h`) — a plain struct with a `const uint8_t*` pointer + dimensions, no light types. The light-domain `PreviewDriver` writes to `PreviewFrame`; HttpServerModule reads it. The wiring happens in `main.cpp`, which is the only file that knows both domains.

## Prior art

### projectMM v1 — HttpServer + WsServer ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/core/HttpServer.h))

HTTP via cpp-httplib (PC) / ESPAsyncWebServer (ESP32). WebSocket on separate port 81.

### projectMM v2 — HttpServerModule + WebSocketModule ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/network/HttpServerModule.h))

Separate MoonModules for HTTP and WebSocket. v3 combines them into one module.
