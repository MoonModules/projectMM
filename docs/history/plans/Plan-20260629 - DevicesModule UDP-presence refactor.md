# Plan — DevicesModule refactor: fast + reliable intra-device communication

## Context

The current `DevicesModule` (627 lines) discovers LAN devices with **two strategies merged into one list**: an mDNS browse every tick and a one-shot HTTP subnet sweep (plus a manual "scan" button). The product owner wants a complete refactor:

- The **HTTP subnet sweep is too slow and offers little** — a *blocking* `httpGet` on the render task, 1 IP/tick, a full /24 takes ~4 minutes; deliberately not periodic because it would flicker LEDs.
- **mDNS adds little value** for projectMM↔projectMM — its main draw (Home Assistant / ESPHome service discovery) can be added later if needed; it is not worth its weight as the discovery base.
- The base for intra-device communication should be **fast and reliable** — not a single transport dogmatically, but the *right* transport per job.

### Framing corrections (product owner, supersedes the old four-mechanism wording)

1. **The thesis is "fast + reliable intra-device communication," not "UDP everywhere."** UDP is one transport. Use it where lossy-is-acceptable (presence/discovery: a missed beacon is caught next cycle). Use **REST for commands** that must arrive (UDP gives no end-to-end delivery guarantee; REST/TCP does). This *keeps* the messaging-vs-discovery axis the old [backlog four-mechanism split](../../backlog/backlog-core.md) got right — it only swaps the *discovery* mechanism (UDP presence in, slow HTTP scan + low-value mDNS out).
2. **WLED is a patch-in to support, not the basis.** WLED's wire format is "a hack in a lot of places" — do **not** design around it as the standard. Design a clean, general intra-device presence protocol first (study how systems intra-communicate broadly), then add a **WLED-compatibility shim** so projectMM can also hear / be heard by WLED. This is *Industry standards, our own code*: the public WLED broadcast format is one interop target, credited and supported, not the architecture.
3. **MoonLight's `ModuleDevices.h` is inspiration, credited, not traced.** It is read for the *idea* (a UDP presence beacon whose first bytes are WLED-compatible, a separate control port). Its code is GPL-v3/commercial-licensed and is **not** copied; projectMM's protocol is written fresh against our `UdpSocket` seam and `Device` model. Prior art credited in the module's "Prior art" section.

This refactor is mostly **subtraction**: two discovery strategies + the `via` bitmask + the blocking sweep collapse into one passive UDP presence listener. Net lines should *drop*.

## Sanity check against the docs

