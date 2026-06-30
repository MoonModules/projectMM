# Plan — HueDriver: Philips Hue lights as a projectMM effect output (shipped)

## Context

The product owner has Hue lights and a bridge ("Hue Ewoud", BSB002, API 1.77, at 192.168.1.143). The reframe that drives this plan, from the product owner: **Hue is an *output*, not a device to list.** projectMM already drives "an array of lights" through the effect → layout → buffer → driver pipeline; Hue maps onto that directly — a handful of bulbs are a small **grid** (e.g. 5×1×1), an **effect** runs on them, and a **`HueDriver`** (a sibling of `RmtLedDriver` / `NetworkSendDriver` in the Drivers container) reads its window of the output buffer and pushes each pixel's colour to the corresponding bulb. The bulbs are *pixels of an effect*, not rows in DevicesModule.

This is *Common patterns first* + *Concrete first, abstract later*: a new driver is the recognised unit of "a new output target," and the architecture already has the seam. No new core concept — one new `DriverBase` subclass + a small outbound-HTTP helper.

### Verified on the wire (not assumed)

- The bridge advertises `_hue._tcp` (mDNS) and answers N-UPnP — discoverable.
- **The bridge still allows the plain-HTTP Hue v1 API**: `http://<bridge>/api/0/config` → 200 (not HTTPS-only). So **no TLS / self-signed-cert handling on the ESP32** — the single biggest simplifier. CLIP v2 exists (`/clip/v2` → 403) but is HTTPS-only + event-stream; **not used**.
- Hue v1 is HTTP + JSON: `POST /api` (link-button pairing → app key), `GET /api/<key>/lights` (list), `PUT /api/<key>/lights/<id>/state` (`{"on":bool,"bri":0-254}`, optional `xy`/`hue`/`sat` later).

## Decisions locked (product owner)

