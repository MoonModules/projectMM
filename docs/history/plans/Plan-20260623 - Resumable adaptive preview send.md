# Plan — Resumable, memory-adaptive preview send with adaptive frame rate

> Saved per CLAUDE.md *Plan before implementing* (2026-06-23). Product-owner archive; agents don't auto-read it.

## Problem (measured this session)

At 128²+ the preview send spins synchronously until the whole frame drains, stalling PreviewDriver's loop on a slow link (observed: classic WiFi AND P4 ethernet — "uptime not progressing"). The adaptive downscale doesn't rescue it on a fast-but-saturated link, because the frame *does* eventually send (`endBinaryFrame()` true) → no struggle signal → factor stays at 1.

## Core idea

Three coupled changes, all flowing from one principle — **adapt to what the link and the memory actually allow, measured not assumed**:

1. **Resumable send** (no spin, no buffer): a byte cursor over `{header, producerBuffer}`, drained across `loop20ms` ticks via the existing non-blocking `writeSome`. The producer buffer is already a stable contiguous block that persists across ticks, so no copy.
2. **Adaptive frame rate**: a frame only starts when the previous one finished draining → the send rate **self-limits to link speed**, with the `fps` slider as the *ceiling*. Shed frame rate before resolution.
3. **Memory-derived cap + chunk**: both `MAX_PREVIEW_POINTS` and the per-tick drain chunk come from `maxAllocBlock()`/free memory — per architecture.md *"Buffer counts and sizes are determined at runtime based on available memory and reallocated when configuration changes"* (§ Scaling to available memory). Replaces the `hasPsram ? 131072 : 16384` constant.

## CRITICAL INVARIANT — "always show something complete, never a partial frame"

A WebSocket message is **atomic to the browser**: `ws.onmessage` fires only when the *whole* message has arrived, and `renderPreviewFrame` rejects an incomplete buffer (`buf.byteLength < 7 + count*3 → return`). Therefore:

- The resumable send MUST keep each frame as **one complete WS message** at the browser level. "Resume across ticks" is about not *spinning the device loop* — the device still delivers a complete message, just spread over wall-clock; the browser draws it whole when it lands. Splitting a frame into multiple WS messages at the byte level would make the browser show **nothing** until 100% arrives (worse than today).
- "Best effort / show something" at huge grids (the 196² case) is **a complete frame that is either downsampled (sparse, every Nth light) or delivered at a reduced frame rate** — NEVER a torn/half-delivered frame. The no-tearing guarantee (fixed earlier this session via the count/stride match guard) is preserved.
- Concretely at 196² (38416 lights): classic (cap 16384) → downsampled complete sparse frame; PSRAM/P4 (under the old constant cap) → complete full-res frame delivered over more ticks at low effective fps. Both draw whole. The memory-derived cap (§3) decides which, per board, from actual free contiguous memory.

This invariant is the headline acceptance criterion: at every grid size on every board, the preview shows a **complete** frame (full-res, downsampled, or low-fps) and the device loop never stalls.

## Design

### §1 Resumable send (core)
`BinaryBroadcaster::sendBufferedFrame(header, hdrLen, body, bodyLen)` — `body` is the caller's stable producer buffer (pointer, NOT copied). HttpServerModule holds one in-flight send:

```
struct PreviewSend {
  uint8_t hdr[16]; size_t hdrLen;       // small WS+app header, COPIED (caller's is a stack local)
  const uint8_t* body; size_t bodyLen;   // producer buffer — pointer only
  size_t sent[MAX_WS_CLIENTS];           // per-client byte cursor (a slow client lags, not blocks)
  uint32_t bodyGeneration;               // invalidation tag (§4)
  bool active;
}
```

- `sendBufferedFrame` while one is **active** → newest-wins **drop** (backpressure). Else: send the WS header to each client, init cursors, mark active.
- `drainPreviewSend()` from `loop20ms`: per client, push **one memory-adaptive chunk** via `writeSome`, advance its cursor; a real socket error closes that client. When every live client reaches `bodyLen`, the send completes; expose the all-sent / idle result for the adaptive signal.
- **Subsumes CodeRabbit R5**: `PreviewSend` IS the frame-level state R5 asked for (remaining = `bodyLen - sent[i]`, a frame-wide budget, over-push guard) — built once, here, instead of grafting it onto the spinning `sendAllOrClose`.

