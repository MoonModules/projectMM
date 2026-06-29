# DevicesModule

![DevicesModule controls](../../assets/screenshots/Devices%20module.png)

A **core**, domain-neutral module that discovers other devices on the LAN, identifies what each is, and presents them as a browsable list. It focuses on *all* devices on the network (including this one, marked as self), not on the host's own state — so its card looks the same on every projectMM instance, ESP32 or PC. Light-domain modules consume the device list; the discovery machinery itself stays domain-neutral.

Submodule of [NetworkModule](NetworkModule.md) — discovery depends on the network being up, the same placement reasoning as [ImprovProvisioningModule](ImprovProvisioningModule.md). Wired by code in `main.cpp` (`networkModule->addChild(devicesModule)`), marked `markWiredByCode()` so persistence preserves it.

## Controls

- `devices` — a [List control](Control.md) whose rows are the discovered devices; each row expands to a detail panel. This module is the list's `ListSource`, walking its own `devices_` array (no copy, no allocation). Read-only from the browser (discovery output flows device → `/api/state` → browser), but **persistable** (see Persistence).

Discovery state ("idle", "N devices", "N devices (cached)") is reported through the standard [MoonModule](MoonModule.md) `setStatus()` channel (rendered generically as the card's status line), not as a separate control. There is no scan button — devices announce themselves; nothing is polled.

## Discovery (UDP presence, passive)

Discovery is **passive UDP**: each device **broadcasts** a small presence packet on a well-known port, and this module **listens** (a bound `UdpSocket`, drained non-blocking each tick). No subnet sweep, no per-host probe, **no mDNS query** — a device appears when its broadcast arrives and ages out when it stops.

- **Broadcast.** Every ~10 s (`kBroadcastEverySec`) this module broadcasts a **44-byte WLED-compatible presence packet** (see [`WledPacket`](../../../src/core/WledPacket.h)) to `255.255.255.255:65506`: `token=255, id=1`, our IP, deviceName, board-type byte — plus a projectMM marker stamped into the version field (a region no WLED validator reads). So a peer projectMM device recognises us, **and** a real WLED / WLED app browsing 65506 lists us too. Discovery-only: a WLED that receives it shows us in its instances list, it does **not** sync to it (sync/control is a separate WLED protocol on a port WLED never shares).
- **Listen.** Each `loop1s` tick drains the bound listener with non-blocking `recvFrom` (bounded per tick) and classifies each datagram through the plugins. Never blocks the tick — the hot-path-safe replacement for the former mDNS query, which destabilised our own advertise (a PTR query for a service we also host exhausts the IDF mDNS pool — see the [discovery-transport lesson](../../history/decisions.md)).

**mDNS is advertise-only.** `mdnsInit` still announces `_http._tcp`+`mm=1` and `_wled._tcp`+`mac=` so the **native WLED app + Home Assistant discover us** (they only browse mDNS — UDP can't replace that). But this module never *queries* mDNS; all discovery is UDP.

### Plugins (the interop seam)

Foreign ecosystems hook in as **plugins**, not hardcoded branches — the adapter pattern (cf. `ListSource`, `ModuleFactory`). A [`DevicePlugin`](../../../src/core/DevicePlugin.h) declares the UDP port it listens on (`discoveryPort()`) and turns a received datagram into a `Device` kind (`classifyPacket`):

| Plugin | Claims (on UDP 65506) | Classifies as |
|---|---|---|
| `MmPlugin` | a valid WLED packet **with** the projectMM marker | projectMM |
| `WledPlugin` | a valid WLED packet **without** the marker | WLED |

`MmPlugin` is offered each packet first, so a projectMM peer (which broadcasts a marked, WLED-valid packet) is typed projectMM and not double-claimed as WLED. A new system is **one new plugin file** listed in the module — no core edit. The seam keeps `DiscoveredDevice` plain so a future hub plugin (Hue) extends it without reshaping the flat case; the (reserved) `command()` half translates a generic command into a system's protocol when a control consumer exists. *Concrete first, abstract later.*

The plugin classification is pure and host-unit-tested (`unit_DeviceIdentify.cpp` feeds synthetic packets, incl. short/garbage → declined), with no network. The full pipeline is tested via `injectPacketForTest` (`unit_DevicesModule_discovery.cpp`) — and because `UdpSocket` works on desktop, the discovery path is host-testable with real datagrams, not just stubs.

### Age-out

Each sighting stamps the device's `lastSeenMs`; `ageOut()` runs every tick and drops a non-self device unheard for `kStaleMs` (60 s ≈ a few broadcast intervals). A **timestamp**, not a counter. The self entry never ages out (restamped each tick it's online). Storage is a fixed `devices_[kMaxDevices]` array — bounded, no heap.

## Interop — projectMM shows up in WLED

Because the presence broadcast and the mDNS advertise are WLED-shaped, a projectMM device appears in the WLED ecosystem two independent ways, with no projectMM software on the other side:

**In WLED's own "Sync interfaces" instances list** — a real WLED lists every projectMM board it heard on UDP 65506. (The `undefined` columns are WLED-sync fields projectMM doesn't fill — the presence packet carries identity, not the full WLED sync state; listing is what we're after.)

![projectMM devices in WLED's instances list](../../assets/screenshots/Wled%20discovers%20projectMM.png)

**In the native WLED app** (iOS / Android) — discovered via the mDNS `_wled._tcp` advertise, validated via the `/json/info` shim, with live colour + a working brightness slider over the `/ws` WebSocket. See [HttpServerModule § WLED-compatibility shim](HttpServerModule.md#wled-compatibility-shim) for the wire contract (reverse-engineered from the [WLED-Android](https://github.com/Moustachauve/WLED-Android) client).

![projectMM devices in the native WLED app](../../assets/screenshots/WLED%20Native%20discovers%20projectMM.jpeg)

## Transport boundary (discovery vs commands)

Discovery is UDP presence (above) — lossy-OK, never device-to-device *commands*. Those split by need: must-arrive config (set brightness, presets, OTA) rides **REST** (`/api/control`, TCP-guaranteed); latency-critical lossy-OK traffic (time sync, live pixels) rides its own **UDP** stream (NetworkSend/Receive). This module does *discovery*; consumers reach a found device over the right transport for the job.

## Persistence (instant boot list)

The discovered list survives reboot: the `devices` [List control](Control.md) is persistable, so a change marks the module dirty and FilesystemModule saves the list as a JSON array; on boot the persistence overlay restores it (via `ListSource::restoreList`, which uses the recursive [JsonUtil](Control.md) reader's `forEachListElement`) *before* the first announcement arrives. So the UI shows the **last-known devices immediately** ("N devices (cached)") rather than waiting for the first re-announcement. The self entry is not restored from the cache (its IP can change); `upsertSelf` re-adds it live with the current address. The restore tolerates an old persisted file carrying extra keys (e.g. the former `via`/`speaks`) — the keyed reader ignores them.

## Self

This device always appears in the list (`upsertSelf`, marked `self:true`), so the card shows the whole network including the host. The UI marks the self row distinctly (an accent edge). The self entry is identified by comparing the announcing IP to the local IP.

## Wire shape

The `devices` List serializes (via [Control](Control.md)'s `ControlType::List`) as a `value` array of row summaries — `{"name","ip","type",["self"]}` — with a parallel `detail` array carrying `url` and `ageSec` (seconds since last heard, computed device-side as `now − lastSeenMs`; omitted on the self row, which is always current). A device restored from persistence but **not yet re-heard live** this session carries `cached:true` instead of `ageSec` — the UI shows "last seen: cached" until an announcement re-confirms it, at which point `cached` clears and a real `ageSec` appears. The UI renders `ageSec` as a relative "last seen 2m ago".

## Prior art

- **mDNS-SD / DNS-SD (Bonjour, Avahi)** — the industry-standard service-discovery pattern this module uses: announce a service, browse for it. WLED, ESPHome, Home Assistant, Hue all speak it.
- **WLED** — the `_wled._tcp` service it advertises (and that the native WLED iOS/Android/Desktop apps browse) is the interop target the `WledPlugin` + the `_wled._tcp` advertise serve.
- **MoonLight** uses a UDP presence broadcast for device discovery; DevicesModule takes the "devices find each other" idea but uses the standard mDNS announce/listen instead of a bespoke UDP beacon (mDNS is what the whole ecosystem already speaks).
- The web installer's `docs/install/devices.js` "Your devices" list is the prior art for the device record shape (name / url / type).

## Source

[DevicesModule.h](../../../src/core/DevicesModule.h) · [DevicePlugin.h](../../../src/core/DevicePlugin.h)
