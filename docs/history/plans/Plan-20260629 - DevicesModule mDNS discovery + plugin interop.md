# Plan — DevicesModule refactor: mDNS discovery + REST commands, plugin interop

> **Design note.** An earlier draft reached for a custom UDP presence beacon; reasoning through what foreign systems (WLED, ESPHome, Tasmota) actually do moved it to the cleaner endpoint recorded here — **mDNS for discovery, REST for commands, foreign systems behind a plugin seam.** mDNS is the standard the whole ecosystem already speaks, so a bespoke UDP beacon was a WLED-ism worth dropping.

## Context

The current `DevicesModule` (627 lines) discovers LAN devices with **two strategies merged into one list**: an mDNS *browse* every tick (a ~20 ms **blocking** `mdnsBrowse` on the render task) and a one-shot HTTP subnet sweep (a **blocking** `httpGet`, 1 IP/tick, ~4 min/.24, flickers LEDs). The product owner wants a complete refactor: drop the slow HTTP scan entirely (devices should **announce themselves**, not be polled), and make the device module our own **industry-standard interop seam** that other systems hook into as plugins.

### The design, reasoned to its endpoint

1. **Discovery = mDNS; commands split across REST and UDP by need (see the transport table).** mDNS is *the* industry discovery standard (Bonjour/Avahi) — every OS, Home Assistant, ESPHome, WLED, Tasmota, Hue speaks it. A device **announces** its service; we **passively listen** — the push paradigm "devices announce themselves" wants, on the standard the whole ecosystem already uses. **Commands are not all one transport:** must-arrive config (brightness, presets, OTA) goes over **REST** (TCP-guaranteed, ~10–50 ms is fine); latency-critical lossy-OK traffic (time sync for synchronized effects, live pixels) goes over **UDP** (~0.5–1 ms, broadcast to N — REST is 10–100× too slow there). See § Transport split.
2. **No HTTP scan — ever.** The slow part was the HTTP *subnet sweep* (active per-IP probing, blocking, flicker-prone). mDNS is the opposite: no per-IP walk, no probing — passive listen to multicast announcements. **The fix for "mDNS was slow too" is to LISTEN non-blocking, not to BROWSE blocking.** The old code's mistake was the blocking `mdnsBrowse` (a ~20 ms tick stall); a non-blocking mDNS listener (IDF `mdns_query_async_*`, poll with timeout 0) is hot-path-safe like any other poll.
3. **Foreign systems are plugins, not hardcoded branches.** A `DevicePlugin` seam (the adapter pattern, cf. `ListSource` / `ModuleFactory`) lets each ecosystem hook in: a plugin recognises a device from an **mDNS service hit** (`_wled._tcp` → WLED, `_esphome._tcp` → ESPHome, `_http._tcp`+`mm=1` → projectMM) and fills its `Device`. Adding Tasmota / NightDriverStrip later = **one new plugin file**, no core edit. The device module is projectMM's own *industry-standard hook-in point* — light software and beyond. (The control half — translate "set brightness" into a system's JSON/`cmnd`/protocol — is a reserved extension on the same seam, added when a consumer exists; *concrete first, abstract later*.)
4. **projectMM is discovered the same way it discovers — via mDNS.** It already advertises `_http._tcp` + a `mm=1` TXT (`platform_esp32.cpp` ~line 1025). No custom UDP beacon for projectMM↔projectMM: the standard advertise + listen covers it, and the same advertise makes projectMM discoverable by Home Assistant / any Bonjour client.

**Why not UDP (the WLED way)?** WLED's UDP port (21324) bundles three jobs: discovery (the historical artifact — WLED *also* has `_wled._tcp` mDNS, so its UDP discovery is redundant even there), state-sync (lossy-OK, reasonable on UDP), and realtime pixels (correct on UDP). WLED's "messages didn't arrive" pain came from treating UDP state-sync as *reliable*. We separate the jobs by transport — mDNS (discovery) + REST (must-arrive) + UDP (lossy streams only) — so we never inherit that conflation.

This refactor is mostly **subtraction**: the blocking HTTP sweep, the blocking mDNS browse, the `via` bitmask, the scan button + progress control, and the HTTP-body classifier all go; discovery becomes a non-blocking mDNS listener feeding a small plugin list. Net core lines drop.

