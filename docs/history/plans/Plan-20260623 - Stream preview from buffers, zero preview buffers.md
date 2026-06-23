# Plan — Stream the preview from the producer buffer; eliminate all preview-side buffers

## Context

The non-blocking preview rework (committed 1e48e92) made the WebSocket preview stream without stalling the render tick, but it introduced/retained **frame-sized buffers** in the preview path: `coords_` (~49 KB packed positions), `rgb_` (per-frame colour copy), `sampledIdx_` (the lattice index map), and the HttpServer **staging buffer** (~49 KB). On a no-PSRAM classic ESP32 these compete for scarce *contiguous* internal RAM: at 128² (16384 lights) the render buffer (49 KB) and a preview buffer (49 KB) can't both find a contiguous block once the heap fragments from grid-resize churn — so the preview (and sometimes the render) fails to allocate. Measured on the bench: a clean boot has a 108 KB contiguous block (128² fits); after resize churn it collapses to ~20–40 KB (128² fails).

The principle this violates is CLAUDE.md's **minimal memory / data-over-objects / hot-path** rules, applied to the preview: the colours already live in the **producer/consumer buffer** (the Layer's logical buffer, or the blend buffer for multi-layer/non-identity mapping). The preview should **stream that buffer to the client**, holding no frame-sized copy of its own. Positions are communicated **once** (event-based: on a layout/modifier change, or when a client connects/refreshes); after that the per-frame stream is just the buffer, 1:1, and the client already knows where each light goes.

architecture.md describes the preview *mechanism* (a one-time coord table + per-frame RGB, §"Output stage"/§UI) but does **not** state the memory model — that the per-frame stream is the producer buffer with no intermediate buffer, and downsampling is "send every Nth light." This plan implements that, and the doc gets updated to capture it.

## The model (settled with the product owner)

- **Coordinates are sent once** (0x03), on geometry change or client (re)connect. They need positions → built from `Layouts::forEachCoord` (a cold/rate-limited path, never the LED render hot path — verified: forEachCoord callers are LUT build, status, and the preview coord build only).
- **Colours are streamed per frame** (0x02) straight from the producer/consumer buffer the driver already holds (`sourceBuffer_`), **1:1, no copy**. The client places colour[i] at coord[i] from the table it already has.
- **Downsampling = send every Nth light**, applied identically to the coord table and the colour frame so they match by construction. Two regimes:
  - **Full resolution (the common case, stride 1):** colour frame is a pure 1:1 buffer stream — no `forEachCoord`, no skip, no buffers.
  - **Downsampled (rare: grid > cap, or link too slow):** to avoid the diagonal moiré that flat `i % N` striding causes on a 2D grid, both passes use the **spatial-lattice** skip (`x%s && y%s && z%s`) via `forEachCoord`. This walks positions (cheap integer loop, rate-limited, off the LED hot path) but still streams — no stored index map.
- **No preview-side frame buffers at all**: streaming via the broadcaster's begin/push/end means neither pass ever holds a frame-sized buffer. `coords_`, `rgb_`, `sampledIdx_`, and the HttpServer staging buffer are all removed.

## Approach

