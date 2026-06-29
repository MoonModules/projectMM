# DevicesModule

A **core**, domain-neutral module that discovers other devices on the LAN, identifies what each is, and presents them as a browsable list. It focuses on *all* devices on the network (including this one, marked as self), not on the host's own state — so its card looks the same on every projectMM instance, ESP32 or PC. Light-domain modules consume the device list; the discovery machinery itself stays domain-neutral.

Submodule of [NetworkModule](NetworkModule.md) — discovery depends on the network being up, the same placement reasoning as [ImprovProvisioningModule](ImprovProvisioningModule.md). Wired by code in `main.cpp` (`networkModule->addChild(devicesModule)`), marked `markWiredByCode()` so persistence preserves it.

## Controls

- `devices` — a [List control](Control.md) whose rows are the discovered devices; each row expands to a detail panel. This module is the list's `ListSource`, walking its own `devices_` array (no copy, no allocation). Read-only from the browser (discovery output flows device → `/api/state` → browser), but **persistable** (see Persistence).

Discovery state ("idle", "N devices", "N devices (cached)") is reported through the standard [MoonModule](MoonModule.md) `setStatus()` channel (rendered generically as the card's status line), not as a separate control. There is no scan button — devices announce themselves; nothing is polled.

## Discovery (mDNS, passive)

Discovery is the standard **mDNS-SD** pattern: each device **announces** a service on the LAN, and this module passively **listens**. No subnet sweep, no per-host probe — a device appears when it announces and ages out when it stops.

- **Announce.** projectMM advertises `_http._tcp` with an `mm=1` TXT marker, and `_wled._tcp` (so it shows up in the native WLED apps + Home Assistant) — see `mdnsInit` in the platform layer.
- **Listen (non-blocking).** Each `loop1s` tick calls `platform::mdnsListenPoll(service, proto, …)` for one service, rotating round-robin through the services the plugins claim. The platform owns a single in-flight async mDNS query and manages its handle entirely within the seam (start → poll with a 0 ms timeout → deliver results → restart), so the call **never blocks the tick** — the hot-path-safe replacement for the former blocking HTTP sweep and blocking mDNS browse.

### Plugins (the interop seam)

Foreign ecosystems hook in as **plugins**, not hardcoded branches — the adapter pattern (cf. `ListSource`, `ModuleFactory`). A [`DevicePlugin`](../../../src/core/DevicePlugin.h) declares the mDNS service it claims and turns a resolved hit (`platform::MdnsHost` — IP, hostname, service, the `mm=1` TXT flag) into a `Device` kind:

| Plugin | Claims | Classifies as |
|---|---|---|
| `MmPlugin` | `_http._tcp` **with** the `mm=1` TXT | projectMM (a bare `_http._tcp` box without the marker is declined) |
| `WledPlugin` | `_wled._tcp` | WLED |

A new system is **one new plugin file** listed in the module — no core edit. Plugins are not all the same shape: a flat-device plugin (WLED, ESPHome) yields one device per hit; a future hub plugin (Hue) would expand one bridge hit into several controllable resources + carry auth — the seam keeps `DiscoveredDevice` plain so that extends without reshaping the flat case. The (reserved) command half (`command()`) translates a generic command into a system's protocol when a control consumer exists; *concrete first, abstract later*.

The plugin classification is pure and host-unit-tested (`unit_DeviceIdentify.cpp` feeds a synthetic `MdnsHost`), with no network.

### Age-out

Each sighting stamps the device's `lastSeenMs`; `ageOut()` runs every tick and drops a non-self device unheard for `kStaleMs` (60 s ≈ a few announce cycles). A **timestamp**, not a counter — "last seen at T" is true regardless of cadence. The self entry never ages out (restamped to "now" every tick it's online). Storage is a fixed `devices_[kMaxDevices]` array — bounded, no heap.

## Transport boundary (discovery vs commands)

Discovery is mDNS (above). It carries only lossy-OK presence — never device-to-device *commands*. Those split by need: must-arrive config (set brightness, presets, OTA) rides **REST** (`/api/control`, TCP-guaranteed); latency-critical lossy-OK traffic (time sync for synchronized effects, live pixels) rides **UDP** (NetworkSend/Receive). The full reasoning is in the design plan; the rule here is simply that this module does *discovery*, and consumers reach a found device over the right transport for the job.

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
