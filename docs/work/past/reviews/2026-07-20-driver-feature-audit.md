# Driver feature audit — pre-merge review of `next-iteration` (2026-07-20)

A Fable-agent review of the whole `next-iteration` branch (10 commits, 92 files, ~10.3K insertions) before it merges to `main`, run per the product owner's request. **Part A** is the standard pre-merge drift review; **Part B** is the per-driver, per-feature inventory (modularity / cost / still-needed?) that is the basis for the merge-prep actions.

**Status of the Part A findings (updated 2026-07-20 after the review):**

| # | Finding | Status |
|---|---------|--------|
| A1 | `Drivers::tick()` cross-core race on quiesce timeout | **FIXED** — `tick()` now `stopEncodeTask()`s the wedged worker before the inline fallback. |
| A2 | `isr_cache_safe` + `termNode` + "OPEN BUG" comments contradict the code | **FIXED** — all four comment blocks rewritten to present-state truth. |
| A3 | Per-row bench diagnostics in the hot encode loop | **Deferred** (post-merge) — diagnostics are kept deliberately through the tuning era (PO). |
| A4 | Duplicated slice-fill (ISR vs prime) | **FIXED** — one shared `fillSlice()`; `ea` real-refill accounting preserved (wall-verified). |
| A5 | Two hand-rolled fork-join workers | **Deferred** (post-merge) — backlog a core `ForkJoinWorker` primitive. |
| A6 | `ringDbg` cryptic 18-field control | **Kept** — diagnostic, tuning era, PO wants it. Trim post-tuning. |
| A7 | Stale ~18ms cost number in the fork-join rationale | **Deferred** — folds into the "measure-then-delete the snapshot half" follow-up. |

The **MERGE-PREP ACTION LIST** at the end is the forward-looking part; items in its group 1 (before-merge) are done, groups 2-3 are the keep/follow-up decisions. This doc is the record — the branch commit and the backlog carry the actions.

---

## PART A — Pre-merge drift review (ranked)

**A1. HIGH — Drivers::tick() races a live core-1 encode after a quiesce timeout.** `Drivers.h:375-379 + 519-531`. When `quiesceEncode()` times out in `tick()`, it sets `renderSplitActive_ = false` and returns — but does **not** stop/join the worker. `tick()` then immediately composites into `outputBuffer_` and falls through to `MoonModule::tick()`, ticking every driver inline on core 0 **while the wedged core-1 task may still be inside a driver's `tick()`** reading the same buffer and the same `inFlight_[]`/bus state. Failure scenario: a transiently-starved worker (the exact case the timeout exists for) un-wedges a moment later → two cores concurrently in one driver's `tickAsync()` → double `busTransmit` on one buffer, corrupted `inFlight_` bookkeeping, potentially a freed-buffer read on the next prepare. `prepare()` and `quiesce()` both already do `if (!quiesceEncode()) stopEncodeTask();` — tick() is the one caller that doesn't. Minimal fix: in `tick()`, on `quiesceEncode()` failure call `stopEncodeTask()` (a join; slow but this is the declared-broken path) before falling back inline — the same rule the other two call sites follow.

**A2. MEDIUM — Safety-invariant comments contradict the code (isr_cache_safe).** `platform_esp32_moon_i80.cpp:364-373` (the `moonI80EofCb` header block) and `platform.h:780-793` both state "the channel does NOT set `isr_cache_safe` … a flash-resident callback is permitted … full-flash-write hardening (isr_cache_safe + IRAM encode) is a later increment." But `initRingDma` **does** set `chanCfg.flags.isr_cache_safe = true` (line 1053) and the whole encode chain is now MM_RAMFUNC/IRAM — that "later increment" shipped, and the ISR carries the `spi_flash_cache_enabled()` defer guard (line 397) precisely because of it. A future editor trusting the comment could legitimately remove the defer guard or move the encode back to flash and get the measured Cache-error panic back. These are the two doc blocks that define the ISR's safety contract; rewrite both to the present state (ring channel = cache-safe + IRAM chain + defer guard; whole-frame channel = not cache-safe, flash callback fine). Also stale: `MoonI80State::termNode`'s comment (line 262-265) says "so the next arm can restore its loop link" — no restore code exists; it is diagnostic-only (`termNodeDiag`).