## Sanity check against the docs

- **README / CLAUDE.md / architecture.md:** honours *Common patterns first* (mDNS-SD is THE textbook LAN discovery standard; the plugin seam is the textbook adapter pattern), *Default to subtraction* (removes the sweep + browse + classifier), *Hot path discipline* (the non-blocking mDNS poll replaces two blocking calls), *Industry standards, our own code* (mDNS the standard, our own listener + plugin model written fresh), and *Robust to any input* (an unrecognised service / malformed TXT is ignored, never crashes).
- **Supersedes** backlog-core's "mDNS preferred for discovery, UDP for streams, HTTP sweep as fallback" — we keep mDNS-for-discovery (vindicated) but drop the HTTP-sweep fallback and make mDNS a **non-blocking listener** + a plugin seam. The lesson (WLED's UDP-discovery is a historical artifact; separate transports by job) goes to `decisions.md`.
- **What it supersedes:** backlog-core's four-mechanism wording — it *keeps* "mDNS for discovery" (vindicated) but drops the HTTP-sweep fallback entirely and makes mDNS a **non-blocking listener** behind a **plugin seam**. The lessons (WLED's UDP-discovery is a historical artifact; commands split three ways by must-arrive × latency, not "all REST") go to `decisions.md`, and the backlog stance is rewritten in the same change.

## Design

### Transport split — three categories on two axes (must-arrive? latency-critical?)

The split is **not** "discovery=mDNS, everything-else=REST". Commands divide by *two independent questions* — must the message arrive, and is it latency-critical — giving three transports. The trap to avoid: treating *every* command as REST. A **config** command is REST; a **sync pulse** is UDP.

| Job | Transport | Latency (ESP32 LAN) | Why |
|---|---|---|---|
| **Discovery** | mDNS (announce + non-blocking listen) | n/a (background) | The standard the whole ecosystem announces on; passive listen, no scan. |
| **Config commands** — must-arrive, latency-tolerant (set brightness, save a preset, push config, fleet OTA) | REST `/api/control` (already built) | ~10–50 ms (TCP handshake + request/response round-trip) | Delivery must be guaranteed; 10–50 ms is invisible for a config change. TCP's ACK/retransmit is the point. |
| **Sync + live streams** — latency-critical, lossy-OK (time sync for synchronized effects, SuperSync clock, live pixel data) | UDP (already: NetworkSend/Receive) | **~0.5–1 ms** one-way, broadcast to N at once | Needs few-ms determinism + fan-out to many devices simultaneously; a dropped pulse self-corrects on the next one. **REST would be 10–100× too slow and is point-to-point** — wrong tool. |

**Why time-sync is UDP, not REST (the latency answer):** a synchronized-effect clock pulse needs sub-few-ms, deterministic delivery, **broadcast to every device at once** — and is inherently lossy-OK (the next pulse corrects drift). REST is ~10–50 ms with jitter, point-to-point (N devices = N serialized TCP exchanges), and waits for an ACK you don't want. That's the textbook UDP-lossy-stream case — the *same* category as live pixels, already reserved here. Routing sync over REST would visibly de-sync the LEDs; this is the one place "REST for commands" must **not** apply.

### Discovery: a non-blocking mDNS listener feeding a plugin list

- **Announce (already done):** projectMM advertises `_http._tcp` + a `mm=1` TXT (`platform_esp32.cpp` ~line 1025). Adding a `_wled._tcp` advertise (one more `mdns_service_add`) makes projectMM appear in the native WLED apps (iOS/Android/Desktop all browse `_wled._tcp` — same Flutter discovery; one advertise covers all three) and in Home Assistant / any Bonjour client. *Caveat:* Android's NSD is stricter than Apple Bonjour about a well-formed instance/SRV/TXT record, so the advertise must be clean `_wled._tcp` — bench-verify on Android, not just iOS.
- **Listen (the new work):** a **non-blocking mDNS listener** — start an async query (IDF `mdns_query_async_new`), poll it each `loop1s` with **timeout 0** (`mdns_query_async_get_results`), collect any results, restart. This is hot-path-safe (a non-blocking poll), replacing the old **blocking** `mdnsBrowse` (~20 ms tick stall). The module cycles through the service types its plugins care about (`_http._tcp`, `_wled._tcp`, later `_esphome._tcp`, `_hue._tcp`), one async query at a time.
- **Plugins classify each hit:** a `DevicePlugin` seam (the adapter pattern, cf. `ListSource` / `ModuleFactory`) — each plugin says which service type(s) it claims and turns an mDNS hit (service type + TXT records + resolved IP/name) into a `Device`. `_http._tcp`+`mm=1` → projectMM; `_wled._tcp` → WLED. A new system is **one new plugin file** listed in the module — no core edit. **Plugins are not all the same shape:** a flat-device plugin (WLED, ESPHome, Tasmota) yields one device per hit; a **hub** plugin (Hue) yields a *bridge* whose children (Zigbee bulbs) are enumerated + controlled via the bridge's authenticated REST API — so the seam must not bake in "flat device", and the (reserved) command half must handle a hub addressing a resource by id, with per-plugin auth state. (Hue is the canonical "more than WLED" case driving this.)

