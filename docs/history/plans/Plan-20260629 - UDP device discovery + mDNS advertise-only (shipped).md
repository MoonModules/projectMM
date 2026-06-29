# Plan — UDP device discovery (projectMM + WLED), mDNS becomes advertise-only

> Builds on the shipped WLED-app interop (mDNS `_wled._tcp` advertise + `/json/info` + WS state). This plan changes how a projectMM device **discovers** other devices on the LAN — moving from mDNS *query* to passive **UDP listen**, per the bench evidence gathered 2026-06-29.

## Why (the evidence, already measured)

- **mDNS discovery is query-driven and the query destabilises our own advertise.** A blocking PTR query for a service we also host exhausts the IDF mDNS pool and makes our `_http`/`_wled` advertisement vanish from peers. Bench-confirmed: with querying disabled, every board became reliably discoverable.
- **A passive mDNS browser can't replace the query** — no device on the LAN re-announces unsolicited (75 s capture: zero announcements), so there's nothing to passively hear over mDNS.
- **But UDP broadcast discovery IS passive and reliable.** projectMM controls its own beacon (both ends ours). WLED broadcasts a 44-byte status packet on **UDP 65506** every ~30 s by default (`token==255, id==1`) — measured live on two reference WLEDs. So *both* ecosystems can be discovered by **listening to UDP broadcasts**, with no querying.
- **mDNS advertise stays REQUIRED** — the WLED native app discovers *us* only via mDNS `_wled._tcp` (confirmed in its source, `DeviceDiscovery.kt`). UDP can't replace that direction. So mDNS shrinks to **advertise-only**: we announce so foreign apps find us; we never query.

Net effect: discovery becomes pure passive UDP receive, the self-query-disturbs-advertise bug is **structurally impossible**, and mDNS does only the one thing it's needed for (making us visible to WLED apps / Home Assistant).

## Sanity check against the docs

- **Common patterns first / Industry standards, our own code:** UDP broadcast presence is the textbook LAN-discovery-without-infrastructure pattern; WLED's 65506 packet is a documented wire format we *observe and re-implement fresh* (not copy — per [[no-wled-mm-derivation]] and the MoonLight `ModuleDevices.h` reference we read for the byte layout). mDNS-for-advertise is the standard service-announce.
- **Default to subtraction / Complexity lives in core:** removes the mDNS-query path from DevicesModule + the platform; the UDP receive primitive is a small core seam each plugin leans on.
- **Robust to any input:** a malformed/short datagram is dropped, never crashes (the robustness contract); the plugin classify stays defensive.
- **Hot path discipline:** the UDP listen is a non-blocking `recvFrom` drained on `loop1s`/`loop20ms`, off the render path — same shape as the current mDNS poll.

## The transport split (the end state)

| Plugin | Discovers peers via | Makes us discoverable via |
|---|---|---|
| **MmPlugin** (projectMM↔projectMM) | **UDP broadcast** — our own presence packet on a chosen port, `255.255.255.255` | the same UDP broadcast |
| **WledPlugin** (WLED / HA / WLED app) | **UDP 65506 listen** — WLED's 44-byte beacon (`token==255,id==1`, byte 38 board type) | **mDNS `_wled._tcp` + `/json/info` shim** (the WLED app only does mDNS — unchanged, already shipped) |

Both discovery paths are passive UDP receive. **No plugin queries mDNS.** mDNS is advertise-only (`mdnsInit` keeps announcing `_http._tcp`+`mm=1` and `_wled._tcp`+`mac=`; the `mdnsListenPoll` query path and its DevicesModule caller are removed).

## Design

### 1. Reshape the `DevicePlugin` seam from mDNS-shaped to transport-agnostic

Today the seam is `service()`/`proto()`/`classify(MdnsHost&)`. Replace with a UDP-discovery shape:

```
class DevicePlugin {
  virtual const char* label() const = 0;
  // The UDP port this plugin listens on for presence packets (0 = none).
  virtual uint16_t discoveryPort() const = 0;
  // Classify a received datagram from `srcIp`. Returns true + fills `out` (type, name)
  // when this plugin owns the packet; false to decline. Defensive: a short/garbage
  // datagram on the claimed port → decline, never crash.
  virtual bool classifyPacket(const uint8_t* data, size_t len, const uint8_t srcIp[4],
                              DiscoveredDevice& out) const = 0;
  // (reserved) command(...) — unchanged, still future.
};
```