### 1. Broadcaster: streaming begin/push/end (already implemented)
`BinaryBroadcaster` gains `beginBinaryFrame(totalLen)` / `pushBinaryFrame(data,len)` / `endBinaryFrame()`. HttpServerModule sends the WS header on begin, fans each pushed slice to every client via the non-blocking `sendAllOrClose` (close a client that can't keep up — it reconnects), and reports all-sent on end. No frame-sized staging buffer.

### 2. PreviewDriver: stream both passes, drop the buffers
- **Coord table** (`buildAndSendCoordTable` → `streamCoordTable`): compute the per-axis lattice step `s` (1 = full res; >1 when the light count exceeds the cap or adaptive downscale raised it). `beginBinaryFrame(coordCount*3 + …)`, then walk `forEachCoord` pushing scaled (x,y,z) for lights on the lattice (`x%s && y%s && z%s`); `endBinaryFrame`. No `coords_`.
- **Colour frame** (`sendFrame`):
  - **stride 1:** `beginBinaryFrame(n*3)`, then push the producer buffer directly. If `cpl==3` it's one push of `sourceBuffer_->data()`; if `cpl!=3` (RGBW) push per-light 3 bytes through a tiny stack temp. No `rgb_`.
  - **stride > 1:** walk `forEachCoord`; for each light on the lattice push its 3 colour bytes from `sourceBuffer_[idx]`. Same predicate as the coord table → same subset/order. No `sampledIdx_`.
- Remove members: `coords_`, `rgb_`, `sampledIdx_`/`sampledIdxCap_`, and the `~PreviewDriver` delete.
- Keep: the **cap** (now just "downsample above N points" — bounds the per-frame work/wire size, not a buffer), the **adaptive downscale** (latency + pending-drop driven), `coordPending_` retry, the u32 count, the browser count/stride guard.

### 3. HttpServerModule: remove the staging machinery
With both passes streamed, the staging buffer + `wsPreviewBuf_/Cap_/Len_/Sent_[]`, the stage-vs-DIRECT branch in `broadcastBinary`, `drainWsSends`, `directBroadcast`, and the drain-tick/stuck-client guard are no longer used by the preview. `broadcastBinary` (the chunk-array form) and `lastDrainTicks` may become unused → remove what's dead. (Adaptive downscale now keys off `endBinaryFrame()` returning false / the coord-pending retry, not `lastDrainTicks` — confirm and simplify.)

### 4. Tests + docs
- `unit_PreviewDriver`: update the `CaptureBroadcaster` mock to implement begin/push/end (accumulate pushed bytes into `lastCoord`/`lastFrame`). Keep the assertions (count, header sizes, lattice regularity, full-res-not-downsampled, coord-pending retry). Add: colour frame at stride 1 equals the source buffer (1:1, no copy).
- `docs/moonmodules/light/drivers/PreviewDriver.md` + `core/HttpServerModule.md`: rewrite to the streamed model (no buffers; positions once; colours 1:1 from the producer buffer; every-Nth downsample; begin/push/end wire).
- `docs/architecture.md`: add the preview **memory model** to the output-stage/UI section — the preview streams the producer buffer with no intermediate copy; this is the data-over-objects / minimal-memory principle applied to the preview.

## Files
- `src/core/BinaryBroadcaster.h` — begin/push/end (done); remove `broadcastBinary` chunk-form + `lastDrainTicks` if dead.
- `src/core/HttpServerModule.h/.cpp` — begin/push/end impl (done); remove staging buffer + drain machinery + stage/DIRECT.
- `src/light/drivers/PreviewDriver.h` — stream both passes; drop `coords_`/`rgb_`/`sampledIdx_`; keep cap + adaptive.
- `test/unit/light/unit_PreviewDriver.cpp` — mock + assertions for the streamed model.
- docs: PreviewDriver.md, HttpServerModule.md, architecture.md.

## Verification
- Host: build (-Werror), ctest, scenarios, spec, platform-boundary.
- ESP32: S3 + classic build.
- **Classic 128² (the target):** with `coords_`/`rgb_`/`sampledIdx_`/staging gone, confirm the render buffer allocates AND the preview streams at 128² without those competing 49 KB blocks; measure `freeInternal`/`maxBlock` to confirm the contiguous-RAM pressure is relieved. Confirm full-res streams 1:1 (no moiré) and a grid past the cap downsamples cleanly (no moiré, matched colour/coord counts).
- S3/P4: confirm no regression (full-res 128²+ still streams; adaptive downscale still engages on a slow link).

## Risks / notes
- **Streaming is synchronous on the preview loop** (rate-limited ≤ fps, off the LED render tick). A slow client is closed (bounded), never an unbounded tick stall. The adaptive downscale shrinks frames on slow links so per-tick send stays small.
- **Multi-client**: each pushed slice fans to all clients in order; a forward-only producer (forEachCoord / buffer walk) is walked once per frame, slices sent to all. Fine for the handful of WS clients.
- **cpl≠3 (RGBW)** stays a per-light 3-byte push (no buffer); cpl==3 is the bulk 1:1 push.
- This is a net **subtraction**: removes ~3 buffers + the staging/drain code; the colour hot path becomes "stream the buffer."
