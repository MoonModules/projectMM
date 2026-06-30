# HueDriver

Drives **Philips Hue lights as a projectMM output** — the bulbs are *pixels of an effect*, not entries in a device list. Make a small grid (e.g. 4×1×1), run any effect on it, add a HueDriver, and each colour bulb in the driver's window becomes one pixel: the effect's per-pixel colour is pushed to the bridge as hue/saturation/brightness. It is a [driver](../../../architecture.md) like any other (a sibling of [NetworkSendDriver](NetworkSendDriver.md) / [RmtLedDriver](RmtLedDriver.md)) — it reads its slice of the shared buffer and sends it out, here over the Hue HTTP API instead of a wire protocol.

![A HueDriver in the UI](../../../assets/light/drivers/Hue%20driver.png)

## What makes Hue different (and why the driver is shaped this way)

Hue is an HTTP hub, not a strip, and these properties drive the design:

- **It's rate-limited** (~10 commands/s across the bridge; true real-time needs the [Hue Entertainment API](https://developers.meethue.com/develop/hue-entertainment/) — DTLS streaming at ~25 Hz, a separate future). So the driver paces itself: **`loop()` does at most one bridge PUT every `kPutIntervalMs`**, gated by a `platform::millis()` check (never work-every-tick), round-robined across the lights. A single bounded ~ms PUT can't stall a frame; the rest of `loop()` returns instantly. This is **smooth ambient colour**, the standard API's sweet spot — not fast strobing.
- **Only colour-capable, reachable lights are driven.** The bridge reports each light's capabilities; the driver keeps only lights whose state has a `hue` field (an "Extended color light") *and* `reachable:true`, so every window pixel maps to a bulb that can actually show the effect's colour right now. A dimmable-only white, an on/off plug, or a powered-off bulb is skipped.
- **Transitions are smoothed by the bridge.** Each PUT carries a `transitiontime` matched to how often that light is refreshed (lights × interval), so the bulb *glides* from its current colour to the next instead of snapping — the bridge's built-in fade, tuned to the cadence. (The Hue default of 400 ms is too long for this rate and smears into a frozen look.)
- **The shared output Correction applies**, same as the physical LED / network drivers: the global brightness slider and a swapped colour-order preset reach the Hue lights too (each pixel runs through the brightness LUT + channel reorder before RGB→HSV). Brightness 0 → black → the light turns off.
- **It needs an app key** (a "username"): the user presses the bridge's physical link button once, then the device claims a key. The driver runs this as a short bounded poll across a few 1 Hz ticks — never blocking the loop waiting for the press.
- **It's plain HTTP, no TLS.** The Hue v1 API answers over `http://<bridge>/api/...`, so there is no certificate handling on the device. Bench-confirmed against a BSB002 bridge (API 1.77).

![An effect driving the Hue lights](../../../assets/light/drivers/Hue%20friendly%20effect.png)

## Controls

- `bridgeIp` — the Hue bridge's LAN IPv4 (the [Control](../../core/Control.md) IPv4 type). Find it via the bridge's app, the router, or `https://discovery.meethue.com`.
- `appKey` — the Hue app key (username). Filled automatically by `pair`, persisted as the driver's credential; can also be pasted if you already have one.
- `pair` — a **button**: press it, then press the bridge's link button within ~30 s. The driver POSTs to the bridge until the press is registered, stores the returned key into `appKey`, and learns the bridge's light list.
- `room` / `light` — two **dropdowns** that narrow *which* colour lights the driver drives. Both default to `All`. Pick a `room` (extracted from the bridge's `/groups`, `type == "Room"`) to drive only that room's colour lights; the `light` dropdown then lists just that room's lights, so picking one drives a single bulb. `room = All, light = All` drives every colour light (the default). The dropdowns store the selected **index** (Hue's room/light *order* — stable on a settled bridge; `All` is always index 0 so the common case never shifts); the option *names* are regenerated from the bridge each time the page reads `/api/state`, they are not persisted.
- `start` / `count` — the window of the shared buffer this driver drives ([the standard driver window](NetworkSendDriver.md); `count` 0 = to the end of the buffer). Window index *i* maps to the *i*-th **driven** light — i.e. the *i*-th light of the current `room`/`light` filter, not the raw bridge list.
The module's generic **status** line carries the driver state: `unpaired`, `pairing: press the bridge button`, `pairing timed out`, or — once paired — `paired, M lights` (M = colour-capable, reachable lights on the bridge), shown as `paired, N-M lights` when the `room`/`light` filter narrows the driven set to N of M. Size a grid layout (N×1×1) to the driven count to map every driven bulb to a pixel.

Once paired, the driver also **lists the bridge in [DevicesModule](../../core/DevicesModule.md)** (alongside discovered WLED / projectMM peers), carrying its name and the colour-light count, refreshed on a slow cadence. It registers through `DevicesModule::active()` — the same static-accessor seam [AudioModule](../../core/AudioModule.md) uses for `latestFrame()`, so the light-domain driver reaches the core module without a structural dependency.

![The Hue bridge listed in the Devices module](../../../assets/core/Hue%20device%20disco.png)

## Wire contract (Hue v1 API, plain HTTP)

The driver talks to the bridge over `platform::httpRequest` (declared in `src/platform/platform.h` — a synchronous LAN HTTP GET/PUT/POST helper that reads the response straight into the caller's buffer):

- **Pair** — `POST http://<bridgeIp>/api` with `{"devicetype":"projectMM#device"}`. Before the link button is pressed the bridge returns `link button not pressed`; after, `[{"success":{"username":"<key>"}}]` — the `<key>` becomes `appKey`.
- **List lights** — `GET http://<bridgeIp>/api/<appKey>/lights` → a JSON object keyed by light id (`{"1":{…},"2":{…}}`). A real bridge's response runs several KB; the driver scans it for colour-capable (`"hue"` in state) + reachable (`"reachable":true`) lights and maps window index → light id.
- **List rooms** — `GET http://<bridgeIp>/api/<appKey>/groups` → a JSON object keyed by group id (`{"1":{"name":…,"lights":["1","2"],"type":"Room"},…}`). The driver scans it for `"type":"Room"` entries, reads each room's `name` and `lights` id array, and records which colour lights belong to each room (a bitmask over the colour-light list). This feeds the `room`/`light` dropdowns and the driven-set filter.
- **Set a light** — `PUT http://<bridgeIp>/api/<appKey>/lights/<id>/state` with `{"on":true,"bri":0-254,"hue":0-65535,"sat":0-254,"transitiontime":N}` (or `{"on":false,…}` when the pixel is black). `hue`/`sat`/`bri` come from a textbook integer RGB→HSV of the pixel; `transitiontime` (deciseconds) is the cadence-matched fade.

## Prior art

The [Hue v1 CLIP API](https://developers.meethue.com/develop/hue-api/) (link-button pairing, `/lights/<id>/state`, `transitiontime`). The effect-as-output mapping (bulbs as pixels of the render buffer, driven through the window) is projectMM's own — the same shape its UDP and LED drivers use.

## Source

[HueDriver.h](../../../../src/light/drivers/HueDriver.h)
