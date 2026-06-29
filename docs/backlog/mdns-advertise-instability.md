# Investigation — mDNS advertise instability (projectMM peers intermittently undiscoverable)

> Forward-looking investigation note (exception to the present-tense rule). The DevicesModule mDNS+plugin refactor works for the common case — WLED discovery is solid, the classic ESP32 now discovers, no crashes — but **a projectMM device's own mDNS service advertisement does not propagate reliably to its peers**, so boards intermittently fail to discover each other. This note records the symptom, what's ruled out, the leading hypotheses, and the next concrete steps, so the fix can be done with a clear head rather than guess-and-flash.

## Symptom

On a 4-board bench (ESP32 classic, S3, P4, S31) + 2 real WLED devices, all on one /24:

| Discoverer | Sees projectMM peers | Sees WLED |
|---|---|---|
| MM-P4 (.133) | MM-s31 only | both (.120, .186) |
| MM-S3 (.157) | MM-s31 only | none this cycle |
| MM-Classic (.208) | MM-s31 only | both |
| MM-s31 (.213) | **none** | both |

**The asymmetry is the signal:** **MM-s31's advertisement is solid — all three other boards find it. The other three boards' advertisements do NOT reach their peers — nobody discovers MM-P4 / MM-S3 / MM-Classic.** It is not random per-query flakiness; it is per-*device*: one board advertises reliably, three don't. The real WLED devices advertise rock-solid (always found), proving the LAN + the querier side are fine.

A Mac `dns-sd -B _http._tcp local` browse shows the same: it intermittently sees one or two of our boards, while the WLED is always present.

## Ruled out (with evidence)

- **Not the network / multicast filtering.** The Mac and the boards are same-/24, same-L2 (ARP resolves all). The WLED devices — which advertise the same `_http._tcp` / `_wled._tcp` — are always discovered. Earlier "the router filters multicast" conclusions were wrong; they were the rate-limit bug (below) destabilising boards.
- **Not an ESP32↔ESP32 mDNS limitation.** WLED runs on ESP32s and is discovered fine, and *some* projectMM↔projectMM discovery works (everyone finds MM-s31). The chip + stack are capable; this is our setup.
- **Not the query side.** The querier resolves WLED + MM-s31 reliably with the (now slow-cadence) blocking `mdns_query_ptr`. The classify→upsert path is unit-tested correct (`unit_DevicesModule_discovery.cpp`, incl. rename + no-contamination).
- **Not the UI / WebSocket.** The 1 Hz WS state push carries the full device list and the UI rebuilds it live; stale UI is stale *data*, not a delivery bug.

## Fixed along the way (real bugs the bench surfaced)

These are committed in the refactor; they are *not* the open issue but explain earlier confusion:
- **mDNS query-rate exhaustion** — firing a query every render-loop tick made the IDF mDNS task log `Cannot allocate memory` (its fixed pool, 33 MB heap free) and **destabilise the device's networking**, so peers appeared to drop off the LAN. Fixed by throttling to one query per `kQueryEverySec` ticks, round-robined. *This was the bug masquerading as "the network" and "a chip limitation."*
- **Query rotation** — the listener rotated services every tick, killing each query before it resolved. Fixed: drive one service to completion per cadence.
- **Classification priority / name authority** — a projectMM device (advertises both `_http`+mm=1 and `_wled`) was relabelled WLED, and renames didn't propagate. Fixed + tested.

## Leading hypotheses for the advertise instability (the open issue)

The per-device asymmetry (one board solid, three not) is the thing to explain.

