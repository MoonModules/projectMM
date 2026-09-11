# Plan, Lean preview transport

(PR #81: all four steps implemented, gates green, bench acceptance on four boards + desktop.)

## The evidence that forces this

One day of bench time produced three symptoms with one root cause:

1. A reconnect storm on `/wsp` (closes at the browser's ~1.3 s retry cadence, clients dying with
   zero frames received).
2. Blank previews after a refresh (zombie slots at the 4-client cap, admission refused).
3. `renderWait` spiking 11 us to ~180,000 us on the Drivers card: WiFi congestion charged to the
   LEDs themselves.

The root cause is the **synchronous send path**: coord tables and downsampled frames stream
all-or-nothing with a 150 ms stall budget, and a stall **closes the client**. Congestion becomes
disconnects; disconnects reset the client's adaptation and re-trigger table streams; the output
core is held for the whole budget. The resumable per-client-cursor path (used by full-res frames)
has none of these problems. We built the right mechanism and then routed half the traffic around
it.

## Design: one path, one signal, cached geometry

### 1. One resumable path for every `/wsp` message

Everything (coord tables, color frames at any stride) is queued as header + stable body and
drained per client by `writeSome` on tick20ms: each socket takes bytes at its own TCP pace, the
render/output path never waits on a socket, and a client is closed **only on a real error or FIN,
never for slowness**. TCP's own flow control is the bandwidth adaptation; the device's job is
only to drop frames at the source (drop-new: a frame offered while the slot drains is skipped).

Deleted outright: `sendAllOrClose`, the begin/push/end fan-out with its per-client skip mask,
`kDirectSendBudgetMs`, close-on-slow, the coord-stream rate limit, and the reap/admission lease
contention that close-churn made critical. The sender lease shrinks to protecting the arm/drain
handoff of one slot.

Cost: the coord table and downsampled frames need a stable body. The downsampled path already
gathers into a staging buffer; the coord table gets a small owned buffer (3 bytes per kept point,
at most ~48 KB on the biggest config, allocated at rebuild, freed after drain).

### 2. Pull, not push: the device is a dumb data producer

The client steers everything through two requests; the device holds no delivery policy at all,
only its caps and the drain.

- `[0x51][stride][fps]`, a **standing frame request**: serve me frames at this stride and rate.
  Across clients the most conservative request wins (coarsest stride, lowest rate), bounded by
  the memory/display caps and the `targetFps` control (the ceiling and the default for a client
  that sends only a stride). **No standing request means the device builds nothing at all**,
  subsuming today's nobody-watching gate.
- `[0x52][stride]`, a one-shot **table request**: send me the coordinate table for this stride,
  paced through the drain like everything else. The device never volunteers a table, so the
  entire client-generation machinery (the bump, the re-stream on connect, the reconnect storm it
  fed) is deleted; a fresh or reconnecting client simply asks.

The 0x03 table and 0x02 frames carry a 1-byte **geometry epoch** (bumped on every rebuild). The
client caches tables keyed (epoch, stride); a frame whose (epoch, stride) hits the cache renders
immediately, a miss triggers one `[0x52]` request. A stride change to a cached rung costs zero
table traffic.

One format for every layout: the point list. A dense-grid shortcut (a closed-form descriptor)
was considered and rejected as a special-case fork, the same call made earlier against special
handling of identity mappings; the table is one-shot and cached, so its size never touches the
steady state.

Client memory: the full ladder (strides 1..64) sums to about 1.33x the stride-1 table. Worst case
today (16384 points) that is ~65 KB on the wire and ~260 KB as Float32Arrays in the browser.
Browsers handle hundreds of MB; this is a non-issue.

The channel then carries almost nothing but frames: the lean throughput channel. The UI ships
embedded in the same firmware image, so client and device always match; no skew case exists.

### The boundary rule: the channel is core, the producer is domain

Core owns the whole **preview channel**, written once for any domain (a non-light app built on
the core gets its preview transport for free): the `/wsp` lifecycle, per-client OPAQUE standing
request bytes, one-shot request forwarding, the resumable paced drain, drop-new with the drops
counter. Core never interprets a request or a frame.

A domain owns the **producer**: light's `PreviewDriver` interprets `[stride][fps]`, aggregates
most-conservative across clients, and builds tables and frames. The channel machinery already
lives in `HttpServerModule` (core); this step formalizes the interface rather than moving code.

### 3. Adaptation: a sender-side congestion signal, not probing

The device knows precisely when the link is behind: it **drops a frame at the arm** because the
previous one has not drained. That is a direct backpressure signal, so put it on the wire: the
0x02 header gains a 1-byte **drops-since-last-frame** counter.

The client controller becomes two rules (replacing the bands, the coarsen-must-pay audit, the
source-limit hold and the failed-stride memory, all of which existed to compensate for a blind
fps-only measurement):

- **Coarsen** when drops stay nonzero over a window: the device is discarding frames, so the
  frames are too big for the link at this rate. Effective fps stays near target and the picture
  coarsens, the trade `targetFps` advertises.
- **Refine** cautiously when a window is clean (zero drops) and the achieved rate is near target:
  step one rung finer. If drops reappear, step back and **back off exponentially** on further
  refine attempts (the abandon-fast, retry-slowly rule ABR players use against oscillation).
  A renderer-limited device (a heavy effect at 6 fps) reports zero drops at full detail, so the
  client correctly never coarsens: the case the audit machinery existed for, now free.

Closed-form seeding: frame size per rung is known (`count(stride) * 3 + 7`), so on connect the
client picks the finest rung whose `size * targetFps` fits the last measured byte throughput
(EWMA), with a 0.8 safety factor, instead of starting blind at stride 1 and stalling the link on
the first frame after every refresh.

Self-repair: the client re-sends its standing request after ~2 s without a frame. A device
reboot (standing requests lost) or a dropped uplink heals itself; a viewer never sits in front
of a silently dead preview.

**The invariant, which is also the acceptance bar:** at the chosen `targetFps`, degradation
sheds SIZE first (the stride ladder, non-oscillating via backoff); when even the coarsest stride
cannot hold the rate, the RATE sags through drop-new. Never a stall, never a disconnect for
slowness, never an absent preview while a viewer stands. Higher target = smaller and smoother;
lower target = finer and slower; the balance always holds.

### 4. What stays

The `/ws` state slot (StateSend), coarsest-of-clients (now most-conservative-of-clients), the
tab hibernation, the memory/display point caps. `targetFps` stays as the device-side ceiling
control with its range tightened to 1-25 (default 24): the 20 ms drain cadence tops out near
50 fps anyway, and 25 is more than a preview needs, so the range now promises only what the
transport comfortably delivers.

## Steps

1. **Transport**: route the coord table and downsampled frames through the resumable slot (owned
   staging bodies); delete the synchronous path and its budget/skip/close machinery; close only
   on error/FIN. The lease shrinks accordingly. *Test*: a wedged mock client never blocks a tick
   and is never closed for slowness; unit drain tests cover multi-message sequencing.
2. **Wire**: the `[0x51][stride][fps]` standing request and `[0x52][stride]` table request;
   epoch byte in 0x03/0x02, drops counter in 0x02; the device answers requests and volunteers
   nothing (client-generation watching deleted). *Test*: header layout pinned; no request means
   no frames; a stride flip with a cached table sends no 0x03; a table request is answered once;
   every layout answers with the one point-list format.
3. **Client**: per-(epoch, stride) table cache; the two-rule controller with exponential refine
   backoff and throughput seeding; delete the old band machinery from `preview-adapt.js` (the
   pure-function + test shape stays). *Test*: drops coarsen, clean windows refine, a failed
   refine backs off 2x each retry, renderer-limited holds full detail, dead link settles without
   flapping.
4. **Bench, the acceptance bar**: on the S3/WiFi with two viewers: `renderWait` flat (< 10 ms
   always), zero `/wsp` closes over 10 minutes of steady viewing, refresh shows the preview
   within a second every time, stride settles without oscillation at target 60 and returns to
   full detail at target 5. Then the other three boards + desktop.

## Verification

`ctest` + `test/js` + scenarios + spec check; the step-4 bench matrix; the product owner's eyes
on the S3/WiFi case, which has been the truth-teller all day.
