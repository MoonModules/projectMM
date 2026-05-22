# Plan-15 — Stream /api/state JSON (fix fixed-buffer overflow)

## Context

Adding several effects to a Layer broke the whole web UI: it showed *"Error: The string did not match the expected pattern."* and rendered no module cards, even after a refresh.

Root cause: `HttpServerModule` built the entire `/api/state` JSON into a single fixed `char jsonBuf_[4096]`. With a larger module tree the JSON exceeded 4 KB; `snprintf`-based appends silently dropped individual fragments past the limit, leaving **malformed JSON** (`…value":0},}]},},},}]}]}]}` — dangling commas, broken nesting). The browser's `JSON.parse` failed and the UI never rendered. The WebSocket state push had the identical bug — a `char json[4096]` stack buffer.

This is exactly the failure the plan-12 spec note predicted: *"revisit only if the tree outgrows the JSON buffer (the documented fallback is then streaming JSON to the socket)."* This plan implements that fallback.

## Decision

Stream the state JSON with **no fixed-size ceiling**, rather than just enlarging the buffer (which only moves the cliff and costs ESP32 RAM). A `JsonSink` abstraction serves both consumers:

- **Socket mode** — a small (1 KB) staging buffer flushes to the `TcpConnection` as it fills; the whole response never lives in RAM at once. Used by `GET /api/state`.
- **Buffer mode** — bytes collect in a heap buffer that doubles on demand. Used by the WebSocket push, whose frame header needs the total length up front so it can't stream incrementally.

Either way a module tree of any size serializes correctly.

## Implementation — `src/core/HttpServerModule.h`

- New `JsonSink` class (before `HttpServerModule`): `append()` / `appendf()` write JSON; a `TcpConnection*` selects socket vs buffer mode. Socket mode auto-flushes the 1 KB stage; buffer mode grows a heap allocation (`platform::alloc`, doubling from 2 KB), freed in the destructor.
- `serveState` — writes the HTTP header directly (no `Content-Length`; `Connection: close` ends the body at EOF), then streams the tree through a socket-mode `JsonSink`.
- `buildStateJson`, `writeModuleJson`, `writeControls` — converted from `(char* buf, size_t bufSize, int& pos)` to a single `JsonSink&`. `appendf` replaces every `snprintf` + `pos`-bookkeeping pair, so the converted code is also shorter. The old "peek `buf[pos-1]` to decide a comma" trick became a `bool first` flag (streaming has no buffer to peek).
- `pushStateToWebSockets` — builds into a buffer-mode `JsonSink`, sends `sink.data()` / `sink.size()` via the unchanged `sendWsTextFrame`.
- `<cstdarg>` added for `appendf`'s varargs. The old `jsonBuf_` / `JSON_BUF_SIZE` stay — `/api/types` and `/api/system` still use them (smaller responses, not the overflow path).

## Verification

- Desktop build clean, zero warnings.
- Live: with the persisted large tree, `GET /api/state` returned **7000 bytes of valid JSON** (was truncated at 4095). Adding 10 more effects via the API pushed it to **~7 KB / 24 modules** — still valid, no truncation.
- Headless browser: the UI rendered all cards with no "pattern" error (the original symptom gone).
- WebSocket: `/ws` handshake returns `101 Switching Protocols` and pushes frames; the state push uses the same verified `buildStateJson`.
- `ctest` 1/1, `mm_scenarios` 8/8.

## Notes

- The fix removes the size ceiling entirely — there is no new larger limit to hit.
- ESP32 RAM: socket mode uses a 1 KB stage (down from the 4 KB static `jsonBuf_` for this path); buffer mode allocates transiently from PSRAM-preferred heap and frees immediately.
- Spec updated: `docs/moonmodules/core/ui.md` — `/api/state` REST entry, the WebSocket push description, and the per-root-filtering note now describe the streaming sink.
- Implemented on `next-iteration`. Pre-commit gates not run — the product owner's gate.