1. **Our own queries withdraw / conflict with our own advertised services.** DevicesModule queries `_http._tcp` and `_wled._tcp` — the *same* services this device *advertises*. On the shared IDF mDNS stack, a PTR query for a service you also host may interact badly (the responder de-prioritising or briefly withdrawing its own record during a query it issued). MM-s31 may differ because of timing/load (it's the fastest board, ~1200 FPS — its query cadence lands differently). **Test:** stop the device from *querying* the services it *advertises* (query only foreign services, or skip self-hosted service types) and see if its advertisement stabilises. Or: advertise on one stack instance and query on another, if IDF allows.
2. **mDNS re-announce / TTL not refreshed.** WLED periodically re-announces its services (mDNS records have a TTL; a good responder re-sends before expiry). If our `mdnsInit` registers the service once and never re-announces, the record expires from peers' caches after the TTL and isn't refreshed — so a peer sees us once, then we age out of *its* mDNS cache. The S31 might re-announce as a side effect of something the others don't. **Test:** check whether IDF auto-re-announces `mdns_service_add` records or needs a periodic `mdns_service_*` refresh; compare the TTL we advertise vs WLED's; watch a peer's cache over minutes.
3. **netif / interface binding at advertise time.** mDNS multicast must egress the right netif. If `mdnsInit` runs before the STA netif is fully multicast-joined (IGMP) on some boards, the service is registered but not announced on-wire. The S31 vs the others differ in bring-up timing (different chips/SDKs, the P4/Classic do Ethernet). **Test:** delay/re-run the service advertise after `IP_EVENT_*_GOT_IP` is fully settled; verify the IGMP join for 224.0.0.251 on each board; compare the mDNS-start log timing across boards.
4. **Hostname/instance collision or churn.** All four use distinct names now (MM-P4/S3/Classic/s31), so a flat collision is out — but a *rename* re-registers the service (`mdnsInit` idempotent path in `NetworkModule::syncMdns`), and if that re-register transiently withdraws the record without cleanly re-adding, repeated renames could leave it withdrawn. **Test:** a board that never renames vs one that does; does disabling the live-rename re-register path stabilise advertisement?

Hypotheses **1 and 2** are the most likely given the symptom (self-query interference; missing re-announce), and the S31-is-different clue points hardest at a **timing/cadence** interaction (#1) or a **re-announce** gap (#2).

## Next concrete steps (in order)

1. **Add advertise-side diagnostics, not query-side.** Log on each board: the exact `mdns_service_add` / instance-name-set return codes, and (if available) any mDNS announce events. Confirm the service stays *registered* over minutes (query the local mDNS service table) vs. silently dropping.
2. **Test hypothesis #1 cheaply:** make DevicesModule NOT query the service types it advertises (or gate self-hosted services out of the query rotation) — leaving only foreign-service discovery — and watch whether the board becomes reliably discoverable by peers. If yes, the self-query-vs-self-advertise interaction is confirmed; the fix is to separate them (don't browse what you advertise, or use a dedicated query interface).
3. **Test hypothesis #2:** compare advertised TTL to WLED's; add a periodic `mdns` service re-announce on a slow timer if IDF doesn't auto-refresh; watch a peer's discovery persistence over 5–10 minutes.
4. **Compare the S31 bring-up to the others** at the netif/mDNS-start boundary (#3) — it is the one board that advertises reliably; whatever it does differently is the clue.
5. Only after the root cause is identified, decide whether mDNS alone suffices for projectMM↔projectMM or whether a **UDP presence beacon for our own peers** (we control both ends; the product owner's original instinct) is the more robust path — keeping mDNS for foreign-system discovery + the WLED-app `_wled._tcp` advertise either way.

## Two related UI observations (from the device card)

- **A projectMM peer can show as `WLED` when only its `_wled._tcp` hit arrived.** Seen live: `MM-S3 · 192.168.1.157 · WLED`. MM-S3 is a projectMM device, but the discovering board only received its `_wled._tcp` announcement, never its `_http._tcp`+`mm=1` one (the advertise-instability above) — so the classification stayed WLED. The classify→upsert priority is correct (a tested raise-to-projectMM on the `_http`+mm=1 hit); the row is wrong only because that authoritative hit never reached this board. **This bug is a *symptom* of the advertise instability** — it self-corrects once both of a projectMM device's services advertise reliably. (A possible mitigation independent of the root fix: a projectMM device could carry an `mm=1` TXT on its `_wled._tcp` advertisement too, so even a `_wled`-only sighting classifies it projectMM. Worth considering when fixing the root cause.)
- **A WLED device shows its mDNS hostname (`wled-c6a5dc`), not its friendly name (`WLEDMM-LowlandsLine`).** Confirmed: WLED's `_wled._tcp` mDNS carries only the `wled-XXXXXX` host name + a `mac=` TXT — the **friendly name lives only in WLED's `/json/info` `"name"`**, not in mDNS. So showing it (and, later, brightness/on-off) requires a **REST `/json/info` fetch after discovery** — the "discovery (mDNS, gets IP) → state (REST, gets name/brightness)" pattern noted in the plan's out-of-scope. This is exactly what WLED's own app does. A follow-up feature, the same mechanism as live peer-state; not a quick fix because the data simply isn't in the mDNS record.

## What works today (for the user-facing record)

WLED discovery (both directions: list WLED + appear in WLED apps via the `/json/info` shim + `_wled._tcp` advertise), the plugin architecture, classification + rename, the live WS-driven UI, and *partial* projectMM-peer discovery (at least one board is always found). The instability is in projectMM-peer *advertisement reach*, characterised above.