- **Hue is an output driver** (`HueDriver : DriverBase`), sibling of `NetworkSendDriver`. Not a DevicesModule entry. (Listing the bridge in DevicesModule + auto-filling the driver's IP from discovery is a **follow-up**, per *concrete first* — build the working output, add the discovery nicety after.)
- **Scope: on/off + brightness** from the effect's per-pixel value (luminance → `bri`). Colour (`xy`/`hue`/`sat`) is a clean later extension on the same PUT.
- **Update model: throttled, changed-lights-only.** Hue's bridge rate-limits to ~10 commands/s/light; a real-time stream would need the Entertainment API (DTLS) — out of scope. The driver samples its window on a **slow tick** (target ≤ ~10 Hz total across its lights) and PUTs **only the lights whose colour changed** since the last push. This is the standard way apps drive Hue from animations.
- **Plain-HTTP Hue v1 API** (no TLS). The bridge IP + app key are **controls on the HueDriver** (self-contained config, like NetworkSendDriver owns its target IP/universe); a **Pair button** runs the link-button POST to fill the app key. Persisted with the module.

## Design

### 1. Outbound HTTP helper (platform seam)

The repo had `httpGet`; it was removed as dead code when DevicesModule's HTTP sweep went away. The HueDriver re-introduces the outbound-HTTP capability — but **minimal and with a real consumer this time** (the prior removal's lesson: don't keep an unused seam). Add a small `platform::httpRequest`:

```
// Outbound HTTP request to a LAN host (plain HTTP, no TLS — Hue v1 allows it). Builds the
// request, returns the status code (0 on failure), fills `body` (NUL-terminated, truncated).
// Synchronous + bounded by `timeoutMs`. Desktop + ESP32 over the existing TcpConnection
// (which has connect()/writeSome()/read()). GET/PUT/POST via `method`.
int httpRequest(const char* method, const char* host, uint16_t port, const char* path,
                const char* reqBody, uint32_t timeoutMs, char* body, size_t bodyLen);
```

- Desktop + ESP32 build it over `TcpConnection::connect()` + `writeSome()` + non-blocking `read()` (the same primitives the HTTP *server* uses). Plain socket, no libcurl, LAN HTTP only — exactly what the removed `httpGet` did, generalised to GET/PUT/POST.
- **Off the render hot path**: the HueDriver calls this on its slow tick (loop1s-cadence), never `loop()`. A bounded blocking call there is fine (same rule as the old mDNS browse / the OTA fetch).

### 2. `HueDriver : DriverBase` (`src/light/drivers/HueDriver.h`)

Header-only light module, mirroring `NetworkSendDriver`'s shape:

- **Controls** (`onBuildControls`): `bridgeIp` (IPv4), `appKey` (Text, persisted — the credential), `pair` (Button → link-button pairing), `start`/`count` (via `addWindowControls()` — its slice of the buffer). A read-only `status` line ("paired, N lights" / "press the bridge button" / "unpaired").
- **`setSourceBuffer` / `setLayer` / `setWindow`** — standard DriverBase wiring (Drivers container passes the shared buffer + the active layer for dimensions).
- **`loop()`** — does NOTHING on the render tick (Hue can't keep up; never block the hot path here). The driver's window is sampled in `loop1s()` instead.
- **`loop1s()`** (the throttle): read the window slice from the source buffer (`windowSlice()`), map each light's RGB → on/off + `bri` (luminance), and for each light whose value **changed** since the last push, `httpRequest("PUT", bridgeIp, 80, "/api/<key>/lights/<id>/state", "{\"on\":…,\"bri\":…}", …)`. A per-light `lastSent` cache (small fixed array, bounded by the window count, capped at e.g. 32 Hue lights) drives the changed-only filter. PUTs are spread (one or a few per tick) so a tick never blocks long.
- **`pair`** (Button → `onUpdate`): for ~a few seconds, `POST /api {"devicetype":"projectMM#<deviceName>"}`; on success store the returned `username` into `appKey` (persist), then `GET /api/<key>/lights` to learn the light id list. The "press the link button" instruction shows in `status`.
- **Light-id mapping**: window index → Hue light id. First cut: the lights list from `GET /api/<key>/lights` in id order maps to window indices 0..N-1. (A future control could let the user reorder / pick which bulbs.)

### 3. Drivers registration + UI

- Register `HueDriver` in the driver factory next to the other drivers (one `ModuleFactory::registerType` line) so it's addable from the UI like any driver.
- The generic UI renders its controls with zero per-driver code (the whole point of the module tree).

### 4. Desktop testability

- `httpRequest` works on desktop (real sockets) → the PUT/GET formatting + the changed-only diff + the window→light mapping are **host-unit-testable** against a tiny stub HTTP responder (or by asserting the formatted request bytes from a seam, no live bridge needed). The product owner's real bridge is the bench cross-check.

## Files

- **New:** `src/light/drivers/HueDriver.h` (the driver), `docs/moonmodules/light/drivers/HueDriver.md` (spec — controls, the Hue v1 wire contract, pairing flow, the rate-limit rationale, prior art).
- **Edit:** `src/platform/platform.h` (+ `src/platform/esp32/` + `src/platform/desktop/` impls) for `httpRequest`; the driver registration in `src/main.cpp`; `test/CMakeLists.txt` + a `test/unit/light/unit_HueDriver.cpp` (request formatting + changed-only diff + window mapping); `docs/backlog/backlog-light.md` (mark the Hue-driver item building / add the follow-ups: colour, DevicesModule bridge discovery, Entertainment-API streaming).

## Riskiest parts

1. **Rate limit / not blocking the loop.** The throttle (changed-only, ≤~10 Hz, a few PUTs per `loop1s`) must keep the bridge happy AND keep each tick short. A PUT is a bounded blocking `httpRequest` off the render path — but many lights × a slow bridge could still make `loop1s` long. Mitigation: cap PUTs-per-tick (round-robin the changed lights across ticks), short `timeoutMs` (~200 ms), and degrade gracefully on a 429/timeout (skip, retry next tick).
2. **Pairing UX is asynchronous + physical.** The user must press the bridge button within the window. The Button handler can't block the loop for seconds — so pairing runs as a short bounded poll across a few `loop1s` ticks (a small state machine: "pairing… press the button" → key obtained / timed out), status-reported. Don't block the render loop during pairing.
3. **Re-introducing outbound HTTP** — keep `httpRequest` minimal (the lesson from deleting `httpGet`: an unused seam is debt). It ships *with* its consumer (HueDriver), so it's earned.
4. **App key is a credential** — persisted in the module JSON like other settings; it's a LAN bridge key (low sensitivity), stored the same way as e.g. a static IP. Note it in the spec.

## Verification

- Desktop build (0 warnings); `ctest` incl. the new HueDriver unit test (request formatting + changed-only diff); scenarios + spec-check green; ESP32 all variants build.
- **Bench (the real test):** on the product owner's bridge — add a 5×1×1 layout, an effect, a HueDriver (window [0,5)), press Pair + the bridge button → key obtained, lights listed; the effect animates the 5 bulbs (on/off + brightness) at the throttled rate, no bridge 429s, render FPS unaffected (the PUTs are on loop1s, off the hot path).
- Save this plan (done); mark `(shipped)` when it lands.

## Out of scope (clean follow-ups)

- **Colour** (`xy` / `hue`/`sat` from the pixel RGB) — same PUT, one more field; the obvious next slice.
- **DevicesModule lists the Hue bridge** + auto-fills the driver's `bridgeIp` from discovery (the product owner's "list it in devices" idea, done as the second step — discovery feeds the output).
- **Hue Entertainment API** (DTLS streaming, ~25–50 Hz) for true real-time effect sync — a major separate feature (TLS-PSK on ESP32, entertainment-area setup, v2 API).
- **DMX lights** as another such output driver (the product owner noted this is coming — Hue maps the "array of foreign lights" pattern that DMX will reuse).
