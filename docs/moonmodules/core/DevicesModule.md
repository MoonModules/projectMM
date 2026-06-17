# DevicesModule

A **core**, domain-neutral module that discovers other devices on the LAN, identifies what each is, and presents them as a browsable list. It focuses on *all* devices on the network (including this one, marked as self), not on the host's own state — so its card looks the same on every projectMM instance, ESP32 or PC. Light-domain modules consume the device list; the discovery machinery itself stays domain-neutral.

Submodule of [NetworkModule](NetworkModule.md) — discovery depends on the network being up, the same placement reasoning as [ImprovProvisioningModule](ImprovProvisioningModule.md). Wired by code in `main.cpp` (`networkModule->addChild(devicesModule)`), marked `markWiredByCode()` so persistence preserves it.

## Controls

- `scan` — a **button** (momentary action, [ControlType::Button](Control.md)): pressing it re-runs the subnet sweep now (`onUpdate` → `restartScan`). A button, not a toggle, because it's a one-shot action, not an on/off state.
- `progress` — a progress bar of the sweep position (host 0..254, where 0 is idle/empty and 1..254 track the sweep). Always present (the WebSocket state push patches control *values* but not *structure*, so a hide-while-idle flag would not update live); at rest its value is 0 (empty bar).

Sweep state ("idle", "scanning A.B.C.0/24", "N devices", "no network") is reported through the standard [MoonModule](MoonModule.md) `setStatus()` channel (rendered generically as the card's status line by HttpServerModule), not as a separate control.
- `devices` — a [List control](Control.md) whose rows are the discovered devices; each row expands to a detail panel. This module is the list's `ListSource`, walking its own `devices_` array (no copy, no allocation).

## Discovery

Two strategies run side by side and merge into the *same* `devices_` list:

**mDNS browse** is the push-style strategy and runs **every tick**. `stepMdns()` cycles round-robin through a small set of service types (`_http._tcp`, `_wled._tcp` today) using the non-blocking `platform::mdnsBrowseStart` / `mdnsBrowsePoll` / `mdnsBrowseStop` cycle: start one async PTR query, poll it (a 0 ms async check — never blocks the render loop), merge any resolved hosts via `upsertMdns`, then advance to the next service type. The service type maps to a `DevType` (`_wled._tcp` → WLED; `_http._tcp` → generic, refined later by the HTTP probe). Because it never blocks, it is the only strategy safe to run continuously, so it picks up advertisers (WLED, projectMM, anything advertising `_http._tcp`) as they come and go.

**Subnet sweep** is the fallback for hosts that don't advertise a useful service (a projectMM **desktop** instance, a generic web host). `restartScan()` captures the local IP (from `platform::ethGetIPv4` / `wifiStaGetIPv4`) and walks the local /24 (`subnet.1` .. `subnet.254`), one IP per `loop1s()` tick. The sweep runs **once at boot** (when the network first comes up) and otherwise only on a `scan` button press — there is **no periodic background sweep**. Reason: the probe is a *blocking* `httpGet` running on the render task, so each probe stalls the tick up to the probe timeout (~150 ms); a continuous background sweep would flicker the LEDs. At boot the LEDs aren't yet critical, so the one-shot sweep there is acceptable.

Per host, `probe()` issues `platform::httpGet` (short timeout) and classifies the response:

| Probe | Match | Type |
|-------|-------|------|
| `GET /api/state` | **HTTP 200** and body contains `"modules"` | projectMM |
| `GET /json/info` | **HTTP 200** and body contains `WLED` | WLED |
| (any other live host) | answered (any status), not the above | generic |
| no response on 80 or 8080 | — | not listed |

The status-200 gate matters: a 404/500 error page that happens to contain `"modules"` or `WLED` must not be misread as a real device. A non-200 response still proves the host is *alive*, so it falls through to the generic classification rather than being dropped.

Both ports 80 and 8080 are probed per host: ESP32 devices and WLED serve on 80, a projectMM **desktop** instance serves its API on 8080. A live host on 80 stops there; the 8080 attempt only adds a second timeout on otherwise-empty IPs.

The display name comes from the probe body — projectMM's `deviceName` (`/api/state`), WLED's `name` (`/json/info`) — falling back to the dotted-quad IP. A foreign device's reply is parsed with a local, defensive string scan (`extractStringAfter`), not the project's own [JsonUtil](Control.md) key parser: any input is tolerated (a garbage body yields an empty name), per the robustness contract for network-sourced data.

### Discovery is per-protocol (HTTP is strategy one)

HTTP and mDNS find web-UI / service-advertising devices, but the wider ecosystem this module will talk to is found and addressed over **other** protocols too: Art-Net / sACN nodes and DDP devices (UDP, no web UI), OSC, RTP-MIDI (mDNS `_apple-midi._udp`). Discovery is therefore structured as **probe strategies** that each contribute to the *same* device list. Two bitmasks on every `Device` keep this open without reshaping the record: `speaks` (which protocols a device talks — `ProtoHttp` today; `ProtoArtnet`, `ProtoDdp`, … as strategies are added) so a consumer (Art-Net sync, fleet OTA) knows *how* to reach it, and `via` (which strategies found it — `scan` / `mdns` / `udp`) so the UI shows the discovery source. A device found by both mDNS and the sweep OR-s both bits — it is the same device, surfaced twice. Adding an mDNS service type is one entry in `kMdnsServices` (Home Assistant `_home-assistant._tcp`, ESPHome `_esphome._tcp`, …); adding a non-HTTP strategy is a new probe plus the bits it sets — neither reshapes the record or the wire format.

Each sighting (any strategy) stamps the device's `lastSeenMs`; `ageOut()` runs every tick and drops a non-self device unseen for `kStaleMs` (24 h). A **timestamp**, not a per-sweep miss-counter, because the strategies run on different cadences (a minutes-long HTTP sweep, a seconds-long mDNS lap, a future async UDP beacon) with no shared sweep boundary to count against; "last seen at T" is true regardless of which strategy saw it. The window is a full day on purpose: mDNS re-confirms its devices cheaply every browse lap, but an HTTP-scan-only device (a PC instance, a generic host) has no cheap recurring refresh — the sweep is boot-once + manual, not periodic — so a short timeout would wrongly drop a still-alive device and force a re-scan. A day lets such a device persist on its single sighting while a genuinely-departed device still clears within a day. The self entry never ages out (restamped to "now" every sweep step). Storage is a fixed `devices_[kMaxDevices]` array — bounded, no heap.

## Persistence (instant boot list)

The discovered list survives reboot: the `devices` [List control](Control.md) is persistable, so a completed sweep marks the module dirty and FilesystemModule saves the list as a JSON array; on boot the persistence overlay restores it (via `ListSource::restoreList`, which uses the recursive [JsonUtil](Control.md) reader's `forEachListElement` to walk the saved array) *before* the first sweep runs. So the UI shows the **last-known devices immediately** ("N devices (cached)") rather than waiting the minutes a fresh sweep takes — the real win for slow-to-find devices (a PC instance, a generic host) that aren't mDNS-discoverable. The self entry is not restored from the cache (its IP can change); `upsertSelf` re-adds it live with the current address.

## Self

This device always appears in the list (`upsertSelf`, marked `self:true`), so the card shows the whole network including the host. The UI marks the self row distinctly (an accent edge). The self entry is identified by comparing each probed IP to the local IP.

## Wire shape

The `devices` List serializes (via [Control](Control.md)'s `ControlType::List`) as a `value` array of row summaries — `{"name","ip","type",["self"]}` — with a parallel `detail` array carrying `url`, the `speaks` protocol array, the `via` discovery-source array, and `ageSec` (seconds since last seen, computed device-side as `now − lastSeenMs`; omitted on the self row, which is always current). A device restored from persistence but **not yet re-seen live** this session carries `cached:true` instead of `ageSec` (its `via` is empty and its timestamp is only the boot stamp, so a real "age" would be misleading) — the UI shows "last seen: cached" until a strategy re-confirms it, at which point `cached` clears and a real `ageSec` + `via` appear. The UI renders `ageSec` as a relative "last seen 2m ago". The List is read-only from the browser's side (discovery output flows device → `/api/state` → browser) but **persistable**: the saved array is parsed back on boot by `restoreList` to seed the instant cached list (see Persistence above).

## Prior art

- **MoonLight** uses UDP broadcast for device presence; DevicesModule takes the "devices find each other" idea but uses an IP-scan + REST-identify outer loop so non-broadcasting devices (a PC instance, Home Assistant, WLED) are found too.
- **WLED** discovery / the WLED JSON API (`/json/info`, `brand:WLED`) — the WLED identify probe and name field come from here.
- The web installer's `docs/install/devices.js` "Your devices" list is the prior art for the device record shape (name / url / type).

## Source

[DevicesModule.h](../../../src/core/DevicesModule.h)
