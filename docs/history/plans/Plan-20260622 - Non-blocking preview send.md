# Plan — Non-blocking preview send: high-resolution preview without stalling the render task

## Context

**The problem.** On a large grid (128² = 16K LEDs) the WebSocket preview shows nothing or kills the connection. The goal: the preview runs fluently at large sizes, reaching **16K on a no-PSRAM classic ESP32** and higher on PSRAM boards.

**What the measurements proved (P4, 2026-06-22) — our own diagnosis.** The wall is **not** RAM, **not** bandwidth (72 KB/s at 128²), **not** the render tick (343 µs, 8× headroom). The wall is the *send mechanism*: a preview frame is one **synchronous `writev` on the shared HTTP/render task**.
- A frame over ~5 KB (≈1700 points) can't go out in one `lwip_writev`; it partial-writes → `broadcastBinary` closes the connection (preview vanishes above ~48²). Bisected live: 32² streams, 48² drops.
- Raising the drain budget made it **worse** — a 60 ms drain at 128² blocked the shared task long enough to starve HTTP accept; the WS handshake itself failed. Confirmed live.

**Root cause, named.** We're doing a **blocking, all-or-nothing send on the render thread**. The textbook fix is the standard one for any producer that must hand bulk data to a slower consumer over a socket: **a non-blocking bounded queue with backpressure** — the producer copies the frame and returns; a separate drain step writes it out in slices as the socket accepts them; if the producer outruns the consumer, the newest frame is **dropped, not blocked** (backpressure). This is the same producer/consumer discipline the render pipeline already uses (effects produce, drivers consume); here the consumer is the socket.

**Why this reaches 16K without downsampling (the analysis).** Once the send is enqueue-and-drain, the per-frame cost is no longer "what fits one `writev`." The remaining limits are concrete and addressable:
- **16-bit WS frame length** (`broadcastBinary` max 65535 B = 21843 pts) → extend to the RFC 6455 **64-bit length form** (~10 lines, localized). Ceiling gone.
- **`count` field is `u16`** in the frame header (max 65535 pts) → widen to `u32` (the system already supports >65K lights: `nrOfLightsType` is `u32` on PSRAM boards).
- **Staging-buffer RAM** (the queued frame, `pts*3`): 16K = **48 KB**, 64K = 192 KB. `platform::alloc` prefers PSRAM with internal-RAM fallback, so a classic board fits ~48 KB in internal RAM and PSRAM boards fit far larger. This *is* the "16K easily, above that needs PSRAM" boundary — derived, not guessed.
- **Drain latency** (not blocking): a 48 KB frame drains over ~10 `loop20ms` ticks ≈ 200 ms, so the preview frame-rate **adapts down** (~5 fps at 16K) — fine for a preview, the tick never stalls.

**Conclusion: raise the cap empirically, keep downsampling as the tested fallback — do NOT remove it.** The enqueue model lets the cap rise well past 1800, but we don't declare a number from a spreadsheet: per the test-first principle, **raise it and measure where it actually breaks on each board** (classic internal-RAM limit, PSRAM headroom, drain-latency floor). The spatial-lattice downsample (already built) **stays** as the deliberate fallback beyond the tested cap — bigger panels will hit limits, so the graceful-degrade path must remain. The cap is **RAM-derived with a measured safety margin**.

**>65K lights is a real target (big ArtNet HUB75 walls).** The system already supports it (`nrOfLightsType` u32 on PSRAM; GridLayout anticipates `512×512 > 65535`), but the preview wire `count` is `u16` — a contradiction. Widening to `u32` is in scope so a >65K panel can be previewed (downsampled to whatever staging RAM allows, but the *count* is no longer capped at 65535).

**Forward-compatible with the producer/consumer two-task split (architecture.md §145).** The two-core design lands soon. This enqueue/drain model **is that shape**: `broadcastBinary` enqueue = producer handoff; `drainWsSends()` = consumer transmit. Today both run on the one Scheduler thread; when §145 lands, **`drainWsSends()` moves to the consumer/network task unchanged** — the queue *is* the handoff boundary. A down-payment on §145, not a single-task hack.

## Approach

Three seams, all in core transport + the driver. No new task yet (it arrives with §145), no new module.

### 1. Non-blocking send queue with backpressure (`HttpServerModule`)