### The DevicePlugin seam (built now: discovery half; reserved: command half)

```
struct DiscoveredDevice { DevType type; char name[24]; /* hub: resource list later */ };

class DevicePlugin {
  virtual const char* name() const = 0;                    // "projectMM", "WLED", "Hue"…
  // Which mDNS service this plugin claims (e.g. "_wled","_tcp").
  virtual const char* service() const = 0;
  virtual const char* proto() const = 0;
  // Turn a resolved mDNS hit (name + TXT) into a device; false to decline.
  virtual bool fromMdns(const platform::MdnsHost& host, DiscoveredDevice& out) const = 0;
  // (reserved) virtual bool command(const DiscoveredDevice&, const DeviceCommand&) const;
};
```

Built minimal-but-real now (the **discovery** half: two concrete plugins — projectMM + WLED — proving the seam isn't shaped to one system). The **command** half (`command()` + capability/auth) is a reserved extension added when a control consumer exists; the discovery code and the module's iteration don't change. *Concrete first, abstract later.*

### What collapses (the subtraction)

- **Delete** the HTTP subnet sweep (`restartScan`, `stepScan`, `probe`/`probePort`, the per-IP blocking `httpGet`, the `scan` button, the `progress` control, `kProbe*` constants). Devices announce themselves; we never poll.
- **Delete** the HTTP-body classifier (`classifyDevice`/`extractDeviceName`/`extractStringAfter` in `DeviceIdentify.h`) — classification now comes from the mDNS hit's service type + TXT, in the plugins. `DeviceIdentify.h` shrinks to the `DevType` enum + `devTypeStr`.
- **Replace** the blocking `mdnsBrowse` browse strategy with the non-blocking listener; the `via` bitmask collapses (mDNS is the one discovery source now — `speaks` may stay for "what protocol can I talk to it with", or also go; decide at implementation).
- **Keep** the `Device` struct (trim `via`), the `ListSource` rendering, persistence (last-known list on boot), age-out, self-row — the parts that *work* and that consumers (UI, main.cpp) depend on.

### Platform (the one new seam)

- **New non-blocking mDNS listener seam.** The existing `mdnsBrowse` is *blocking*; the refactor needs `platform::mdnsListenStart(service, proto)` + `platform::mdnsListenPoll(cb)` (timeout-0 async poll), implemented on ESP32 via `mdns_query_async_*` and a desktop stub (no mDNS on host). This is the only platform addition — it's real work but it's the *right* seam (the blocking browse was the smell). The existing `MdnsHost` POD (resolved IP/hostname/port + TXT marker) is the result type; extend it with general TXT-record access if a plugin needs a TXT beyond `mm=1`.

## Why this matters — the multi-ecosystem positioning (a selling point)

The plugin seam isn't only clean architecture; it's a **differentiator**. projectMM/MoonLight becomes a **hub that discovers and controls every light on the LAN — one UI across ecosystems**, not just its own LEDs. WLED can't drive Hue; Home Assistant can but is heavyweight. A lightweight LED controller that *also* sees and steers WLED, ESPHome, and Hue from one device list is a genuine pitch. This is why "device interop must be our own industry-standard seam" (the product owner's framing) is right: the device module is the **hook-in point**, and each ecosystem is a plugin.