- **README / CLAUDE.md / architecture.md:** the refactor honours *Default to subtraction* (removes the slow sweep), *Hot path discipline* (UDP receive is a non-blocking poll off the render task — the sweep's blocking `httpGet` was the hot-path smell), *Common patterns first* (a UDP presence/heartbeat beacon is the textbook LAN-discovery pattern — WLED, mDNS-SD announcements, SSDP all broadcast presence), and *Robust to any input* (a malformed/foreign datagram is ignored, never crashes).
- **The one stance it supersedes:** backlog-core's "mDNS preferred for discovery." We replace the *rationale* (recognizable standard) with "UDP presence is faster, simpler, and WLED-interop comes free." Recorded in `decisions.md` with the why, and the backlog stance rewritten in the same change.

## Design

### Transport split (the "fast + reliable" system)

| Job | Transport | Why |
|---|---|---|
| **Discovery / presence** | UDP broadcast beacon | Lossy is fine — a dropped beacon is re-heard next interval. Fast, no scan, no per-host probe. |
| **Commands (must-arrive)** | REST `/api/control` (already built) | TCP guarantees delivery — the MoonLight "messages didn't arrive" pain was UDP-for-must-arrive; we don't repeat it. Future group-sync (brightness to a group) rides this. |
| **Lossy real-time streams** | UDP (already: NetworkSend/Receive) | SuperSync clock / live timing — drop-and-continue, low latency. Out of scope here; named so the axis is complete. |

### The presence beacon (general protocol, WLED shim layered on)

- **Our beacon** is a small fixed-size struct broadcast every N seconds (e.g. 5 s) on a projectMM port: `{ proto-magic, version, ip[4], name, type, capabilities }`. Designed clean — no WLED constraints. A receiver upserts the sender into its `Device` list; a device unseen for K intervals ages out.
- **WLED interop is a shim, not the core.** WLED broadcasts a public 44-byte presence header on port 21324. Two thin additions, isolated from our protocol:
  - **Hear WLED:** also bind/listen the WLED notifier port; parse the 44-byte header (token==255, id==1, name, type) into a `Device` with `type = Wled`. This is *parsing a foreign format*, kept in one `wled_compat` helper.
  - **Be heard by WLED *devices* (UDP):** prefix our beacon's first 44 bytes with the WLED-compatible header so WLED instances list us. (Locked decision #1: both directions.)
  - **Be heard by the WLED *apps* (mDNS) — appear in the native WLED clients:** the Moustachauve WLED app family — [iOS](https://github.com/Moustachauve/WLED-iOS), [Android](https://github.com/Moustachauve/WLED-Android), [Desktop](https://github.com/Moustachauve/WLED-Desktop) — all discover via **mDNS `_wled._tcp`** (same Flutter codebase; only the OS wrapper differs — Apple NWBrowser / Android NSD / Flutter `multicast_dns` — same multicast-mDNS wire protocol), NOT the UDP notifier (that port is device↔device sync; the apps don't browse it). So one **`_wled._tcp` advertise serves all three clients** — there is no per-platform work. This is mDNS *advertise* (publishing a service), a different capability from the mDNS *browse* this refactor drops, so it does **not** reopen the discovery decision. Low-cost: the platform already advertises `_http._tcp` + a `mm=1` TXT (`platform_esp32.cpp` ~line 1025, via `mdns_service_add`); appearing in the WLED apps is **one more `mdns_service_add("_wled","_tcp",80,…)`** in the same place, on the already-running mDNS stack. Bonus: the same advertise makes us natively discoverable by Home Assistant / any Bonjour client later, for free. *Caveat:* Android's NSD is stricter than Apple Bonjour about a well-formed instance/SRV/TXT record, so "works in iOS" does not fully guarantee Android — the advertise must be clean `_wled._tcp`; verified on the bench (below), not assumed.
  - The compat code is a **patch-in**: a separate small translation unit / helper, so the core presence protocol stays clean and WLED quirks don't leak into it. The `_wled._tcp` advertise lives in the platform's existing mDNS-advertise block, not the core protocol.

### What collapses (the subtraction)

- **Delete** the HTTP subnet sweep (`restartScan`, the per-IP `httpGet` probe, the sweep-progress control, the "scan" button) — UDP presence replaces it. The HTTP-body `classifyDevice` in `DeviceIdentify.h` goes too (locked decision #2): the type comes from the beacon's type byte; the WLED 44-byte parse moves to `wled_compat`.
- **Delete** the mDNS browse strategy (`stepMdns`, `kMdnsServices`, the `mdnsBrowse*` platform calls from this module — the seam stays for any future use).
- **Collapse** the `via` bitmask (mdns/scan/udp) — one source now, so "how found" is just "heard a beacon."
- **Keep** the `Device` struct (trim `via`), the `ListSource` rendering, persistence (last-known list on boot), age-out, self-row — these are the parts that *work* and that consumers (UI, main.cpp) depend on.

### Platform

- **No new seam needed.** The existing `UdpSocket` (`platform.h`) already has `bind(port)`, non-blocking `recvFrom(buf, len, srcIp)`, and broadcast `sendToAddr` — everything a presence beacon requires. The refactor is core-only; the platform layer is untouched (a strongpoint: it proves the seam was the right shape).

## Files

- **Edit:** `src/core/DevicesModule.h` (the refactor — remove sweep + mDNS, add UDP presence listen/broadcast, trim `via`), `docs/moonmodules/core/DevicesModule.md` (rewrite the discovery section), `docs/backlog/backlog-core.md` (rewrite the four-mechanism stance + delete the now-shipped HTTP-scan-mock scenario item), `docs/history/decisions.md` (the lesson: why UDP presence over mDNS+scan).
- **New (small):** a `wled_compat` helper (the 44-byte WLED header parse/build) — its own file so the WLED hack stays out of the clean protocol. `DeviceIdentify.h` may shrink or fold in.
- **Maybe delete:** the HTTP-classify path in `DeviceIdentify.h` if UDP-type-byte fully replaces it.

## Riskiest parts

1. **Broadcast reliability across subnets / AP isolation** — UDP broadcast can be filtered by some routers / guest-AP isolation. mDNS had the same issue; not a regression, but note it (a unicast-to-known-IPs fallback is a later option).
2. **WLED port coexistence** — binding the WLED notifier port (21324) on a device that *is* also doing ArtNet must not clash; check the `SO_REUSEADDR` story in `UdpSocket::bind`.
3. **Self-beacon loopback** — a device must ignore its own broadcast (match on self IP), as MoonLight does.
4. **Persistence shape change** — the persisted device list drops `via`; the restore path must tolerate an old file with the field (robust-to-any-input).

## Verification

- **Desktop:** a unit test for the beacon parse/build (round-trip our struct; parse a golden WLED 44-byte header → `Device{type:Wled}`; reject a too-short/foreign datagram). The pure parse/build is host-testable like `DeviceIdentify` is today.
- **Scenario:** presence upsert → age-out → list-serialize, with canned datagrams fed through a settable `UdpSocket` test source (the desktop-mock seam the old backlog item wanted — now it's a UDP source, not an httpGet table).
- **Bench (the real test):** two projectMM devices + one WLED device on the LAN → each projectMM lists the other two within one beacon interval; the projectMM devices appear in a real WLED device's list (UDP interop); and projectMM devices appear in a **native WLED app** — test **both WLED-iOS and WLED-Android** (Android's NSD is the stricter `_wled._tcp` consumer, so iOS passing doesn't guarantee it); no LED flicker (no blocking probe on the render task). Run on the S3 + ESP32-16MB + a WLED unit + the phone apps.
- Full gate set (build all ESP32 variants, ctest, scenarios, check_devices, check_specs, KPI — discovery now adds ~zero tick cost, a KPI win to note).

## Decisions — LOCKED

1. **WLED interop: both — hear AND be heard, by WLED devices *and* apps.** (a) *Devices:* our beacon's first 44 bytes are the WLED-compatible UDP header (WLED instances list projectMM), and we parse WLED's notifier (projectMM lists WLED). (b) *Apps:* advertise `_wled._tcp` over mDNS so the native WLED clients (iOS/Android/Desktop — one `_wled._tcp` advertise covers all three) list projectMM devices. The UDP compat is isolated in `wled_compat`; the `_wled._tcp` advertise sits in the platform's existing mDNS-advertise block — neither leaks WLED quirks into the clean presence protocol.
2. **Drop the HTTP classifier entirely.** No periodic sweep, no on-demand HTTP probe — the blocking `httpGet` on the render task is exactly the hot-path violation that made the sweep slow + flicker-prone (~150 ms tick stall per probe). UDP `recvFrom` is non-blocking, so discovery adds ~zero tick cost. A non-beaconing plain web host simply isn't listed — acceptable per "http scan offers little." `DeviceIdentify.h`'s HTTP-classify path is deleted (the type now comes from the beacon's type byte); the WLED 44-byte parse moves to `wled_compat`.
3. **Beacon interval 5 s, age-out 15 s (3 missed).** Traffic is negligible and was *not* MoonLight's problem (that was UDP-for-must-arrive commands, which we route over REST): one beacon is ~100 bytes, so even **100 devices = ~2 KB/s broadcast (~6% of a single ArtNet stream)**; 10 devices = 200 B/s. The old 30 s was conservative for no benefit. 5/15 gives <5 s discovery and ~15 s departure detection at trivial cost.
4. **Port numbers (settle at implementation):** pick a projectMM presence port that avoids the in-use ArtNet (6454) / E1.31 (5568) / DDP (4048) ports, and listen on the WLED notifier port (21324) for inbound WLED. Confirm `UdpSocket::bind`'s `SO_REUSEADDR` lets the WLED-port listen coexist with any ArtNet receive. (A mechanical choice, not a design fork — left to the implementing commit.)

## Out of scope (named, for later)

- **Themes / device groups** (sync commands to a group — brightness, palette) — rides REST (must-arrive), built on top of the device list this refactor produces. The group *membership* could be announced in the beacon; the *commands* go over REST.
- **SuperSync / synchronized clocks** — UDP lossy stream, a separate feature.
- **Home Assistant / ESPHome discovery** — if wanted later, mDNS browse can be re-added as a *targeted* feature (the seam still exists), not the discovery base.
