# Backlog — mixed (core + light)

Forward-looking items whose work genuinely spans **both** the core and light domains — a core mechanism interacting with a light driver/effect/modifier, where assigning it to one side would misrepresent it. Core-only items are in [backlog-core.md](backlog-core.md), light-only in [backlog-light.md](backlog-light.md). Index + overview: [README.md](README.md).

## Cross-domain

### MultiplyModifier mapping-LUT memory at large grids (investigation, re-verify on classic)

`scenario_perf_full` on the S3 (2026-06-17) measured the MultiplyModifier's cost across grid sizes. The finding, stated correctly: the modifier **reduces compute** (with the default 2×2 kaleidoscope the effect renders only the ¼-size logical quadrant — Noise+Multiply at 16K is 29,647µs vs 50,555µs for Noise alone), and its real cost is **memory** — the 1:N fan-out mapping LUT. Measured modifier heap cost on the S3: 16²→1.7KB, 32²→10.8KB, 64²→23.5KB, **128²(16K)→93KB** (the LUT destinations array; `nrOfLightsType` is `uint32_t` on a PSRAM board). On the S3's 8MB PSRAM this is trivial. [Composed modifiers](#composed-modifiers--chain-the-whole-modifier-stack-not-just-the-first-planned-multi-commit) would multiply this memory cost by the chain depth — size it there.

**This is NOT a no-PSRAM blocker** — 16K Noise + Multiply has run on a classic ESP32 (no PSRAM, 320KB internal) before at **10–20 FPS** (WiFi vs Ethernet), sending frames out over **ArtNet to a display, not physical LED drivers**. It works there because classic's `nrOfLightsType` is `uint16_t` (half the LUT size) and the modifier shrinks the logical render grid. So the action is **re-verify the working classic setup when a classic board is connected** (find the config — grid, mirror, ArtNet target — that reproduces the historical 10–20 FPS), not "fix an impossibility." Worth investigating only if that re-verification shows the LUT memory has regressed since: the destinations array is the obvious lever (it stores a `nrOfLightsType` per physical destination; a 2× kaleidoscope is 1:1 in *count* so the LUT need not store fan-out > the physical count — confirm it isn't over-allocating to `maxMultiplier()` when the effective fan-out is 1). Capture the classic numbers into performance.md's multi-board table first.

### Composed modifiers — chain the whole modifier stack, not just the first (planned, multi-commit)

**Confirmed scope, not an open question:** multiple modifiers per Layer applied as a stack was always the plan, and it ships in **MoonLight** (Mirror, Rotate, Transpose, Kaleidoscope, … all composable on one layer — see [moonlight-inventory.md](../history/moonlight-inventory.md)). projectMM's single-modifier behaviour is the not-yet-finished state, not a design choice.