- **MmPlugin:** `discoveryPort()` = the projectMM presence port (a fixed port we pick — e.g. reuse/define one distinct from 65506 so we don't collide with WLED; the MoonLight precedent uses 65506 for WLED-compatible + 65507 for its own — we can broadcast a projectMM packet on our own port). `classifyPacket` recognises our own presence packet (a small fixed header with a magic + the deviceName + IP) → `DevType::ProjectMM`.
- **WledPlugin:** `discoveryPort()` = 65506. `classifyPacket` validates `len>=44 && data[0]==255 && data[1]==1` → `DevType::Wled`, extracting the name (WLED's packet carries the hostname at bytes 6–37) and the source IP.

### 2. projectMM presence broadcast (the MmPlugin's "make us discoverable")

A small periodic broadcast (every ~10–30 s, slow like WLED's) of a projectMM presence packet: magic bytes + protocol version + our IP + deviceName. Sent from the platform (it owns the socket + the broadcast address), driven on a slow `loop1s` cadence from DevicesModule (or a dedicated slow timer). Fixed-size, no allocation.

### 3. Platform UDP-discovery seam

Two small additions to `platform.h` (desktop stubs as usual):
- `udpDiscoveryListen(port)` / a shared receive that DevicesModule drains — OR reuse the existing `UdpSocket` (`bind` + non-blocking `recvFrom(srcIp)`) directly. **Prefer reusing `UdpSocket`** (already in `platform.h`, used by ArtNet) — DevicesModule owns one bound `UdpSocket` per distinct discovery port, drains each on its tick, and feeds datagrams to the plugins. No new platform primitive if `UdpSocket` covers it (it does: `bind`, non-blocking `recvFrom` with `srcIp`, `sendToAddr`/broadcast for our own beacon).
- Broadcast send for our presence packet: `UdpSocket::sendToAddr({255,255,255,255}, port, …)` — confirm the socket has `SO_BROADCAST` (add if missing; ArtNet may already broadcast).

### 4. DevicesModule rewire

- Drop the mDNS-query loop (`mdnsListenPoll` calls, the `queryTick_`/`serviceCursor_` rotation).
- Own a bound `UdpSocket` per distinct `discoveryPort()` across plugins (dedupe — projectMM + WLED are different ports). On `loop1s` (or `loop20ms` for snappier discovery), `recvFrom` each socket in a non-blocking loop, hand each datagram to the plugins (`classifyPacket`), upsert the recognised device. Same `Device` struct, ListSource, persistence, age-out, self-row.
- Broadcast our own presence packet on the slow cadence.
- `kStaleMs` sized to a few presence intervals (a device re-announces every ~10–30 s, so ~3× that).

### 5. mDNS: advertise-only

- `mdnsInit` unchanged (keep the `_http`+`mm=1`, `_wled`+`mac=`, re-advertise, symmetric stop — all shipped).
- **Remove** `mdnsListenPoll` (platform) + its decl + the desktop stub + the DevicesModule caller. mDNS no longer queries anything.
- `MdnsHost` struct + `mdnsBrowse` may stay if still used elsewhere; if not, remove (subtraction).

## Files

- **Edit:** `src/core/DevicePlugin.h` (seam reshape + Mm/Wled `classifyPacket`), `src/core/DevicesModule.h` (UDP listen/drain/broadcast, drop mDNS query), `src/platform/platform.h` (+ desktop/esp32 if `UdpSocket` needs a broadcast flag or a presence-send helper), `src/platform/esp32/platform_esp32.cpp` (remove `mdnsListenPoll`; presence broadcast if platform-side), `src/platform/desktop/platform_desktop.cpp` (stub adjustments).
- **Tests:** `test/unit/core/unit_DevicesModule_discovery.cpp` — drive `classifyPacket` with synthetic datagrams (a 44-byte WLED packet `token=255,id=1`; a projectMM presence packet; a short/garbage packet → declined). The `injectMdnsHitForTest` seam becomes `injectPacketForTest`. Plus the existing age-out / no-contamination cases adapted.
- **Docs:** `docs/moonmodules/core/DevicesModule.md` (UDP discovery + the transport-per-plugin table), `docs/history/decisions.md` (the transport-split lesson).

## Decisions locked (product owner)

- **Port: 65506, WLED-compatible 44-byte format.** projectMM broadcasts a valid `UDPWLEDHeader` (`token=255, id=1`, real IP octets, deviceName at bytes 6–37, board-type byte at 38). **Verified safe** against the product owner's concern "don't send WLED wrong info it gets confused by": per MoonLight's working code, a WLED receiving a 65506 packet uses it for **discovery only — it shows the sender in its device list and does NOT sync to it or change state**. The packet carries no command. WLED's validation is `token==255 && id==1 && ip0==localIP[0]` (a subnet check), so we set `ip0` to our real first octet. Sync/control is a *separate* concern on port 65507 which WLED never listens on — so there is no path for our presence packet to command a WLED. Bonus: real WLEDs + WLED apps that browse 65506 may see us via UDP too.
- **Cadence: ~10 s broadcast, drain on `loop1s`.** A new device appears within ~10 s; light traffic. `kStaleMs` ~60 s (≈ 6 intervals).

## Riskiest parts

1. **Packet contents must be a *valid* WLED header so WLED reads us but never mis-syncs** — resolved above (discovery-only by WLED's design; we fill token/id/ip0/name/type correctly). The one must-get-right: `ip0 == our real first IP octet` or WLED's subnet check rejects us.
2. **`UdpSocket` broadcast** — confirm `SO_BROADCAST` is set for the send side; receiving broadcasts needs `bind(port)` on `0.0.0.0` (already how ArtNet-in binds). On ESP32, a bound listen socket must survive netif up/down (re-bind on reconnect).
3. **Multiple bound sockets** — DevicesModule binds one per discovery port (projectMM + 65506). Bounded (≤ plugin count). Non-blocking drains, no starvation.
4. **Desktop** — `UdpSocket` works on desktop (ArtNet uses it), so discovery can actually be unit-tested with real loopback datagrams, not just stubs — a nice testability gain over the mDNS-stubbed path.

## Verification

- Desktop build green; `ctest` + scenarios green; ESP32 all variants green.
- Unit: `classifyPacket` accepts a real 44-byte WLED packet + a projectMM presence packet, declines garbage; no name/IP cross-contamination; age-out at the new window.
- **Bench (the real test):** all 4 boards discover each other (projectMM presence) + both reference WLEDs (65506) and hold steady — *without* the advertise instability (the mDNS advertise stays rock-solid because nothing queries it). Cross-check: the WLED native app still lists all 4 boards (mDNS advertise + `/json/info` unchanged).
- Save this plan (done); mark `… (shipped).md` when it lands.

## Out of scope

- **The control/command half** (`DevicePlugin::command`) — still reserved; we discover + classify, not yet command foreign devices.
- **WLED UDP sync/realtime** (ports 21324/11988) — a separate feature (driving WLED pixels), not discovery.
- **Live peer state** (a peer's brightness in our list) — still the REST-after-discovery follow-up.