The seam delivers it **incrementally**, on an honest difficulty gradient (state it plainly so it's not oversold):

| Tier | Systems | What the plugin needs | Seam fit |
|---|---|---|---|
| **Easy** | WLED, ESPHome | mDNS discovery + REST/JSON commands — flat devices | Fits **today** — the seam built now |
| **Medium** | Philips Hue, IKEA Trådfri | mDNS discovery + a **hub** model (bridge → child resources) + per-plugin **auth** (Hue link-button key, Trådfri CoAP/DTLS PSK), still IP/REST | The seam is **designed to allow** this (hub-shaped `DiscoveredDevice` + auth in the command half) |
| **Hard (IP)** | Tasmota-MQTT, zigbee2mqtt, Matter | a transport the device doesn't have yet — an **MQTT client** or a **Matter stack** | "plugin **+ a transport addition**" — still clean, but a bigger lift; don't promise casually |
| **Hard (radio, board-gated)** | direct Zigbee / Thread bulbs (IKEA, raw Hue bulbs, no bridge) | the **802.15.4 radio** (S31 / C6 / H2 only) + the esp-zigbee / OpenThread stack — projectMM *is* the coordinator, talking to bulbs over the mesh directly | Same plugin philosophy, **non-IP transport + board-gated**. The biggest differentiator (a WiFi LED controller that *also* drives your Zigbee bulbs with no gateway) and the biggest lift. Far future. |

So the **promise to make**: *"pluggable — WLED, ESPHome, and Hue-class systems hook in as plugins; MQTT/Matter and direct-radio Zigbee need a transport addition first (the radio one only on 802.15.4 boards)."* The Easy tier ships with this refactor (WLED + projectMM); the rest are future plugin files (+ a transport for the Hard tiers), each landing without a core change. That incrementality — *one plugin at a time, no core churn* — is what makes "controls the whole LAN's lights" a credible roadmap. **The S31's Thread/Zigbee radio is the standout future card here:** unlike every IP plugin (which talks to a *bridge*), a radio plugin makes projectMM the *hub itself* — direct to the bulbs, no Hue/Trådfri gateway. A separate, board-gated capability, but the same seam philosophy.

## Files

- **Edit:** `src/core/DevicesModule.h` (rewrite — mDNS-listener discovery iterating plugins, drop the sweep + blocking browse, trim `via`), `src/core/DeviceIdentify.h` (shrink to the enum + label), `src/platform/platform.h` + `platform_esp32.cpp` + `platform_desktop.cpp` (the non-blocking mDNS listen seam + the `_wled._tcp` advertise), `src/main.cpp` (pass the numeric version if a plugin needs it), `docs/moonmodules/core/DevicesModule.md` (rewrite the discovery section + the plugin model), `docs/backlog/backlog-core.md` (rewrite the four-mechanism stance), `docs/history/decisions.md` (the lesson).
- **New:** `src/core/DevicePlugin.h` (the seam + the two concrete plugins). A `_wled._tcp` advertise (a few lines in `platform_esp32.cpp`).

## Riskiest parts

1. **The non-blocking mDNS listener** is the real work — `mdns_query_async_*` must be driven correctly (start / poll-timeout-0 / collect / delete / restart) without leaking search handles (the old blocking browse had a handle-lifetime crash the synchronous call avoided; the async path must manage the handle across ticks carefully — own exactly one in-flight query at a time, delete it before starting the next).
2. **mDNS reliability across subnets / AP isolation** — multicast can be filtered by guest-AP isolation; not a regression (mDNS had this before), note it.
3. **Plugin order / ambiguity** — a projectMM device advertises `_http._tcp`; so does a generic web box. The `mm=1` TXT disambiguates (the projectMM plugin requires it; a bare `_http._tcp` hit is generic or skipped). Pin this in a test.
4. **Persistence shape change** — the persisted list drops `via`; the restore path must tolerate an old file that still has it (the keyed reader already ignores extra keys — robust to any input).
5. **Hub plugins (Hue) are deferred but must not be designed out** — the seam's `DiscoveredDevice` + the reserved `command()` should leave room for one hit → many resources + auth; don't bake in flat-device.

## Verification

- **Desktop:** unit tests for each plugin's `fromMdns` (a `_wled._tcp` hit → `Device{type:Wled}`; a `_http._tcp`+`mm=1` hit → projectMM; a bare `_http._tcp` hit → generic/declined; a malformed/empty hit → declined). Pure, host-testable like `DeviceIdentify` was — feed a synthetic `MdnsHost`, assert the classification.
- **Scenario:** mDNS-hit upsert → age-out → list-serialize, with canned `MdnsHost` results fed through the listener seam's desktop stub (a settable result table — the desktop-mock the old backlog item wanted, now an mDNS source).
- **Bench (the real test):** a projectMM device + a WLED device + an ESPHome/HA instance on the LAN → projectMM lists the WLED (and projectMM peers) within a discovery cycle; the projectMM device appears in a real WLED's list AND in a **native WLED app — both iOS and Android** (Android NSD is the stricter `_wled._tcp` consumer); no LED flicker (the listener is a non-blocking poll). Run on the S3 + ESP32-16MB + a WLED unit + the phone apps.
- Full gate set (build all ESP32 variants, ctest, scenarios, check_devices, check_specs, KPI — discovery drops from a blocking sweep + blocking browse to a non-blocking poll, a KPI/tick win to note).

## Decisions — LOCKED

1. **mDNS for discovery; commands split by must-arrive × latency-critical (three transports, not two).** mDNS = discovery (the standard every system announces on). **REST** = must-arrive, latency-tolerant config (set brightness, presets, OTA) — TCP guarantees delivery, ~10–50 ms is fine. **UDP** = latency-critical, lossy-OK (time sync for synchronized effects, live pixels) — ~0.5–1 ms, broadcast to N at once; REST would be 10–100× too slow here. The trap is "all commands = REST": a *config* command is REST, a *sync pulse* is UDP. This is precisely the three jobs WLED conflated on one UDP port; we keep them on the right transport each.
2. **No HTTP scan, ever — and no blocking mDNS browse.** Discovery is a **non-blocking** mDNS listener (async query polled at timeout 0). The slow part was active per-IP probing (HTTP sweep) AND the blocking browse; both go. The HTTP-body classifier (`classifyDevice` et al.) is deleted with the sweep.
3. **Foreign systems are plugins.** A `DevicePlugin` seam (adapter pattern) lets a system hook in as one file (claim a service type, classify an mDNS hit). Two concrete plugins now (projectMM + WLED); ESPHome/Tasmota/Hue later are additive. The seam allows hub-shaped plugins (Hue: bridge → resources + auth), not just flat devices.
4. **Appear in the WLED app ecosystem.** Advertise `_wled._tcp` so the native WLED iOS/Android/Desktop apps (and Home Assistant) list projectMM devices. One advertise; bench-verify Android specifically.

## Out of scope (named, for later)

- **The command half of the plugin seam** — `command()` + per-plugin capability/auth, so projectMM can *control* a discovered foreign device (set WLED brightness via its JSON API, a Hue resource via the bridge's authenticated CLIP API, a Tasmota via `cmnd`). Built when a control consumer exists; the discovery seam is shaped to accept it (incl. hub plugins).
- **Themes / device groups** (sync a command to a group — brightness, palette) — rides REST (must-arrive), built on top of the device list this refactor produces.
- **SuperSync / synchronized clocks** — a UDP lossy stream, a separate feature.
- **Additional discovery plugins** — ESPHome (`_esphome._tcp`), Tasmota, NightDriverStrip, Hue (`_hue._tcp`, hub-shaped) — each a new plugin file against this seam, no core change.
- **Live peer STATE in the list (brightness, on/off, …), not just identity.** Discovery today carries identity only — name, IP, type — and a peer's **name** comes from the mDNS announcement itself (the `_http._tcp` instance name *is* the deviceName; no REST call needed to learn it), so a rename propagates on the next query (within the ~`kQueryEverySec × kPluginCount`-second cycle, live, no UI re-query). But mDNS is discovery-only: it does **not** carry mutable state like brightness or power. To show a peer's *current* brightness/on-off in the device list and have it update live, the device must **poll the peer's REST `/api/state` (or `/json/info` for WLED) periodically** once it has the IP — i.e. discovery (mDNS, gets IP+name) then state (REST, gets brightness etc.), on a slow cadence off the hot path. This is the read counterpart of the command half: the same per-plugin "how do I talk to this system over REST" the control path needs. A future feature; the device list + the plugin seam are the foundation it builds on.