### §2 Adaptive frame rate (the elegant part)
PreviewDriver only calls `sendBufferedFrame` when `bufferedSendIdle()` (previous frame fully drained). So:
- Fast link → drains in ~1 tick → next frame fires next loop → runs at the `fps` ceiling.
- Slow link → drains over many ticks → next frame waits → **effective fps drops automatically to what the link sustains.** Zero extra logic; the resumable send *is* the rate limiter.
- `fps` slider becomes the **max** fps (spec/label intent; keep the control name `fps`). Status line shows effective fps when below the ceiling ("preview · 6/24 fps · link limited").
- **Degradation order** (textbook, like video): shed frame rate first (this, free); downscale resolution only when even a full-res frame can't drain within a bounded number of ticks at the floor (the deeper fallback). Re-tune `slowStreak_`/`cleanStreak_` against this latency signal (closes the old "195² churn" + the "P4 factor stuck at 1 on a slow link" follow-up: the latency signal fires even when the frame eventually sends).

### §3 Memory-derived cap
`MAX_PREVIEW_POINTS` → a runtime value from `maxAllocBlock()` (largest contiguous, any memory) with a reserve margin for stack/WiFi/HTTP. A fragmented classic downscales sooner; a big PSRAM board goes higher — replacing the `hasPsram` tiers, per arch.md § Scaling to available memory. The spatial-lattice downsample stays as the graceful fallback above the cap. Reserve margin = one named constant, tuned from the measured classic headroom (the 16K-at-128² figure), not a guess.

### §4 Robustness — invalidate on resize (the use-after-free guard)
`Buffer::allocate()` does `free()` + `alloc()`, so a grid resize mid-send dangles `body`. PreviewDriver bumps `bodyGeneration` and calls `cancelBufferedSend()` from `onBuildState()` (the geometry-change signal). `drainPreviewSend` checks the tag and abandons a stale send (those clients' partial WS messages end incomplete → the browser discards them → fresh coord table + frame next tick). **Regression test** pins it (resize during an active send ≠ use-after-free), per the robustness Hard Rule.

### §5 Multi-client
Per-client cursors over one shared body handle ≤4 clients (a slow client lags its own cursor, doesn't hold the others). Newest-wins drop → never two frames queued → bounded memory (one header copy + the cursors; the body is the producer buffer, not ours).

## Files
- `src/core/BinaryBroadcaster.h` — `sendBufferedFrame` / `cancelBufferedSend` / `bufferedSendIdle`.
- `src/core/HttpServerModule.h/.cpp` — `PreviewSend` state, `drainPreviewSend()` from `loop20ms`, memory-adaptive chunk, generation guard. The synchronous header push keeps a tight bounded spin; the body drains by cursor.
- `src/light/drivers/PreviewDriver.h` — full-res → `sendBufferedFrame`; gate on `bufferedSendIdle()`; memory-derived `MAX_PREVIEW_POINTS`; effective-fps status; re-tuned downscale on the latency signal; bump generation + cancel on `onBuildState`.
- `src/ui/preview3d.js` — status shows effective vs max fps + "link limited".
- Tests: `unit_HttpServerModule` (drain-across-ticks, newest-wins drop, cancel-on-generation, over-push guard); `unit_PreviewDriver` (full-res routes buffered; resize-during-send safe; effective-fps falls under a throttled broadcaster; the "complete frame at every size" invariant — downsample engages past the memory cap).
- Docs: `HttpServerModule.md` / `PreviewDriver.md` (resumable + adaptive-fps + memory-derived cap; remove the synchronous-stall caveat); `architecture.md` graceful-degradation made non-aspirational + cross-ref § Scaling to available memory.

## Verification
- Host: build -Werror, ctest (new cases), scenarios, spec, boundary.
- ESP32 S3 + classic build.
- **Live (the real test)**: classic 128² WiFi — uptime progresses while streaming (stall gone), effective fps drops gracefully. Sweep 16²→256² on classic/S3/P4: at EVERY size a **complete** frame shows (full-res, low-fps, or downsampled), the tick never stalls, fps adapts, downscale engages only past the memory cap. The 196² "show something" case shows a complete frame on every board. KPI tick unchanged at small sizes.

## Open question (one)
The memory-derived cap's reserve margin — derived from the measured classic headroom, a single named constant tuned by the live sweep. Flagged so it's not a surprise in the diff.
