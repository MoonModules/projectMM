# Plan, A lossy channel for the preview stream

(PR #81. The device-side adaptation this plan sketched was later replaced, see the Lean preview transport plan.)

## The problem, measured

On a 768×384 layout with the preview open, over 10 s on the single `/ws` socket:

| | msgs/s | KB/s | share |
|---|---|---|---|
| Preview colour frames (`0x02`) | 22.6 | **244.8** | **98.5%** |
| State pushes (JSON) | 2.2 | 3.8 | 1.5% |
| Coordinate table (`0x03`) | 0.1 | 1.1 |, |

Three symptoms follow, all reported by users and all explained by that ratio:

1. **The connection indicator flickers.** State pushes queue *behind* 10.8 KB preview frames on one TCP connection, textbook head-of-line blocking. The browser sees delayed messages and reports the link as troubled.
2. **Fields visibly refill with unchanged values.** Every state push runs `updateValues()`; a full state additionally runs `renderCards()`. At 2.2/s that is a DOM write over every visible control twice a second.
3. **The UI stops responding on a large layout.** The device is fine (`/api/state` answers in 8-12 ms under this load); the *browser* is decoding 22 frames/s and drawing them on the same main thread that handles clicks.

A user reported the compounding case: with a big grid, the add-module dropdown vanished before it could be clicked, and **disabling the preview did not help**, because the driver broadcasts to every connected client whether or not anyone is watching.

## The decision: a separate channel, and a higher cap

Two candidate directions, and the measurement picks between them.

**Rejected: cap the preview harder.** It treats the symptom by giving up the thing the product owner explicitly wants, the highest-resolution preview the hardware can sustain. Our cap is already a flat `kDisplayCap = 4096` "for ANY board", ignoring that a PSRAM board has the memory and the link for far more.

**Chosen: separate the two traffic classes, then raise the cap.** The preview is a **lossy** stream (a dropped frame is invisible; the next one is 44 ms away) sharing one **lossless** ordered connection with the control plane, where every message matters and latency is user-visible. That mixed-criticality pairing is the recognised cause of the head-of-line blocking above.

This is the industry-standard remedy, not a bespoke choice. *High Performance Browser Networking* (O'Reilly, ch. 17) names this exact case, "multiple classes of messages: high-priority updates, such as control traffic, and low-priority updates, such as background transfers", and gives two documented answers: **separate connections**, or an application-level priority queue driven by `bufferedAmount`. Concurrent WebSockets are the standard mitigation for WebSocket head-of-line blocking. (WebTransport over HTTP/3 would give unreliable datagrams natively and is the eventual destination, but as of 2026 no shipped ESP32 stack offers it.)

**Prior art, and it is our own.** WLED streams its live preview only to a client that asked for it (`wsLiveClientId`, set by `{"lv":true}`), never sends while that client's queue is non-empty (`queueLength() > 0`, retry in 20 ms), and caps at 256/1024 lights. **WLED-MM raised that cap to 4096, and 8192 on PSRAM boards** ("better preview on PSRAM boards"), while keeping both the opt-in and the queue check. That is the shape to carry forward: the opt-in and the backpressure are what make a high cap affordable.

**On the architecture boundary.** `BinaryBroadcaster` is a domain-neutral sink and stays one: the core still only takes bytes and broadcasts them, with no knowledge that they are a preview. What changes is *which* sink the driver pushes to, a producer's choice, the same shape as any capability flag. The core learns "there are two channels", never "one of them is a preview".

## Design

### 1. A second WebSocket path, `/wsp`

`HttpServerModule` gains a second upgrade route beside `/ws`, with its own client list and its own `BinaryBroadcaster` implementation. `/ws` keeps the control plane (JSON state, patches); `/wsp` carries binary preview frames only.

Two independent TCP connections means a preview frame in flight cannot delay a state push, which is the whole point. Cost: one extra socket per previewing client, bounded by the existing client cap.

**The socket budget, measured before designing against it.** `MAX_WS_CLIENTS` is **8**, inside a `CONFIG_LWIP_MAX_SOCKETS` of **16** shared with HTTP, mDNS, Art-Net, MQTT and OTA. A preview socket per WS client would need 16 WS sockets at the cap and leave nothing for anything else.

So the preview channel gets its **own, lower cap, 2 to 4**, refused beyond that rather than competing for the control plane's slots. That matches the use: a preview is something one or two people watch, while the control plane is what wants 8. A client refused a preview socket keeps its `/ws` connection and simply shows no preview (or falls back to the shared path, whichever the UI step below settles).

Raising `CONFIG_LWIP_MAX_SOCKETS` is a **fallback, not part of this plan**. At ~40-60 bytes of static RAM per socket, going to 24 costs about 400 bytes, but the IDF's own guidance is that lwIP "is not designed for many simultaneous connections" and that the socket count alone is not the whole story, the TCP PCB counts (`LWIP_MAX_ACTIVE_TCP`, `LWIP_MAX_LISTENING_TCP`, both 16 today) must move with it. Do it on measured evidence of exhaustion, never preemptively. (For scale: MoonLight allowed 100+ clients, which is far outside what lwIP is built for; 16 is the shipped default and [raising it even to 32](https://github.com/espressif/esp-idf/issues/14454) is an open upstream debate.)

### 2. Opt-in, so nothing streams when nobody watches

The preview streams **only** to clients connected on `/wsp`. Close the preview pane and the browser closes that socket; no client → the driver's `tick()` returns before building a frame. This is WLED's `wsLiveClientId` in a cleaner form: connection presence *is* the subscription, so there is no `{"lv":true}` message to lose track of.

This alone fixes the reported case, the user who turned the preview off and saw no improvement.

### 3. Backpressure: never queue behind yourself, ALREADY IMPLEMENTED

Verified during step 3 (2026-08-25): `PreviewDriver::tick()` already gates on
`broadcaster_->bufferedSendIdle()` and skips the slot when the previous frame is still draining,
incrementing `framesWaiting_` as the "link is behind" signal that drives the adaptive resolution.
That is WLED's `queueLength() > 0` check with a richer signal, and it needs no change.

Proven on the bench with a deliberately slow reader: a client draining at 2 KB/s received 2 KB/s
rather than the 241 KB/s the channel was producing, the server matched the reader instead of
queueing, and `/api/state` stayed responsive throughout.

This step is recorded rather than removed: it was in the plan because the design was written from
`maxPreviewPoints()` without reading the tick path, and the next person deserves to know the check
exists rather than adding a second one.

### 3b. (original text, for reference)

Before building a frame, ask the channel whether the previous one has drained. `BinaryBroadcaster` grows one query, `sendQueueDepth()` or a `bool busy()`, and the driver skips the slot when it is non-zero, exactly as WLED does. A lossy stream must *drop*, not queue: a queued frame is stale by the time it arrives and delays the next one.

We already have the raw material: `framesWaiting_` counts slots skipped because the previous frame is still draining, and `slowStreak_`/`cleanStreak_` drive the adaptive factor. This makes the signal explicit rather than inferred.

### 4. Measure throughput, and raise the cap to what the link sustains

Replace the flat `kDisplayCap = 4096` with a **measured** ceiling:

- Track bytes actually sent and the drain time per frame → an observed KB/s for this client's link.
- Raise the point cap while frames drain promptly, lower it when they do not, the existing `cleanStreak_`/`slowStreak_` machinery, applied to the ceiling rather than only to the divisor.
- Keep an absolute ceiling from **memory** (`maxAllocBlock()`, as today) and a floor (1024) so a tight board still previews.

Target the WLED-MM numbers as the starting envelope: **4096 baseline, 8192 on a PSRAM board**, and let the measurement go higher when a gigabit-linked desktop or S31 sustains it. The principle the product owner set: *if the infrastructure can deal with it, do not limit it.*

### 5. What we do NOT do

- **No UDP / WebTransport.** Genuinely the right transport for lossy pixel data, but no browser-reachable path exists on ESP32 today. Revisit when WebTransport ships.
- **No compression.** The frames are already downsampled; compressing costs CPU on the render core to save a link that a separate channel has already unblocked.
- **No change to the wire format.** `0x02` / `0x03` are unchanged; only which socket carries them.

## Status, implemented 2026-08-25

| Step | Result |
|---|---|
| 1. `/wsp` route + cap | **done.** `MAX_PREVIEW_CLIENTS = 4`, sized from observed use (1 browser typical, 2 common, 4 ceiling). Verified: 4 stored, the 5th refused, control plane untouched. |
| 2. Driver on `/wsp`, subscriber gate | **done.** Verified: no subscriber → 0 frames built; `/ws` carries no binary; `/wsp` carries no JSON. |
| 3. Backpressure | **already existed**, `bufferedSendIdle()` gating. Verified: a 2 KB/s reader received 2 KB/s rather than a 241 KB/s backlog. |
| 4. Measured cap | **done.** Ceiling 4096 → 16384 as frames drain promptly. On a 768×384 wall: stride **9 → 5**, points **3,698 → 11,858** (3.2× detail), stable (0 stride changes in 16 s), `/api/state` 9-31 ms under load. |
| 5. UI on `/wsp` | **done.** Verified end to end: dismissing the pane closed the socket within 2 s and re-opening restored it. |
| 6. Docs | **done.** Driver card + architecture § two channels. |

**Carried forward, unproven:** step 4 exposed a real gap, the struggle signal was only evaluated on slots where a frame *completed*, so a link bad enough that nothing completes produced no adaptation at all and the driver would sit forever at a resolution the link could not carry. `stuckWaiting` now treats a long unfinished wait as struggle on the same threshold. **That path has never been observed running**: the test mock clears its busy flag on every idle poll and so cannot hold a channel stalled, and a healthy link never triggers it. Weak WiFi is where it fires; watch for it there.

**Also watch:** bandwidth rose 202 → 738 KB/s on a local link, which is the resolution working as intended. The number to check is what it settles to over WiFi and 100 Mbit Ethernet.

## Steps

1. **`/wsp` route + a second broadcaster, with its own cap.** `HttpServerModule` hosts the path, tracks its clients against a preview-specific `MAX_PREVIEW_CLIENTS` (2-4, deliberately below `MAX_WS_CLIENTS`), and implements `BinaryBroadcaster` for it. *Tests:* a client on `/wsp` receives binary and no JSON; a client on `/ws` receives JSON and no preview frames; disconnecting one does not disturb the other; **an upgrade past the preview cap is refused cleanly and leaves the control plane's slots untouched**.
2. **Point the driver at it, and gate on subscribers.** `PreviewDriver` takes the preview broadcaster; `tick()` returns early with no clients. *Tests:* no clients → no frames built (pin it by counting, so the *work* is skipped, not just the send).
3. **`sendQueueDepth()` + skip-when-busy.** *Tests:* a busy channel skips the slot and does not queue; the frame after a drain goes out.
4. **Measured cap.** Throughput tracking, cap adaptation, PSRAM-aware ceiling. *Tests:* the cap rises on clean streaks and falls on slow ones, bounded by memory and the floor.
5. **UI:** `preview3d.js` opens `/wsp` when the preview pane is visible and closes it when hidden. *Test:* the JS suite pins that the preview socket is opened lazily and closed on hide.
6. **Docs:** the PreviewDriver card + architecture § the two channels and why.

## Risks

1. **Socket exhaustion.** 8 WS clients each with a preview socket = 16, which alone consumes the entire `CONFIG_LWIP_MAX_SOCKETS=16` budget and starves HTTP, mDNS and Art-Net. Mitigated by the separate lower preview cap (2-4) plus opening the socket only while the pane is visible, which puts a typical device at 2-3 total. **Verify by opening several browsers at once and watching for upgrade refusals and for Art-Net/mDNS failures**, which is where exhaustion would show first.
2. **The cap becoming a runaway.** A measured ceiling that only ever rises would rediscover the current problem at a higher resolution. The memory ceiling and the slow-streak backoff must both remain hard bounds.
3. **Two channels, two disconnect paths.** The UI must survive the preview socket dropping while the control socket lives, and vice versa. Pin both.
4. **The DOM-repaint symptom is NOT fixed by this plan** (symptom 2 above). Separating the channels stops preview traffic *delaying* state, but a 2.2/s state push still repaints unchanged controls. That is its own item; do not let this plan claim it.

## Verification

- The measurement that opened this plan, repeated: preview KB/s on `/wsp`, control-plane latency on `/ws`, with the preview at maximum resolution.
- `ctest`, the JS suite, scenarios, spec check.
- On the bench: a large layout on an S3 and on the S31, with the preview open, driving the UI at the same time, the case users reported. **The product owner's eyes: the dropdown must stay put and the indicator must stop flickering.**
- A desktop 1024×1024 run to find where the measured cap settles when the link is not the constraint.
