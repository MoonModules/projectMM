# DevicesModule

A **core**, domain-neutral module that discovers other devices on the LAN, identifies what each is, and presents them as a browsable list. It focuses on *all* devices on the network (including this one, marked as self), not on the host's own state — so its card looks the same on every projectMM instance, ESP32 or PC. Light-domain modules consume the device list; the discovery machinery itself stays domain-neutral.

Submodule of [NetworkModule](NetworkModule.md) — discovery depends on the network being up, the same placement reasoning as [ImprovProvisioningModule](ImprovProvisioningModule.md). Wired by code in `main.cpp` (`networkModule->addChild(devicesModule)`), marked `markWiredByCode()` so persistence preserves it.

## Controls

- `scan` — a **button** (momentary action, [ControlType::Button](Control.md)): pressing it re-runs the subnet sweep now (`onUpdate` → `restartScan`). A button, not a toggle, because it's a one-shot action, not an on/off state.
- `progress` — a progress bar of the sweep position (host 1..254). Shown only while a sweep runs; hidden at rest.

Sweep state ("idle", "scanning A.B.C.0/24", "N devices", "no network") is reported through the standard [MoonModule](MoonModule.md) `setStatus()` channel (rendered generically as the card's status line by HttpServerModule), not as a separate control.
- `devices` — a [List control](Control.md) whose rows are the discovered devices; each row expands to a detail panel. This module is the list's `ListSource`, walking its own `devices_` array (no copy, no allocation).

## Discovery

A subnet sweep is the outer loop: `restartScan()` captures the local IP (from `platform::ethGetIPv4` / `wifiStaGetIPv4`) and walks the local /24 (`subnet.1` .. `subnet.254`), one IP per `loop1s()` tick. The sweep runs **once at boot** (when the network first comes up) and otherwise only on a `scan` button press — there is **no periodic background scan**. Reason: the probe is a *blocking* `httpGet` running on the render task, so each probe stalls the tick up to the probe timeout (~150 ms); a continuous background sweep would flicker the LEDs. At boot the LEDs aren't yet critical, so the one-shot sweep there is acceptable. Moving the probe to its own task (the enabler for safe periodic scanning + a UDP presence beacon) is in the backlog. mDNS browse is not used either (the desktop instance does not advertise mDNS, and not every device advertises a useful service); the IP sweep finds anything with an HTTP API regardless.

Per host, `probe()` issues `platform::httpGet` (short timeout) and classifies the response:

| Probe | Match | Type |
|-------|-------|------|
| `GET /api/state` | body contains `"modules"` | projectMM |
| `GET /json/info` | body contains `WLED` | WLED |
| (any other live host) | answered, not the above | generic |
| no response on 80 or 8080 | — | not listed |

Both ports 80 and 8080 are probed per host: ESP32 devices and WLED serve on 80, a projectMM **desktop** instance serves its API on 8080. A live host on 80 stops there; the 8080 attempt only adds a second timeout on otherwise-empty IPs.

The display name comes from the probe body — projectMM's `deviceName` (`/api/state`), WLED's `name` (`/json/info`) — falling back to the dotted-quad IP. A foreign device's reply is parsed with a local, defensive string scan (`extractStringAfter`), not the project's own [JsonUtil](Control.md) key parser: any input is tolerated (a garbage body yields an empty name), per the robustness contract for network-sourced data.

### Discovery is per-protocol (HTTP is strategy one)

HTTP discovery finds web-UI devices, but the wider ecosystem this module will talk to is found and addressed over **other** protocols: Art-Net / sACN nodes and DDP devices (UDP, no web UI), Home Assistant (mDNS `_home-assistant._tcp`), RTP-MIDI (mDNS `_apple-midi._udp`), OSC, ESPHome / Shelly / Tasmota (mDNS service types). Discovery is therefore structured as **probe strategies** that each contribute to the *same* device list, and every `Device` carries a `speaks` protocol bitmask (`ProtoHttp` today; `ProtoArtnet`, `ProtoDdp`, … as strategies are added) so a consumer (Art-Net sync, fleet OTA) knows *how* to talk to a device. v1 ships the HTTP strategy only; mDNS browse and a UDP beacon / ArtPoll listener are the planned next strategies (mDNS browse is the biggest reach-extender — HA, WLED, ESPHome, RTP-MIDI at once). Adding a strategy is a new probe plus the bits it sets, not a reshape of the record or wire format.

After each full sweep, `ageOut()` drops devices not seen for `kMaxMissed` consecutive sweeps (a device that left the network); the self entry never ages out. Storage is a fixed `devices_[kMaxDevices]` array — bounded, no heap.

## Persistence (instant boot list)

The discovered list survives reboot: the `devices` [List control](Control.md) is persistable, so a completed sweep marks the module dirty and FilesystemModule saves the list as a JSON array; on boot the persistence overlay restores it (via `ListSource::restoreList`, which uses the recursive [JsonUtil](Control.md) reader's `forEachListElement` to walk the saved array) *before* the first sweep runs. So the UI shows the **last-known devices immediately** ("N devices (cached)") rather than waiting the minutes a fresh sweep takes — the real win for slow-to-find devices (a PC instance, a generic host) that aren't mDNS-discoverable. The self entry is not restored from the cache (its IP can change); `upsertSelf` re-adds it live with the current address.

## Self

This device always appears in the list (`upsertSelf`, marked `self:true`), so the card shows the whole network including the host. The UI marks the self row distinctly (an accent edge). The self entry is identified by comparing each probed IP to the local IP.

## Wire shape

The `devices` List serializes (via [Control](Control.md)'s `ControlType::List`) as a `value` array of row summaries — `{"name","ip","type",["self"]}` — with a parallel `detail` array carrying `url` as well. The List is read-only and non-persistable: discovery output flows one direction (device → `/api/state` → browser), never parsed back, which matches the project's flat (non-recursive) JSON reader.

## Prior art

- **MoonLight** uses UDP broadcast for device presence; DevicesModule takes the "devices find each other" idea but uses an IP-scan + REST-identify outer loop so non-broadcasting devices (a PC instance, Home Assistant, WLED) are found too.
- **WLED** discovery / the WLED JSON API (`/json/info`, `brand:WLED`) — the WLED identify probe and name field come from here.
- The web installer's `docs/install/devices.js` "Your devices" list is the prior art for the device record shape (name / url / type).

## Source

[DevicesModule.h](../../../src/core/DevicesModule.h)
