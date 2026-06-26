# Backlog — mixed (core + light)

Forward-looking items whose work genuinely spans **both** the core and light domains — a core mechanism interacting with a light driver/effect/modifier, where assigning it to one side would misrepresent it. Core-only items are in [backlog-core.md](backlog-core.md), light-only in [backlog-light.md](backlog-light.md). Index + overview: [README.md](README.md).

## Cross-domain

### MultiplyModifier mapping-LUT memory at large grids (investigation, re-verify on classic)

`scenario_perf_full` on the S3 (2026-06-17) measured the MultiplyModifier's cost across grid sizes. The finding, stated correctly: the modifier **reduces compute** (with the default 2×2 kaleidoscope the effect renders only the ¼-size logical quadrant — Noise+Multiply at 16K is 29,647µs vs 50,555µs for Noise alone), and its real cost is **memory** — the mapping LUT's destinations array. Measured modifier heap cost on the S3: 16²→1.7KB, 32²→10.8KB, 64²→23.5KB, **128²(16K)→93KB** (`nrOfLightsType` is `uint32_t` on a PSRAM board). On the S3's 8MB PSRAM this is trivial. Under the physical→logical fold build each physical light contributes ≤1 destination, so the destinations array is bounded by the real light count regardless of chain depth — there is no build-time fan-out.

**This is NOT a no-PSRAM blocker** — 16K Noise + Multiply has run on a classic ESP32 (no PSRAM, 320KB internal) before at **10–20 FPS** (WiFi vs Ethernet), sending frames out over **ArtNet to a display, not physical LED drivers**. It works there because classic's `nrOfLightsType` is `uint16_t` (half the LUT size) and the modifier shrinks the logical render grid. So the action is **re-verify the working classic setup when a classic board is connected** (find the config — grid, mirror, ArtNet target — that reproduces the historical 10–20 FPS), not "fix an impossibility." Worth investigating only if that re-verification shows the LUT memory has regressed since: the destinations array is the obvious lever (it stores a `nrOfLightsType` per physical destination; a 2× kaleidoscope is 1:1 in *count* so the LUT need not store fan-out > the physical count — confirm it isn't over-allocating to `maxMultiplier()` when the effective fan-out is 1). Capture the classic numbers into performance.md's multi-board table first.

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