**A3. MEDIUM — Per-row bench diagnostics are unconditionally compiled into the shift encode hot loop.** `ParallelLedDriver.h:817-848` (`dbgSegGatherCy/EmitCy/Rows`, two `platform::cycleCount()` calls + three volatile RMWs **per row**, marked "TEMP DIAGNOSTIC") and `tickRing`'s `dbgTickWaitUs/SnapUs/PrimeUs` with the hardcoded `constexpr uint32_t kCyPerUs = 240; // S3 at 240 MHz` (line 555). Three problems: (a) they run in the ISR refill and on every whole-frame shift encode, for every user, forever — a few % of the hottest loop paid for a bench instrument; (b) `kCyPerUs=240` is wrong on the P4 (360 MHz) and meaningless on desktop (cycleCount is ns there) — a bespoke constant where `platform` owns clock facts; (c) they are `static inline volatile` on a class template, shared across instances (documented, but a 2-driver board reads garbage). These earned their keep during the 48×256 hunt; at merge they should be gated (an `MM_RING_DIAG` compile flag or deleted with the lesson recorded). Same for the `segT1/segT2` "TEMP DIAGNOSTIC" stamps inside `encodeRows`.

**A4. MEDIUM — Duplicated slice-fill logic in the platform ring (ISR vs prime).** `platform_esp32_moon_i80.cpp:427-472` (EOF ISR batch refill) and `965-994` (`primeRingRange`) implement the same body twice: short-last-slice tail memset + `shortSlice → bufNeedsPrefill`, encode-slice, past-frame zero-fill + `bufNeedsPrefill`, and the `s == nSlices` frame-close call. That is the *No duplication* smell in the most delicate code in the branch — the two copies have already diverged once (the ISR adds timing/late instrumentation) and any future fix (e.g. the close-word rule) must be made twice. Minimal fix: one `fillSlice(st, slot, sliceIdx)` helper both call (IRAM).

**A5. LOW — Two hand-rolled fork-join worker patterns, one mechanism.** `Drivers` (encodeTask_/encodeDone_/encodeStop_/quiesceEncode, Drivers.h:471-583) and `MoonLedDriver` (snapHelper_/snapHelperDone_/snapHelperStop_/helperJoin, MoonLedDriver.h:511-598) are the same construct — spawn-parked worker, notify-kick, acquire-spin join with timeout + self-heal — written twice with slightly different timeouts and degradation latches. Per *"when core already owns a mechanism for one path, extend it"* this wants a small core/platform `ForkJoinWorker` primitive; both call sites would shrink to a few lines. Not a merge blocker (both copies are individually correct and tested), but it should be named in the backlog as the standard fix, per the interim rule in CLAUDE.md.

**A6. LOW — `ringDbg` is a bespoke 18-field cryptic string control.** `MoonLedDriver.h:289-307` — `"sl%u/bf%u dn%u ld%u lt%u tx%u ipb%u ci%u tn%d de%u enc%u ea%u sg%u se%u tw%u ts%u tp%u gap%u"`. No widely-used project ships a UI control like this; it is a serial-log line living in a control (the stated reason — /api/state polling beats serial scraping — is real and recorded, so it passes the bespoke-with-reason bar *as a diagnostic*). It also carries the marker "TEMP DIAGNOSTIC" on its backing buffer (line 602). Post-merge it should shrink to the few fields that remain meaningful in operation (`lt`, `enc/ea`, `gap`) or move behind a debug build. Related nit: `refreshBusKpi`'s read-and-clear of the ISR-written `dbgSeg*` volatiles is a cross-context RMW race — harmless for a diagnostic, but that's another reason to gate it.

**A7. LOW — stale cost numbers in MoonLedDriver's fork-join rationale.** `MoonLedDriver.h:512-513`: "the snapshot correction (~18 ms at 48×256) and the pool prime (~14 ms)". The pre-corrected snapshot was replaced by the raw memcpy + fused correction this same branch (encodeRows doc, ParallelLedDriver.h:796-803: "deletes the whole ~4.7 ms pre-correction pass"), so the ~18 ms figure describes a deleted mechanism and currently over-justifies the snapshot half of the fork-join (see B, snapshot fork-join). Fix the comment (and see the action list — the snapshot half itself may now be removable).