- **One staging buffer for the live-preview client**, sized to the RAM-derived point cap, allocated once via `platform::alloc` (PSRAM-preferred; classic falls back to internal RAM). Single live client (§2) → one buffer.
- `broadcastBinary` → **non-blocking enqueue**: **backpressure gate first** — if the live client still has unsent bytes from the previous frame, **drop this frame** (newest-wins). Else copy WS header + payload into the staging buffer, set `len`, `sent=0`, return. Never blocks.
- New **`HttpServerModule::drainWsSends()`** called from `loop20ms()`: flush the staging buffer with the **non-blocking** `writeSome` — send what the socket takes now, advance `sent`, leave the rest for the next tick. Mid-frame partial is expected (we own the offset); only a real socket error closes. The exact function §145 later hosts on the consumer task. (As implemented, the drain runs before the accept so a connection burst can't strand it.)
- **Extend `broadcastBinary`'s WS header to the 64-bit length form** so a >65535-byte frame is legal (replaces the current `else { return; }`).

### 2. Single live-preview client (bound the memory)

The preview is a *live view* — one viewer at a time is the real use case, and it bounds the staging buffer to one instance instead of `MAX_WS_CLIENTS`×48 KB. Target the **most-recently-connected** WS client (`wsClientGeneration_` already tracks new connections; PreviewDriver re-sends its coord table on a generation bump). State-JSON pushes still go to all clients — only the binary preview is single-target.

### 3. PreviewDriver: raise the cap empirically, widen count to u32, keep downsample fallback

- Replace fixed `MAX_PREVIEW_POINTS = 1800` with a **RAM-derived cap** (`platform::hasPsram`/`freeInternalHeap()`/`maxAllocBlock()` with a margin for stack/HTTP/WiFi), **tuned by measurement**. The spatial-lattice downsample **stays** and engages beyond the cap.
- **Widen the frame `count` field `u16 → u32`** (both 0x02 colour and 0x03 coord headers) on device and browser.
- **Does NOT touch the `rgb_`/`coords_` build buffers** — only how the built frame is *sent* and the count width. Zero-copy producer-buffer reuse + channelsPerLight/offset wire model are a separate deferred step.

## Files

**Core transport (the enqueue + drain — the §145-ready seam):**
1. **Edit** `src/core/HttpServerModule.h` — staging buffer (`wsPreviewBuf_`, `wsPreviewCap_/Len_/Sent_`, target client index + generation), `drainWsSends()` decl, free in `teardown()`.
2. **Edit** `src/core/HttpServerModule.cpp` — rewrite `broadcastBinary` as non-blocking enqueue with the backpressure gate; add the 64-bit WS length branch; add `drainWsSends()`; call it from `loop20ms()` after the accept early-return. Lazy-alloc staging via `platform::alloc`.

**Driver + wire format (RAM-derived cap, u32 count):**
3. **Edit** `src/light/drivers/PreviewDriver.h` — `MAX_PREVIEW_POINTS` → RAM-derived cap, tuned by measurement; keep the lattice fallback. Widen the 0x02/0x03 header `count` to `u32`.
4. **Edit** `src/ui/preview3d.js` — read `count` as `u32` (`getUint32`) in `renderPreviewFrame` (0x02) and `parsePreviewCoords` (0x03); adjust header offsets.

**Tests + docs:**
5. **Edit** `test/unit/light/unit_PreviewDriver.cpp` — count fits the RAM-derived cap + lattice-regularity; a grid past the cap still downsamples (fallback intact); a `u32`-count round-trip for a >65535-point grid. Add a `unit_HttpServerModule` case: a second `broadcastBinary` while the first is undrained is **dropped** (backpressure); `drainWsSends()` makes partial progress; the 64-bit WS length header is emitted for a >65535 B frame.
6. **Edit** `docs/moonmodules/light/drivers/PreviewDriver.md` + `docs/moonmodules/core/HttpServerModule.md` — non-blocking enqueue + backpressure-drop + RAM-derived cap + u32 count + 64-bit frames; update the wire-contract layout. Update `docs/architecture.md`: preview send is enqueue-on-produce + drain-on-transport-poll, never synchronous on the render tick; the drain is the consumer-side step §145 will host.

## Verification

- **Host:** `cmake --build build` (-Werror), `ctest`, `uv run scripts/scenario/run_scenario.py`, `check_specs.py`, `check_platform_boundary.py`.
- **ESP32 build** (`esp32p4-eth` + `esp32s3-n16r8` + classic `esp32`).
- **Live — find where it breaks (test-first):** websockets probe — sweep Grid 48²→64²→128²→195²→256²→512², recording at each size: WS open?, frame point-count (full vs downsampled), `/api/system` tick, preview fps. Locate the real break point **per board** and set the cap from that. Assert the WS never closes and the tick never stalls.
- **Classic board (the key test):** sweep toward 16K+, find where internal RAM / drain-latency forces downsampling, confirm the device degrades-never-crashes. Sets the classic-tier cap.
- **>65K (u32 count):** a grid above 65535 lights previews (downsampled) with a correct count — the HUB75-wall path.

## Risks / notes

- **Memory:** one staging buffer, RAM-derived; single live client keeps it ×1. Classic internal-RAM headroom is the binding constraint.
- **Drain latency, not blocking:** preview fps adapts down at big sizes; the tick never stalls. §145 consumer task can later drain continuously for smoother large previews.
- **`broadcastBinary` is preview-only** (only PreviewDriver calls it), so the contract change is safe.
- **Two-task forward-compat:** `drainWsSends()` is a standalone entry point §145 moves to the consumer task without a rewrite.
- **Downsampling stays** — raised, not removed; the lattice fallback is the tested graceful-degrade path.
- **Deferred (next commit):** zero-copy producer-buffer reuse + channelsPerLight/offset wire model.
- The diagnostic `writeChunks`/`maxDrainMs` machinery was removed entirely; the transport primitive is the non-blocking `writeSome`.