Today a Layer applies **only the first enabled modifier**. `Layer::rebuildLUT()` finds the first enabled `Modifier` child and `break`s ([Layer.h](../../src/light/layers/Layer.h) `rebuildLUT`), and `Layer::loop()` ticks only that one (with an explicit comment that ticking a later one would desync the LUT, since a dynamic modifier's `loop()` can drive a rebuild the LUT must reflect). So with two modifiers on a Layer the second is dead weight — dragging it above the first is the only way to make it the active one. The intended behaviour is **modifier order = apply order**: a stack where each modifier reshapes the result of the one below ("modifiers on modifiers"), e.g. Multiply (kaleidoscope) *then* Rotate the kaleidoscoped result. The [modifier-chain-viz UI item](backlog-core.md#open-design-questions) is the surface for it and only becomes meaningful once this lands.

**Mechanism — follow MoonLight's proven model, our own code** ([*Industry standards, our own code*](../../CLAUDE.md#principles)). MoonLight composes by streaming the layout's coordinates through each modifier's `modifyLayout`/`modifyLight` in order while the mapping table is built, so the *final* table already encodes the whole chain — the per-frame hot path stays a single lookup. We do the same with our pieces: `rebuildLUT()` walks the layout's coordinate stream (`Layouts::forEachCoord`) and passes each coordinate through modifier 1, then 2, …, then *n* before recording the destination, so the built `MappingLUT` is the composition `M₁ ∘ M₂ ∘ … ∘ Mₙ` collapsed to one `logical→driver` table. Composition is a **cold-path, build-time** concern; modifiers stay simple (each still answers `logicalDimensions()` + its own per-coordinate transform), so the complexity lives in the core per *[Complexity lives in core](../../CLAUDE.md#principles)*. Worth studying MoonLight's `PhysMap` 1:0/1:1/1:N packing (inventory §1) when sizing the table — a deep chain with fan-out is exactly where the per-entry byte cost matters.

Why it's not a one-liner:

- **Build path** — `rebuildLUT()` must iterate *all* enabled modifiers bottom-up, threading each stage's logical dimensions into the next, and fold the per-stage transforms into one final LUT. The single-modifier `maxDest` / fan-out ceiling math (the `maxMultiplier()` clamp that fixed the multiplyZ overflow) has to generalise to a **product** of multipliers across the chain — the dominant new correctness risk (and the memory blow-up noted in the MultiplyModifier-LUT item above: a 2-deep 2× chain is up to 4× the destinations).
- **Tick path** — a dynamic modifier (RandomMapModifier, RotateModifier) calls back into `Layer::onBuildState()` on its timer to rebuild the LUT. With a chain, *any* dynamic stage rebuilding must recompose the *whole* chain, and `loop()` must tick every enabled modifier (not `break` after the first) in the right order, after the effect pass.
- **Degrade path** — the per-stage OOM degrade (`degradeIdentity`) must decide what "degrade" means mid-chain (drop the offending stage? collapse to identity?) without leaving a stale partial LUT.
- **Tests** — `unit_Layers_container` / the modifier unit tests pin single-modifier behaviour; composed-order needs new cases (A∘B ≠ B∘A, a disabled middle stage is skipped not collapsed, the fan-out product ceiling holds at no-PSRAM `uint16_t`), plus a scenario that reorders a 2-modifier stack and asserts the composite changes.

**Estimate: medium — roughly 4–6 commits.** (1) design note pinning the coordinate-stream composition model + the fan-out-product ceiling rule (reference the MoonLight inventory); (2) `MappingLUT` compose/fold primitive + unit tests in isolation; (3) `rebuildLUT()` chain iteration + `loop()` tick-all-in-order, behind the existing single-modifier tests staying green; (4) degrade-path decision + tests; (5) reorder scenario + `performance.md` memory capture at depth 2–3; (6) UI follow-up (the modifier-chain-viz item — see the correction noted there). Gate the depth: most setups are 1 modifier, so the chain path must cost nothing when `n == 1` (the current fast path stays the `n == 1` branch).

### Intermittent ~0.5 s LED pauses with the RMT driver (pending investigation)

Observed on the bench (2026-06): LED output running on the RMT driver occasionally freezes for about half a second. Postponed by the product owner until more observations exist. Ranked suspects from the initial analysis, each with a cheap experiment:

1. **WiFi modem power-save never disabled** — nothing in `src/` calls `esp_wifi_set_ps(WIFI_PS_NONE)`, so the IDF default `WIFI_PS_MIN_MODEM` is active; the radio's DTIM sleep causes exactly this class of intermittent multi-hundred-ms stall. WLED and the v1/v2 lineage disable sleep. Experiment: one line in the ESP32 platform code after association.
2. **NetworkSendDriver sending synchronously every tick to an absent destination** (default `192.168.1.70`) — lwIP keeps re-ARPing a dead address while the send sits in the render tick. Data point (2026-06-10): the bench esp32-16mb had NetworkSend *disabled* in its persisted config, consistent with the pauses being annoying enough to switch the sender off. Experiment: point the ArtNet IP at a live host (or disable the driver) and see if the pauses stop.
3. **`rmt_tx_wait_all_done` 1 s timeout** — a wedged transmission blocks the tick up to a full second (multi-pin: up to N×1 s). Least likely (~1 s, not ~0.5 s) but it's the only hard block in the driver itself.

If pauses correlate with UI control changes, also consider the 2 s-debounced SPIFFS save stalling flash-resident code. The per-tick KPI log around a pause discriminates between these immediately.

### NoiseEffect simplex cost on ESP32 (investigation)

With mirror XY at 128×128, NoiseEffect renders the 64×64 logical quadrant in **~11 ms/tick** on the Olimex (measured) — the simplex math dominates, since the Xtensa LX6 has no FPU and float math is software-emulated. (RainbowEffect on the same pipeline is much cheaper.) This is correct, non-degraded behaviour; it's only worth revisiting if a deployment needs Noise faster than ~11 ms at this grid.

Worth investigating if so:

- **Q16 fixed-point simplex** instead of float (kills the software-float emulation cost).
- **Lower-precision hash** — current simplex uses a 256-entry permutation lookup; a smaller / SIMD-friendly hash may be faster on Xtensa.
- **Strided sampling + interpolation** — render at 32×32, bilinear up to 64×64. Visual quality cost; needs A/B comparison.
- **Inline / unroll the inner per-pixel loop** to keep the simplex state in registers.

None of these are obviously free, and a fixed-point port may shift the visual signature. Defer until there's a real use case — on the no-PSRAM Olimex at large grids the tick is dominated by the synchronous ArtNet send (~35 ms), not Noise, so the effect is rarely the bottleneck there.

**S3 render-only data point (2026-06-17, `scenario_perf_full`):** on the PSRAM S3 with **no output driver**, Noise is the dominant cost at every grid and there's no ArtNet floor to hide it: 16²→738µs, 32²→2,831µs, 64²→11,235µs, **128²(16K)→50,555µs (~20 FPS)** — clean ~linear-in-pixels (67×), so no fragmentation/realloc pathology, just raw simplex compute. The light effect (Checkerboard) on the same sweep is 6–11× faster (16K→7,949µs, ~128 FPS). So on a PSRAM board the heavy effect IS the 16K bottleneck (where on the Olimex the network send was). This is the strongest case for the fixed-point/strided-sampling ideas above, since a PSRAM board can run 16K grids that the network-bound Olimex never reaches. The S3 has a real FPU (LX7), so the win is less about software-float emulation and more about per-pixel simplex work; profile before committing.