**A8. Clean.** Domain boundary: `check_platform_boundary.py` passes; all LCD_CAM/GDMA/FreeRTOS code is in `src/platform/esp32/`; the drivers reach hardware only through `platform::` seams; `MM_RAMFUNC` is a platform_config macro (empty on desktop) — the right shape. `pinExpanderMode()` (a pass-through alias of `pinExpander`) carries its stated reason at the site; borderline but within the rules. Spec/docs: ADR-0014, the two catalog pages, and MIGRATING.md all landed with the rename (`I80LedDriver`→`MultiPinLedDriver`, ✓ with a migration note); `kExactLaneCount`→`kPowerOfTwoBus` rename is consistent. Tests: the branch adds serious pinning — unit_ParallelLedDriver_ring.cpp (978 lines), _pinexpander (545), unit_ParallelSlots growth (+457), unit_MoonLedDriver (141) — including the recycled-buffer==fresh and prefill+data==whole-slot equivalences the correctness story depends on.

---

## PART B — Driver feature audit

### DriverBase (shared base)

| Feature | Modularity | Cost (flash / memory+degradation / hot-path) | Still needed? |
|---|---|---|---|
| preset/whiteMode/localBrightness correction controls + `Correction` LUT | Clean: base owns wiring, `Correction` is a flat POD applied per light. | LUT = 256 B/driver; apply() is per-light hot but integer/LUT. | **Keep — load-bearing** for every physical driver. |
| `wire_` scratch (now internal-RAM-first, ×kMaxCores) | Clean grow-only lifecycle on base; per-CPU slicing is the textbook per-CPU-data pattern. | ≤ 64×outCh×2 ≈ 384 B. Internal-first is measured (PSRAM scratch multiplied encode). Degrades to alloc(), then idles. | **Keep.** |
| `driverHeapBytes()`/`publishHeapBytes()` accounting | Good shape: one virtual summing hook instead of setDynamicBytes pasted per alloc site — exactly the centralize-the-rule principle. | Zero hot-path (cold-path calls only). | **Keep.** |
| `setDrivingInfo(..., mode)` ring-regime suffix | Small, additive. | Nil. | Keep — "primed"/"lapping" in the status is genuinely user-meaningful. |
| `kFailBufLen` 48→64 | Trivial, justified by -Wformat-truncation. | +16 B. | Keep. |

### ParallelLedDriver (CRTP base — where most features live)

| Feature | Modularity | Cost (flash / memory+degradation / hot-path) | Still needed? |
|---|---|---|---|
| **doubleBuffer** (deferred-wait async, tickSync/tickAsync) | Clean: mode fixed by whether buffer 1 was allocated (no stale-flag routing); OFF path is byte-for-byte the original. Excision would be clean but unwanted. | 2nd DMA buffer (frame-sized; reserve-guarded, allocate-and-degrade ✓). Hot path: `max(encode,wire)` vs sum — the win. | **Keep — load-bearing** (48→76 fps measured). The A/B switch itself is cheap and documented as measurement knob; fine to keep. |
| **pinExpander + latchPin** ('595 shift mode) | Very clean at this layer: a bool + a pin; all geometry (`outputsPerPin`, latchBit, busWidth rounding, frame ×8) derives from it. Encoders live in ParallelSlots (domain-pure, host-tested). | Flash: the shift encoders are templated ×2 widths + IRAM-resident (MM_RAMFUNC) — a real few-KB IRAM cost on S3, but only on chips that compile them. Memory: frame ×8 (whole-frame) or ring pool. | **Keep — this is the 48×256 goal feature.** |
| **prefillShiftConstants / prefillShiftRows + needsPrefill skip** | Good split: constants once (cold), data-word-only per frame (hot); the buffer-lifecycle fact (`needsPrefill`) is computed by the one party that knows (platform) — right seam. | Saves 2/3 of encode stores (9.7→3 µs/light measured); prefill skip saved ~1/3 of ISR refill. | **Keep — load-bearing** for the ISR deadline. Note the mask-run loop in `prefillShiftRows` duplicates encodeRows' mask build (minor, acceptable). |
| **ringSnapshot (memcpy snapshot + fused correction)** | Clean: one bool, `encodeSrc_` bias pointer keeps encodeRows' index formula unchanged; sized off hot path; freed when OFF or on whole-frame fallback (readout honest). | ~36 KB internal at 48×256 (PSRAM fallback = measured ~10% encode cost, degrade not crash ✓). Hot: one windowed memcpy/frame. | **ON path is load-bearing** (ISR reads a frozen frame; UAF-on-resize guard). The **OFF A/B leg is now risk**, not lever: it re-opens the exact concurrent-read hazard the snapshot exists for, is reachable from the UI, and the question it answered (snapshot cost) is settled by the `ts` meter. Candidate: remove the control, keep the mechanism. |
| **Snapshot fork-join (snapHelperKick/copyRange/copyHelperRange/snapLineAlignedHalf)** | The hooks are clean CRTP no-ops on other drivers, but it drags 5 members + a gcd/cache-line-alignment function + `<numeric>` into the base for what is now a **single memcpy**. | Splitting one ~36 KB internal-SRAM memcpy across two cores is memory-bandwidth-bound — near-zero win post-fusion. | **Likely OBSOLETE — superseded by its own branch-mate.** It was designed for the ~18 ms *pre-corrected* snapshot (see A7); the raw memcpy this branch replaced it with is sub-ms. Measure `ts` with the helper off; if flat, delete the snapshotHalf job + alignment math (the **prime** fork-join stays — that one parallelizes real encode work, ~14 ms). |
| **tickRing** (async wait/snapshot/arm) | Clean third path, explicitly separated; never blocks a frame on the wire (the UI-refresh-freeze fix). | Hot: wait + memcpy + prime per frame; prime is the big one (fork-joined). | **Keep — load-bearing.** But strip the dbgTick* stamps / kCyPerUs (A3). |
| **busWaitIfBusy / deadFrames_ / busGaveUp + periodic retry** | Textbook give-up-with-retry breaker, well placed in the base (all backends inherit). Status re-derivation via parseConfig on recovery is correct. | Hot path: two branches/tick when healthy. | **Keep — load-bearing robustness** (the "misconfigured LED driver made the device unreachable" fix). |
| **waitBudgetMs (frame-derived timeout)** | Clean, derived not constant. | Nil. | Keep. |
| **Loopback self-test (private-bus)** + loopbackTxPin/loopbackStrand | Well-contained control cluster; conditional-hidden control shape is consistent. But it deinits/rebuilds the live bus — on the 48×256 ring that means re-allocating a ~121 KB pool post-test (fragmentation risk, acknowledged in code). | Zero unless enabled; allocs are control-driven. | **Keep** — it is the project's only ground-truth instrument (the memory notes repeatedly say "the wall is the instrument"; this is the machine version). |
| **loopbackIntrusive (ride mode + patternHoldStrand_)** | Cleaner than the private-bus mode for the ring (no teardown, no alloc, driver-agnostic via `ws2812LoopbackRide`); the pattern-hold hook in snapshotSourceForRing is a small but real domain-logic intrusion into the hot snapshot (one branch/frame, `patternHoldStrand_ >= 0` — pays one compare when off). Comment says "the coming loopbackMode dropdown folds this + loopbackTest into one" — mildly future-tense. | One compare/frame when off; `delayMs(40)` only in the control path. | **Keep** — it's the only test that can verify the ring at 48×256 *at zero extra RAM*. Fold the two bools into the promised dropdown post-merge (that comment is a forward-looking note in present-tense code — do it or cut the promise). |
| **frameTime KPI (tick1s)** | Clean, cheap, per the sub-hot-path rule. | One snprintf/s. | Keep. |
| **kMaxStrands=64 / uint64 activeMask + 32-bit halves** | The 32-bit-half discipline (Xtensa __ashldi3 lesson) is applied consistently in all four sites. | Hot-path win, measured. | Keep. |
| **dbgSeg*/dbgTick* statics** | See A3. | Per-row cost. | **Remove/gate before or shortly after merge.** |

### MoonLedDriver (+ platform_esp32_moon_i80.cpp)

| Feature | Modularity | Cost (flash / memory+degradation / hot-path) | Still needed? |
|---|---|---|---|
| **Own gapless i80 DMA backend (whole-frame)** | Exemplary platform split: driver is one-liner forwards; platform file mirrors esp_lcd function-for-function with cited line numbers; ADR-0014 records the decision; GPIO teardown on destroy is complete (matrix detach). | Flash: ~1.8 K-line platform file, S3/P4 only. Whole-frame path: same costs as esp_lcd sibling minus the ghost pins. | **Keep — load-bearing** (it's what proved the PSRAM-at-shift-clock measurement AND hosts the ring). |
| **useRing** (path selector) | Clean; the no-auto-router rationale (silent fallback hid the active path) is a genuinely good call, documented at the site. | Nil. | **Keep** the switch; whole-frame remains the A/B reference below ~96/strand and the fallback degrade path. |
| **Streaming ring: looping GDMA chain, ISR inline refill, clock oracle, prime-only self-termination** | The heart of the branch. Layering is right (platform owns descriptors/ISR/oracle; domain owns encode via the `MoonI80EncodeFn` seam with `needsPrefill` — a well-designed seam). Internals are intricate but every non-obvious choice carries its measured reason (auto_update_desc=false, one-node-per-buffer clamp, mark_eof-on-terminator-only, kResetLowUs timed reset, kLead/kBatchMax). | Memory: pool = rows×rowBytes×bufs internal DMA (auto up to ~free−64K reserve — at 48×256 ~121 KB, deliberate spend); reserve honored; falls back to whole-frame on any alloc failure ✓. Hot: EOF ISR at intr priority 3 encodes slices — *the* hot path; IRAM-resident; cache-off defer guard. | **Keep — THE load-bearing mechanism for 48×256.** Duplication finding A4 applies. The stale "OPEN BUG — 8..16 slices" comment block (lines 172-186) describes a bug the clock-oracle/lapping work has since resolved per the memory/commit trail — verify and rewrite present-tense, it currently tells a reader the ring is broken. |
| **ringAuto / ringRows / ringBufs / ringPadUs** | The DHCP write-back pattern (auto fills the visible controls) is recognisable and honest. Bounds shared via platform constants (kRingNodeMaxBytes etc.) so driver/platform can't drift — good. `busInitRing` mutating controls during prepare is unusual but documented. | Nil hot. | **Keep ringAuto + the three manual controls** (lapping frontier tuning is real, per the memory notes: pad is a per-wall hardware fact). Consider whether `ringRows` max of 64 in the control is honest when the node clamp makes 7 the effective max at 16 strands — the auto path shows real values, manual can silently clamp (documented, acceptable). |
| **shiftOverclock** (20 vs 26.67 MHz) | Clean switch-not-divider with wall-verified rationale; div plumbed as a file-static global (`g_shiftClockDiv`) — a documented, single-knob exception. | Nil. | **Keep — a real hardware A/B** (151 vs 118 fps; per-wall reliability). The default OFF matches the memory's reliability findings. |
| **Prime fork-join (primeHalf via snapHelper)** | Buffers are index-independent so disjoint ranges are safely parallel; join-fence-then-arm ordering is right; self-heal latch on timeout. | ~14 ms serial prime split across cores — real win at 48×256. | **Keep.** (The *snapshot* half of the same helper is the obsolete part — see ParallelLedDriver row.) |
| **ringDbg control** | See A6. | One snprintf/s. | **Trim post-merge** to lt/enc/ea/gap; delete the "TEMP DIAGNOSTIC" fields whose bugs are closed (ipb/ci/tn were the prime-only bug instruments; ld/dn the coalescing one). |
| **busRingMode ("primed"/"lapping")** | Clean. | Nil. | Keep. |
| **Loopback over the ring (copy-slice encoder, pool step-down, largest-first capture alloc)** | Thoughtful: tests the actual transport, geometry from the live controls. | Control-driven only. | Keep. |

### MultiPinLedDriver (esp_lcd reference)

| Feature | Modularity | Cost (flash / memory+degradation / hot-path) | Still needed? |
|---|---|---|---|
| **Whole driver (esp_lcd LCD_CAM/I2S i80)** | Excellent — ~230 lines of CRTP hooks, shares everything with ParallelLedDriver. | Nearly free (shares the base). | **Keep — load-bearing on classic ESP32** (the ONLY i80 path there, I2S backend). On S3/P4 it is the declared reference/default with a written retirement criterion ("retired only if the challenger demonstrably beats it"). Settle the S3/P4 A/B post-merge. |
| **clockMultiplier shift path through esp_lcd** (`i80Ws2812Init(..., clockMultiplier)`) | First-gen shift path; superseded in *capability* by MoonI80's ring. | Whole-frame ×8 (internal-RAM-bound, ~96 lights/strand cap). | **Keep for now; named RETIREMENT candidate** — the ring strictly supersedes it. Retiring it also deletes the ghost-pin/dcPin tax it drags along. Retire this leg first when the challenger is promoted. |
| **kSupportsPinExpander / kPowerOfTwoBus / kLoopbackFullWidth constexpr hooks** | Clean compile-time capability flags. | Zero (compile-time). | Keep. |

### ParlioLedDriver
Small delta: `kSupportsPinExpander=false` with the 65,535-byte transfer-cap reason at the site ✓; rename fallout only. All base features inherited; nothing ring/shift compiles for it. **Keep as-is.**

### RmtLedDriver
Delta is only `driverHeapBytes` accounting + rename comments. Unchanged behavior. Keep.

### PreviewDriver

| Feature | Modularity | Cost (flash / memory+degradation / hot-path) | Still needed? |
|---|---|---|---|
| **resumableFrames** (staged gather + resumable buffered send) | Clean: control + affectsPrepare, buffers allocated only when ON, freed when OFF, cancel-before-free UAF guards present, degradation statuses name *why* (alloc-miss → warning). | stage_ ~3×points (~24 KB), keptIdx_ cache; both accounted via driverHeapBytes. Removes a measured ~17 ms *synchronous socket write on the encode worker* — a sub-hot-path fix. | **ON is load-bearing** (the LED-hitch fix). The **OFF leg** is declared "the proven-correct reference to A/B on hardware" — legitimate short-term; once soaked, the OFF path (the blocking sender) is the thing the fix exists to kill. Candidate for post-merge removal of the *control*, keeping sync only as the automatic alloc-failure degrade (which refreshStatus already surfaces). |
| **keptIdx_ index cache** | Right: cache lifecycle == coord-table lifecycle, alloc-miss falls back to the full walk (correct-but-slower). | Removes an O(total-lights) forEachCoord walk per frame (~8 ms at 12K). | **Keep.** |

### NetworkSendDriver / HueDriver
**Unchanged on this branch** (not in the diff). No audit action; they inherit nothing from the new machinery (windowed DriverBase only).

### Drivers (container)

| Feature | Modularity | Cost (flash / memory+degradation / hot-path) | Still needed? |
|---|---|---|---|
| **multicore render↔encode split** (encodeTask_, quiesceEncode, forced identity outputBuffer_) | Right home (container owns the boundary, buffer, task); engage predicate from alloc *outcome* (allocate-and-degrade ✓); core's tickChildren gate reused on both sides ✓; quiesce() wired into structural mutations ✓. | One frame-sized handoff buffer + 8 KB task stack; hot: one atomic wait per frame (`renderWait` KPI measures it). | **Keep — load-bearing** (the core-0 network-starvation fix; also what makes the ring's core-1 tick + core-0 helper topology exist). Fix A1. |
| **renderWait KPI** (peak-per-window) | Clean; hidden when off. | Nil. | Keep — it is the declared Step-2b decision meter. |
| **multicore switch** | ON-is-better documented; OFF is the escape hatch. | Nil. | Keep the switch (escape hatch on a 2-core chip with a misbehaving worker is worth it). |

### Platform seams added (platform.h)
`allocInternal`, `cycleCount`, `currentCore`/`kMaxCores`, `cpuInfo`, `wifiApClientCount`, the MoonI80 family + `MoonI80RingStats`, shared ring constants (kRingRowsDefault/BufsDefault/PadMaxUs/NodeMaxBytes/BufsMin/Max), `ws2812LoopbackRide`, RmtLoopbackResult capture diagnostics. All are recognisable primitives with named precedents at their introduction sites (per-CPU data, rdtsc-class counter, DHCP-style constants sharing). `cycleCount`'s only questionable consumer is the diagnostics (A3) — the seam itself is fine and core-worthy. `kMaxCores=2` as an inline constexpr in platform.h is the right shape.

---

## MERGE-PREP ACTION LIST

> **Superseded (2026-07-20, later same day).** This forward-looking list is kept verbatim as the review's own record of what it recommended, but it is no longer the live action list — that belongs in `docs/backlog/`, not this history snapshot. What has happened since: group-1 items **A1, A2, A4 shipped** (see the status table above); the group-3 follow-up **"measure-then-delete the snapshot half of the fork-join" also shipped** — the snapshot fork was removed (serial snapshot on core 1, prime fork kept) as part of the flicker + idle-starvation fix, deleting `copyHelperRange`/`snapHelperLo_/Hi_`/`snapLineAlignedHalf` and the `HelperJob` enum. The still-open items (ringSnapshot OFF-leg retirement, PreviewDriver sync-send retirement, `loopbackMode` dropdown, `ringDbg` trim, the `ForkJoinWorker` core primitive, the S3/P4 A/B) live in [backlog-light.md](../../future/backlog-light.md) as the authoritative to-do. The list below is left intact as the retrospective it is.

### 1. Fix / simplify before merge
1. **Fix A1** — `Drivers::tick()`: on `quiesceEncode()` timeout, `stopEncodeTask()` before falling back inline (3-line change, closes a real cross-core race on the declared-broken path).
2. **Fix A2** — rewrite the two isr_cache_safe comment blocks (moon_i80.cpp:364-373, platform.h:780-793) and the `termNode` "restore" comment to present-tense truth; also verify-and-rewrite the "OPEN BUG — 8..16 slices" block (moon_i80.cpp:172-186) which describes a since-fixed failure as open.
3. **Gate or delete the TEMP diagnostics (A3)** — `dbgSegGatherCy/EmitCy/Rows` (per-row cycleCount in the ISR encode), `dbgTickWaitUs/SnapUs/PrimeUs` + the `kCyPerUs=240` hardcode, and their `sg/se/tw/ts/tp` fields in ringDbg. Cheapest honest form: one `MM_RING_DIAG` compile-time flag defaulting off; the lessons they produced are already in memory/lessons.
4. **De-duplicate the platform slice-fill (A4)** — one `fillSlice()` shared by the EOF ISR and `primeRingRange` (this is the code a future ring bug will be fixed in; two copies is how the fix gets missed).

### 2. Safe to keep (load-bearing or earning their A/B keep)
- The **ring** itself (looping chain, ISR refill, clock oracle, prime-only termination, ringAuto + manual geometry, ringPadUs), **pinExpander** + prefill/data-word split + needsPrefill, **memcpy snapshot with fused correction** (ON), **prime fork-join**, **doubleBuffer**, **multicore split** + renderWait, **dead-frame give-up + retry**, **frame-derived waitBudget**, **both loopback modes**, **shiftOverclock**, **driverHeapBytes accounting**, **GPIO drive-strength CAP_3** (hpwit-verified need), **MultiPinLedDriver as reference/classic-ESP32 path**, **useRing switch**, **PreviewDriver resumableFrames ON + keptIdx cache**, all new platform seams.

### 3. Follow-up after merge (ordered by payoff)
1. **Measure-then-delete the snapshot half of the fork-join** (A7/B): with the memcpy snapshot, time `ts` helper-off at 48×256; if sub-ms, delete `snapCopySrc_/snapCopyCh_/snapHelperLo_/Hi_`, `copyHelperRange`, `snapLineAlignedHalf` (+`<numeric>`), keeping the helper task for primeHalf only. Net-negative diff on the hairiest file.
2. **Retire the `ringSnapshot` OFF leg** (control → always-on mechanism): the A/B answered its question; OFF is a live UAF-hazard toggle in the UI.
3. **Retire PreviewDriver's synchronous send as a *control*** once resumable has soaked; keep it only as the automatic alloc-failure degrade.
4. **Fold `loopbackTest`+`loopbackIntrusive` into the promised `loopbackMode` dropdown** (the code comment already commits to it).
5. **Trim `ringDbg`** to lt/enc/ea/gap once the lapping work stabilizes; delete the closed-bug instrument fields (ipb/ci/tn, ld/dn) and the `descErr` B1-discriminator if it stays 0 through the soak.
6. **Lift the fork-join worker into a core/platform primitive (A5)** and re-base Drivers + MoonLedDriver on it; backlog it with the core fix named, per the interim rule.
7. **Settle the S3/P4 A/B** (MoonLed vs MultiPin): the retirement criterion is already written in MoonLedDriver's doc; when the challenger wins, retire the esp_lcd *shift-mode* leg (`clockMultiplier` through i80Ws2812Init) first — it is the one capability the ring strictly supersedes — and only then consider the whole reference on LCD_CAM chips (classic ESP32 keeps MultiPin regardless).
8. **Per-pin ≤16-bit active mask** to lift kMaxStrands=64 — already correctly deferred in the kMaxStrands doc; leave backlogged until a board needs >56 strands.

Key files: `src/light/drivers/ParallelLedDriver.h`, `src/light/drivers/MoonLedDriver.h`, `src/light/drivers/Drivers.h`, `src/platform/esp32/platform_esp32_moon_i80.cpp`, `src/platform/platform.h`, `src/platform/esp32/platform_esp32_worker.cpp`.
