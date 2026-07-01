# Scenario Tests

Auto-generated from `test/scenarios/{core,light}/scenario_*.json` by `scripts/docs/generate_test_docs.py`. **Do not edit by hand** — update the JSON file's top-level fields and per-step `description` / `bounds` / `contract` / `observed` instead, then regenerate.

Scenario tests are the integration tier in the [test strategy](../testing.md): each one is a JSON script that drives the full pipeline (PC or live ESP32) and captures tick / heap per step against per-target contracts. Run them with `scripts/scenario/run_scenario.py` (PC) or `scripts/scenario/run_live_scenario.py` (live device). See [testing.md § Performance contracts](../testing.md#performance-contracts-contracttarget) for the contract semantics.

## AudioModule

### scenario_Audio_mutation

`test/scenarios/light/scenario_Audio_mutation.json` — Add / configure / remove the AudioModule peripheral and an audio-reactive effect while the render pipeline runs, proving the robustness rule for the audio producer/consumer pair. AudioModule is a Peripheral (it sits beside the pipeline, publishing an AudioFrame), and the audio effects read it through the static AudioModule::latestFrame() accessor, NOT a boot-time pointer — so add/remove can happen in any order at runtime. The checks assert the pipeline keeps RENDERING (buffer non-null, fps measurable) through each mutation: adding the mic, setting its three pins one at a time (the install fan-out's exact add-then-configure sequence — the web installer / MoonDeck / OTA picker add the AudioModule then POST wsPin/sdPin/sckPin as separate control writes from a catalog entry's modules+controls), adding a consumer effect, and crucially REMOVING the mic while a consumer is still live (the consumer must fall back to a silent frame, never deref a dangling pointer — the bug the boot-loop fix and the unit lifecycle tests pin, here proven end-to-end through the Scheduler). The per-pin sequence proves the self-correcting partial-fill: the first two writes leave a pin unset so AudioModule's guard makes the rebuild a no-op, the third completes the set — the pipeline never stalls through any of it. On the host the mic is inert (hasI2sMic false), so this exercises the wiring/lifecycle, not real capture; capture is proven on hardware. Grid is 64x64 so the tick stays above the host microsecond clock at every step.

**Mode**: `mutate` · **Also touches**: SystemModule, Layouts, GridLayout, Layer, RainbowEffect, AudioVolumeEffect, AudioSpectrumEffect, Drivers, PreviewDriver

#### `measure-pipeline-only` (measure)  📏

Baseline: the render pipeline runs with no audio module present.

**Bounds**:
- FPS ≥ 1 (absolute)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 15,625-125,000 | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-12 → 2026-07-01

#### `measure-audio-added` (measure)  📏

Pipeline still renders with the (idle, unconfigured) mic added.

**Setup** (preceding non-measured steps):
- `add-audio-module` (add_module) — Add the AudioModule peripheral under SystemModule (where the user adds it, beside the board). Pins default unset, so it stays idle; the pipeline must keep rendering.

**Bounds**:
- FPS ≥ 1 (absolute)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 15,873-125,000 | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-12 → 2026-07-01

#### `measure-pins-configured` (measure)  📏

All three mic pins set via the sequential install-fan-out order: pipeline still renders through the full add-then-configure flow a catalog inject performs (add AudioModule, then wsPin/sdPin/sckPin one at a time).

**Setup** (preceding non-measured steps):
- `configure-ws-pin` (set_control) — Set the first mic pin (wsPin). This mirrors the install fan-out: a catalog entry's Audio pins arrive as separate /api/control writes, one per pin. After this single write wsPin is set but sdPin/sckPin are still 0, so AudioModule's unset-pin guard short-circuits before audioMicInit (no I2S init attempted) and the rebuild is a cheap no-op. The pipeline must keep rendering through it.
- `configure-sd-pin` (set_control) — Second pin (sdPin). Still one pin unset (sckPin=0), so still guarded — the second cheap no-op rebuild. Proves the partial-pin-fill sequence the install injection produces never disturbs the running pipeline.
- `configure-sck-pin` (set_control) — Third and final pin (sckPin). Now all three are set, so the guard passes and a real mic init is attempted. On host hasI2sMic is false so the mic stays inert regardless, but this is the write that completes the install-injected pin set; the buildState rebuild must not disturb the running pipeline.

**Bounds**:
- FPS ≥ 1 (absolute)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 15,873-125,000 | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-13 → 2026-07-01

#### `measure-consumer-live` (measure)  📏

Pipeline renders with the producer + consumer both wired.

**Setup** (preceding non-measured steps):
- `add-audio-consumer` (add_module) — Add an AudioVolumeEffect consumer under the Layer. It reads the mic via the static accessor; with the mic present it gets the live (silent, on host) frame.

**Bounds**:
- FPS ≥ 1 (absolute)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 13,889-125,000 | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-12 → 2026-07-01

#### `measure-after-mic-removed` (measure)  📏

Mic gone, consumer remains: pipeline keeps rendering on silent audio (buffer non-null, fps measurable). No crash from the orphaned consumer.

**Setup** (preceding non-measured steps):
- `remove-audio-module` (remove_module) — Remove the mic while the consumer is STILL live. The consumer must fall back to AudioModule::latestFrame()'s static silence — no dangling pointer, no crash. This is the robustness rule's hardest case for this pair.

**Bounds**:
- FPS ≥ 1 (absolute)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 13,514-125,000 | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-12 → 2026-07-01

#### `measure-back-to-baseline` (measure)  📏

Both audio modules gone: back to the pipeline-only baseline, still rendering.

**Setup** (preceding non-measured steps):
- `remove-audio-consumer` (remove_module) — Remove the orphaned consumer too — clean teardown, pipeline still live.

**Bounds**:
- FPS ≥ 1 (absolute)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 15,873-125,000 | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-12 → 2026-07-01

## DevicesModule

### scenario_DevicesModule_scan

`test/scenarios/core/scenario_DevicesModule_scan.json` — Trigger the device-discovery sweep repeatedly on a running device and confirm the render loop survives every sweep. Pins the robustness principle for DevicesModule: pressing the `scan` button (a Button control on the Devices submodule of Network) re-runs the subnet sweep, whose HTTP probes block the render task up to the probe timeout per tick. No press, sweep state, or completion may crash or wedge the tick. Runs live only — discovery needs a real LAN to probe and the module only exists on a connected device, so the in-process desktop runner SKIPs it; on a device it presses the button over HTTP. The bound checks FPS stays within range across repeated sweeps (a sweep is a transient cost, not a permanent degradation), proving the scan never leaves the render loop in a wedged state. (Background: the sweep is boot-only + manual precisely because the blocking probe must not run continuously on the render task — see DevicesModule.md.)

**Mode**: `mutate` · **live-only** (skipped in-process)

#### `scan-1` (set_control)  📏

First manual sweep. Baseline: the device renders while a discovery sweep runs.

#### `scan-2` (set_control)  📏

Re-trigger the sweep while the previous one's state is still settling — confirms a re-press mid-cycle doesn't wedge the loop.

#### `scan-3` (set_control)  📏

Third sweep, bounded: FPS must stay within 20% of the first (a sweep is a transient cost; repeated scans must not permanently degrade the render loop).

**Bounds**:
- FPS ≥ 80% of baseline

## GridLayout

### scenario_GridLayout_resize

`test/scenarios/light/scenario_GridLayout_resize.json` — Resize the grid while the pipeline is running and verify it reallocates cleanly under memory pressure. Lowers to 128x64 (release memory), increases to 128x128 (heaviest config: mirror + LUT). Each measured step captures tick/FPS/heap so the runner reports the degrade behaviour.

**Mode**: `mutate` · **Also touches**: MultiplyModifier, Layer

#### `size-128x128` (set_control)  📏

Set grid height to 128 (alongside default width 128). Measures the heaviest config as the baseline for the next two steps.

**Setup** (preceding non-measured steps):
- `canvas-clear-layers` (clear_children) — Self-canvas: clear+rebuild the pipeline this scenario assumes, so it runs from any device state (the perf scenarios' pattern). Pre-wired apparatus (Preview/Board) survives clear_children. On the in-process runner the fixture already built the tree; clearing then rebuilding is harmless there and makes the live run order-independent.
- `canvas-clear-layouts` (clear_children)
- `canvas-clear-drivers` (clear_children)
- `canvas-grid` (add_module)
- `canvas-layer` (add_module)
- `canvas-noise` (add_module)
- `canvas-mirror` (add_module)
- `canvas-artnet` (add_module)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 4.5 | — / 83KB | — / 48KB |
| `esp32-eth` | — / 10.7-10.8 | — / 132KB | — / 48KB-52KB |
| `esp32-eth-wifi` | ≥ 10.0 / 12.4 | ≥ 103KB / 93KB | — / 48KB |
| `esp32p4-eth` | — / 739-880 | — / 33206KB-33218KB | — / 376KB |
| `esp32s3-n16r8` | — / 106-217 | — / 8315KB-8321KB | — / 104KB-108KB |
| `pc-macos` | ≥ 8,333 / 3,534-10,526 | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 3,413-4,566 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-22
- `esp32s3-n16r8`: observed 2026-06-22
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-03
- `pc-windows`: observed 2026-06-07

#### `shrink-to-128x64` (set_control)  📏

Shrink to 128x64. Measured: tick/heap captured so the runner reports the realloc behaviour against each target's contract. (The old relative-to-baseline FPS bound was removed — it compared against the runner's idle pre-scenario baseline, not the prior render step, so it false-failed on fast boards like the P4 where idle FPS dwarfs render FPS.)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 11.1 | — / 63KB | — / 17KB |
| `esp32-eth` | — / 26.4-26.5 | — / 114KB | — / 48KB |
| `esp32-eth-wifi` | ≥ 22.2 / 31.8 | ≥ 83KB / 75KB | — / 24KB |
| `esp32p4-eth` | — / 1,527-1,739 | — / 33214KB-33226KB | — / 376KB |
| `esp32s3-n16r8` | — / 415-787 | — / 8324KB-8331KB | — / 100KB-112KB |
| `pc-macos` | ≥ 16,667 / 4,695-21,739 | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 7,299-10,638 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-22
- `esp32s3-n16r8`: observed 2026-06-22
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-08
- `pc-windows`: observed 2026-06-07

#### `grow-to-128x128` (set_control)  📏

Grow back to 128x128. Measured: confirms the heap can return to the heavy baseline after a shrink.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 4.0 | — / 83KB | — / 52KB |
| `esp32-eth` | — / 10.4 | — / 132KB | — / 48KB |
| `esp32-eth-wifi` | ≥ 10.0 / 12.2 | ≥ 103KB / 93KB | — / 52KB |
| `esp32p4-eth` | — / 762-875 | — / 33206KB-33218KB | — / 376KB |
| `esp32s3-n16r8` | — / 132-251 | — / 8312KB-8322KB | — / 100KB-112KB |
| `pc-macos` | ≥ 8,333 / 3,257-10,204 | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 3,436-4,608 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-22
- `esp32s3-n16r8`: observed 2026-06-22
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-03
- `pc-windows`: observed 2026-06-07

## Layer

### scenario_Layer_base_pipeline

`test/scenarios/light/scenario_Layer_base_pipeline.json` — Core pipeline: build Layouts→Grid→Layer→RainbowEffect→Drivers→NetworkSendDriver from scratch and verify each module wires correctly. Drives the bounded FPS check at the end so a render-path regression is caught.

**Mode**: `construct` · **Also touches**: GridLayout, RainbowEffect, Drivers, NetworkSendDriver

#### `add-artnet` (add_module)  📏

Add NetworkSendDriver and run the bounded FPS measurement (expected to stay at >=80% of the rated FPS for the 128x128 grid this scenario builds; min_pct needs a live baseline, so it gates only on hardware and is skipped with a WARN in the desktop runner).

**Setup** (preceding non-measured steps):
- `add-layout-group` (add_module) — Create the top-level Layouts container.
- `add-grid` (add_module) — Add a 128x128 GridLayout child to Layouts. Set explicitly (the module default is 16x16x1) so the tick is above the host's microsecond clock resolution — a 16x16 grid renders in <1us on desktop, flooring tick to 0.
- `add-layer` (add_module) — Add a top-level Layer wired to the Layouts container, RGB (3 channels per light).
- `add-rainbow` (add_module) — Add RainbowEffect as the Layer's only effect.
- `add-driver-group` (add_module) — Add a top-level Drivers container wired to the Layer's output buffer.

**Bounds**:
- FPS ≥ 80% of baseline
- FPS × lights ≥ 294,912

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | ≥ 20,000 / 4,608-— | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 7,874-8,475 | — / unlimited | — / unlimited |

- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-30
- `pc-windows`: observed 2026-06-07

### scenario_Layer_memory_1to1

`test/scenarios/light/scenario_Layer_memory_1to1.json` — Verify that an unshuffled 1:1 mapping (no modifier) uses no LUT and no driver buffer. Catches a regression where Layer would allocate a passthrough LUT for the identity case.

**Mode**: `construct` · **Also touches**: MappingLUT, BlendMap

#### `add-artnet` (add_module)  📏

Add NetworkSendDriver and run the bounded FPS measurement on the no-LUT path.

**Setup** (preceding non-measured steps):
- `add-layout-group` (add_module) — Create the top-level Layouts container.
- `add-grid` (add_module) — Add a 16x16 GridLayout.
- `add-layer` (add_module) — Add a Layer wired to Layouts (RGB).
- `add-rainbow` (add_module) — Add RainbowEffect as the Layer's effect.
- `add-driver-group` (add_module) — Add a Drivers container wired to the Layer.

**Bounds**:
- FPS ≥ 80% of baseline

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | ≥ 20,000 / 12,500-— | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 500,000-1,000,000 | — / unlimited | — / unlimited |

- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-05
- `pc-windows`: observed 2026-06-07

### scenario_modifier_chain

`test/scenarios/light/scenario_modifier_chain.json` — Stack TWO modifiers on one Layer (Region then Multiply) and verify the chain composes live end-to-end — the capability the old single-modifier engine couldn't do. Prepares its own canvas: Layout(Grid 32x32) + Layer + NoiseEffect + Region(0..50) + Multiply(2x), measures the composite, then adds a third (Checkerboard mask) and measures again, then removes the middle modifier and measures — exercising add/remove on a multi-modifier chain. A broken fold (null buffer, wrong light count, crash on a disabled/removed stage) shows up as a failed measure. The fold composition + order semantics are pinned by unit_Layer_modifier_chain; this is the live end-to-end gate.

**Mode**: `mutate` · **Also touches**: RegionModifier, MultiplyModifier, CheckerboardModifier, RotateModifier, NoiseEffect, Layouts, GridLayout, Drivers, NetworkSendDriver

#### `add-mask` (add_module)  📏

Add a third modifier (Checkerboard mask) on top of the chain — a 3-deep fold. Measure that the deeper chain still renders.

**Setup** (preceding non-measured steps):
- `region-then-multiply` (measure) — Two stacked modifiers: Region(top-left quarter) then Multiply(2x mirror) compose into one mapping. Measure the live composite.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 18,182-200,000 | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-26 → 2026-07-01

#### `remove-middle` (remove_module)  📏

Remove the middle modifier (Multiply) — the chain re-folds with Region then Checkerboard, no stale state. Measure.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 6,536-55,556 | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-26 → 2026-07-01

#### `add-live-rotate` (add_module)  📏

Add a DYNAMIC Rotate on top of the static chain — its modifyLive runs the per-frame remap pass over the composed buffer. Verifies a static chain + a live modifier coexist (the buffer is remapped each frame on top of the baked Region/Checkerboard mapping) without a crash or null buffer.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 5,128-31,250 | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-26 → 2026-07-01

### scenario_modifier_swap

`test/scenarios/light/scenario_modifier_swap.json` — Swap the Layer's modifier between Multiply and Checkerboard and verify the pipeline stays live across each replace. Prepares its own canvas (clear + rebuild) so it runs from any device state: one Layout(Grid 32x32) + one Layer + one effect + one modifier, then replace_module cycles the modifier MOD slot Multiply -> Checkerboard -> Multiply, measuring after each so a broken swap (null buffer / wrong light count) shows up. Exercises the modifier-replace path the UI's drag-replace uses.

**Mode**: `mutate` · **Also touches**: MultiplyModifier, CheckerboardModifier, NoiseEffect, Layouts, GridLayout, Drivers, NetworkSendDriver, PreviewDriver

#### `multiply-1` (measure)  📏

Multiply modifier active — pipeline live, LUT folds the grid.

**Setup** (preceding non-measured steps):
- `shrink-w` (set_control)
- `shrink-h` (set_control)
- `clear-layers` (clear_children)
- `clear-layouts` (clear_children)
- `build-grid` (add_module)
- `build-layer` (add_module)
- `build-fx` (add_module)
- `build-mod` (add_module)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 1,783-2,179 | — / 145KB | — / 108KB |
| `esp32-eth` | — / 1,580-7,752 | — / 172KB-225KB | — / 76KB-108KB |
| `esp32p4-eth` | — / 5,587-6,061 | — / 33243KB | — / 376KB |
| `esp32s3-n16r8` | — / 1,773-2,571 | — / 8350KB | — / 92KB |
| `pc-macos` | — / 25,000-166,667 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-25
- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `esp32p4-eth`: observed 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-25
- `pc-macos`: observed 2026-06-07 → 2026-06-30

#### `checkerboard` (measure)  📏

Checkerboard modifier active — masks half the lights; pipeline stays live (driver buffer non-null).

**Setup** (preceding non-measured steps):
- `swap-to-checker` (replace_module)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 892-922 | — / 145KB | — / 108KB |
| `esp32-eth` | — / 769-990 | — / 170KB-225KB | — / 76KB-108KB |
| `esp32p4-eth` | — / 2,747-2,762 | — / 33242KB | — / 376KB |
| `esp32s3-n16r8` | — / 924-943 | — / 8349KB | — / 92KB |
| `pc-macos` | — / 4,184-58,824 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-25
- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `esp32p4-eth`: observed 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-25
- `pc-macos`: observed 2026-06-07 → 2026-07-01

#### `multiply-2` (measure)  📏

Back to Multiply — replace round-trips cleanly, pipeline live again.

**Setup** (preceding non-measured steps):
- `swap-to-multiply` (replace_module)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 2,079-2,208 | — / 145KB | — / 108KB |
| `esp32-eth` | — / 1,587-2,278 | — / 169KB-225KB | — / 76KB-108KB |
| `esp32p4-eth` | — / 6,329-6,410 | — / 33243KB | — / 376KB |
| `esp32s3-n16r8` | — / 2,146-2,604 | — / 8349KB-8350KB | — / 92KB |
| `pc-macos` | — / 10,101-166,667 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-25
- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `esp32p4-eth`: observed 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-25
- `pc-macos`: observed 2026-06-07 → 2026-07-01

### scenario_perf_full

`test/scenarios/light/scenario_perf_full.json` — Comprehensive incremental performance check (the SLOW, on-device companion to scenario_perf_light). Mutate mode + canvas-preparing: clear_children whatever the device already had (pre-wired apparatus like PreviewDriver/Board survives — clear_children only drops user-editable children), rebuild a known minimal tree, then add one subsystem at a time — audio, device discovery, a modifier, then EVERY output driver this board has (each optional + capped to 64 output LEDs so its per-frame cost is comparable, not its transmit-all-16K time), then a network driver — measuring the tick/heap delta after each so each subsystem's cost is isolated. Then sweep the grid 16²→32²→64²→128² (16K) for both a LIGHT effect (Spiral) and a HEAVY one (Noise) to bracket the compute range across sizes. LED drivers are platform-gated (RMT on classic/S3, LCD on S3, Parlio on P4; none on desktop) so each driver step is optional:true and skipped where absent — the all-drivers comparison is assembled across boards (S3 gives RMT vs LCD, P4 gives RMT vs Parlio). Subsumes the old scenario_Layer_buildup (incremental module cost), scenario_GridLayout_grid_sizes (grid sweep), and scenario_AllEffects_grid_sizes (per-effect size sweep, here reduced to a light/heavy bracket). Runs minutes on a device; not a per-commit gate.

**Mode**: `mutate` · **Also touches**: Layouts, GridLayout, Drivers, PreviewDriver, NetworkSendDriver, RmtLedDriver, LcdLedDriver, ParlioLedDriver, MultiplyModifier, SpiralEffect, NoiseEffect

#### `measure-minimal` (measure)  📏

Bare minimum at 16²: Grid + Layer + Spiral, no output driver, audio/discovery still on as the device ships. The floor for the subsystem-cost diffs below.

**Setup** (preceding non-measured steps):
- `clear-layers` (clear_children) — Start clean: drop whatever effects/modifiers/layouts/drivers the device had (pre-wired Preview survives).
- `clear-layouts` (clear_children)
- `clear-drivers` (clear_children)
- `build-grid` (add_module)
- `build-layer` (add_module)
- `build-fx` (add_module)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 7,692-8,929 | — / 134KB-147KB | — / 108KB |
| `esp32p4-eth` | — / 14,925-17,544 | — / 33226KB-33245KB | — / 376KB |
| `esp32s3-n16r8` | — / 5,376-9,009 | — / 8340KB-8352KB | — / 92KB-112KB |
| `pc-macos` | — / 142,857-— | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-26
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-25
- `pc-macos`: observed 2026-06-17 → 2026-07-01

#### `measure-no-audio` (measure)  📏

**Setup** (preceding non-measured steps):
- `disable-audio` (set_control) — Disable AudioModule (stops I2S sampling in loop()). Diff vs measure-minimal = the audio subsystem's per-tick cost (device only; optional).

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 8,621-9,901 | — / 134KB-147KB | — / 108KB |
| `esp32p4-eth` | — / 18,182-18,868 | — / 33228KB-33245KB | — / 376KB |
| `esp32s3-n16r8` | — / 8,065-9,901 | — / 8338KB-8352KB | — / 92KB-112KB |
| `pc-macos` | — / 142,857-— | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-26
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-25
- `pc-macos`: observed 2026-06-17 → 2026-07-01

#### `measure-quiet` (measure)  📏

Quiet baseline: render-only, audio + discovery off. The cleanest render floor; the per-driver costs below diff against this.

**Setup** (preceding non-measured steps):
- `disable-devices` (set_control) — Disable the Devices module (stops the blocking HTTP discovery sweep in loop1s()). Diff = the discovery cost (device only; optional).

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 7,246-9,901 | — / 131KB-146KB | — / 108KB |
| `esp32p4-eth` | — / 17,544-18,519 | — / 33226KB-33245KB | — / 376KB |
| `esp32s3-n16r8` | — / 7,752-9,901 | — / 8337KB-8352KB | — / 92KB-112KB |
| `pc-macos` | — / 142,857-— | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-26
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-25
- `pc-macos`: observed 2026-06-17 → 2026-07-01

#### `measure-modifier` (measure)  📏

**Setup** (preceding non-measured steps):
- `add-modifier` (add_module) — +MultiplyModifier: allocates the mapping LUT. Diff = modifier + LUT cost.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 2,786-3,610 | — / 130KB-145KB | — / 108KB |
| `esp32p4-eth` | — / 8,772-10,638 | — / 33224KB-33243KB | — / 376KB |
| `esp32s3-n16r8` | — / 3,413-4,237 | — / 8336KB-8350KB | — / 92KB-112KB |
| `pc-macos` | — / 333,333-— | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-26
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-25
- `pc-macos`: observed 2026-06-17 → 2026-07-01

#### `measure-preview` (measure)  📏

**Setup** (preceding non-measured steps):
- `remove-modifier` (remove_module) — Drop the modifier so the driver-cost measurements below are on the plain 1:1 pipeline (drivers, not the LUT, are what we compare here).
- `add-preview` (add_module) — +PreviewDriver. Optional: on a device the pre-wired Preview survives clear_children so it's already present (the add is skipped); on the in-process desktop runner there's no apparatus, so this adds it. Either way the next measure includes Preview.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 8,696-9,524 | — / 123KB-147KB | — / 108KB |
| `esp32p4-eth` | — / 15,873-18,182 | — / 33228KB-33245KB | — / 376KB |
| `esp32s3-n16r8` | — / 8,065-9,434 | — / 8335KB-8352KB | — / 92KB-112KB |
| `pc-macos` | — / 142,857-— | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-25
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-25
- `pc-macos`: observed 2026-06-17 → 2026-07-01

#### `measure-network` (measure)  📏

**Setup** (preceding non-measured steps):
- `add-network-driver` (add_module) — +NetworkSendDriver (ArtNet/DDP — works on every platform). Diff = the network output cost (capped by the 16² grid here).

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 6,098-7,194 | — / 131KB-145KB | — / 108KB |
| `esp32p4-eth` | — / 14,493-17,544 | — / 33226KB-33244KB | — / 376KB |
| `esp32s3-n16r8` | — / 6,452-8,065 | — / 8334KB-8351KB | — / 84KB-112KB |
| `pc-macos` | — / 142,857-— | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-26
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-26
- `pc-macos`: observed 2026-06-17 → 2026-07-01

#### `measure-rmt` (measure)  📏

**Setup** (preceding non-measured steps):
- `remove-network-driver` (remove_module)
- `add-rmt-driver` (add_module) — +RmtLedDriver capped to 64 output LEDs (one pin, ledsPerPin=64). Optional — classic + S3. Diff = the RMT per-frame cost at a fixed 64-LED output.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 6,579-9,174 | — / 106KB-122KB | — / 84KB-108KB |
| `esp32p4-eth` | — / 15,873-17,857 | — / 33200KB-33221KB | — / 376KB |
| `esp32s3-n16r8` | — / 7,194-9,346 | — / 8307KB-8328KB | — / 84KB-112KB |
| `pc-macos` | — / 111,111-— | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-25
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-26
- `pc-macos`: observed 2026-06-17 → 2026-07-01

#### `measure-lcd` (measure)  📏

**Setup** (preceding non-measured steps):
- `remove-rmt-driver` (remove_module)
- `add-lcd-driver` (add_module) — +LcdLedDriver capped to 64 LEDs on lane 0 (i80 needs all 8 data pins; unused lanes get 0 LEDs). Optional — S3 only. Diff = the LCD_CAM per-frame cost.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 8,403-9,901 | — / 126KB-147KB | — / 108KB |
| `esp32p4-eth` | — / 15,873-17,857 | — / 33225KB-33245KB | — / 376KB |
| `esp32s3-n16r8` | — / 7,042-9,259 | — / 8333KB-8352KB | — / 88KB-112KB |
| `pc-macos` | — / 83,333-— | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-25
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-25
- `pc-macos`: observed 2026-06-17 → 2026-07-01

#### `measure-parlio` (measure)  📏

**Setup** (preceding non-measured steps):
- `remove-lcd-driver` (remove_module)
- `add-parlio-driver` (add_module) — +ParlioLedDriver capped to 64 LEDs on lane 0. Optional — P4 only. Diff = the Parlio per-frame cost.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 8,475-9,901 | — / 135KB-147KB | — / 108KB |
| `esp32p4-eth` | — / 15,873-17,857 | — / 33225KB-33245KB | — / 376KB |
| `esp32s3-n16r8` | — / 7,692-9,434 | — / 8338KB-8352KB | — / 92KB-112KB |
| `pc-macos` | — / 111,111-— | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-25
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-25
- `pc-macos`: observed 2026-06-17 → 2026-07-01

#### `measure-light-16` (measure)  📏

**Setup** (preceding non-measured steps):
- `remove-parlio-driver` (remove_module)
- `add-preview-for-sweep` (add_module) — Re-add PreviewDriver as the output for the grid sweep (the per-driver adds above each removed their driver; Preview is the cheap, every-board output for a pure-render size curve).
- `light-16-w` (set_control) — Grid sweep, LIGHT effect (Spiral is already FX).
- `light-16-h` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 6,711-9,804 | — / 134KB-147KB | — / 108KB |
| `esp32p4-eth` | — / 15,385-18,868 | — / 33226KB-33245KB | — / 376KB |
| `esp32s3-n16r8` | — / 8,403-9,901 | — / 8336KB-8352KB | — / 92KB-112KB |
| `pc-macos` | — / 111,111-— | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-25
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-25
- `pc-macos`: observed 2026-06-17 → 2026-07-01

#### `measure-light-32` (measure)  📏

**Setup** (preceding non-measured steps):
- `light-32-w` (set_control)
- `light-32-h` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 2,801-3,367 | — / 134KB-144KB | — / 108KB |
| `esp32p4-eth` | — / 7,246-7,576 | — / 33225KB-33243KB | — / 376KB |
| `esp32s3-n16r8` | — / 3,049-3,597 | — / 8331KB-8350KB | — / 92KB-112KB |
| `pc-macos` | — / 35,714-1,000,000 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-26
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-25
- `pc-macos`: observed 2026-06-17 → 2026-07-01

#### `measure-light-64` (measure)  📏

**Setup** (preceding non-measured steps):
- `light-64-w` (set_control)
- `light-64-h` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 870-928 | — / 125KB-135KB | — / 108KB |
| `esp32p4-eth` | — / 2,008-2,232 | — / 33218KB-33234KB | — / 376KB |
| `esp32s3-n16r8` | — / 894-1,011 | — / 8312KB-8341KB | — / 88KB-112KB |
| `pc-macos` | — / 9,091-333,333 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-26
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-25
- `pc-macos`: observed 2026-06-17 → 2026-07-01

#### `measure-light-128` (measure)  📏

**Setup** (preceding non-measured steps):
- `light-128-w` (set_control)
- `light-128-h` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 224-238 | — / 89KB-99KB | — / 62KB |
| `esp32p4-eth` | — / 515-573 | — / 33182KB-33198KB | — / 376KB |
| `esp32s3-n16r8` | — / 114-134 | — / 8291KB-8305KB | — / 92KB-112KB |
| `pc-macos` | — / 2,165-62,500 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-25
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-25
- `pc-macos`: observed 2026-06-17 → 2026-07-01

#### `measure-heavy-16` (measure)  📏

**Setup** (preceding non-measured steps):
- `swap-heavy` (replace_module) — Swap to the HEAVY effect (Noise) and repeat the sweep — the upper bracket of per-pixel compute.
- `heavy-16-w` (set_control)
- `heavy-16-h` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 990-1,224 | — / 136KB-147KB | — / 108KB |
| `esp32p4-eth` | — / 2,865-3,367 | — / 33229KB-33245KB | — / 376KB |
| `esp32s3-n16r8` | — / 1,100-1,361 | — / 8342KB-8352KB | — / 92KB-112KB |
| `pc-macos` | — / 47,619-333,333 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-25
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-26
- `pc-macos`: observed 2026-06-17 → 2026-06-27

#### `measure-heavy-32` (measure)  📏

**Setup** (preceding non-measured steps):
- `heavy-32-w` (set_control)
- `heavy-32-h` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 306-314 | — / 134KB-144KB | — / 108KB |
| `esp32p4-eth` | — / 799-898 | — / 33227KB-33243KB | — / 376KB |
| `esp32s3-n16r8` | — / 290-356 | — / 8339KB-8350KB | — / 92KB-112KB |
| `pc-macos` | — / 11,765-71,429 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-26
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-25
- `pc-macos`: observed 2026-06-17 → 2026-06-27

#### `measure-heavy-64` (measure)  📏

**Setup** (preceding non-measured steps):
- `heavy-64-w` (set_control)
- `heavy-64-h` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 73.8-79.4 | — / 125KB-135KB | — / 108KB |
| `esp32p4-eth` | — / 196-229 | — / 33218KB-33234KB | — / 376KB |
| `esp32s3-n16r8` | — / 85.2-90.3 | — / 8330KB-8341KB | — / 92KB-112KB |
| `pc-macos` | — / 2,119-16,129 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-26
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-25
- `pc-macos`: observed 2026-06-17 → 2026-06-27

#### `measure-heavy-128` (measure)  📏

**Setup** (preceding non-measured steps):
- `heavy-128-w` (set_control)
- `heavy-128-h` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 16.0-19.0 | — / 89KB-99KB | — / 62KB |
| `esp32p4-eth` | — / 53.7-57.4 | — / 33182KB-33198KB | — / 376KB |
| `esp32s3-n16r8` | — / 19.2-20.8 | — / 8293KB-8305KB | — / 92KB-112KB |
| `pc-macos` | — / 584-3,676 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-25
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-25
- `pc-macos`: observed 2026-06-17 → 2026-07-01

#### `measure-mod-16` (measure)  📏

**Setup** (preceding non-measured steps):
- `add-modifier-for-sweep` (add_module) — Re-add the MultiplyModifier on the HEAVY effect and sweep grid sizes — the diff vs the matching measure-heavy-N step (no modifier) is the modifier's PER-FRAME cost at each grid, answering whether the ~180µs seen at 16² scales with pixel count or is fixed overhead.
- `mod-16-w` (set_control)
- `mod-16-h` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 2,020-2,222 | — / 135KB-145KB | — / 108KB |
| `esp32p4-eth` | — / 5,263-6,494 | — / 33224KB-33243KB | — / 376KB |
| `esp32s3-n16r8` | — / 2,193-2,618 | — / 8340KB-8350KB | — / 92KB-112KB |
| `pc-macos` | — / 200,000-1,000,000 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-25
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-25
- `pc-macos`: observed 2026-06-17 → 2026-07-01

#### `measure-mod-32` (measure)  📏

**Setup** (preceding non-measured steps):
- `mod-32-w` (set_control)
- `mod-32-h` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 547-586 | — / 130KB-140KB | — / 108KB |
| `esp32p4-eth` | — / 1,631-1,876 | — / 33218KB-33237KB | — / 376KB |
| `esp32s3-n16r8` | — / 600-710 | — / 8329KB-8344KB | — / 92KB-112KB |
| `pc-macos` | — / 5,882-333,333 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-26
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-26
- `pc-macos`: observed 2026-06-17 → 2026-06-25

#### `measure-mod-64` (measure)  📏

**Setup** (preceding non-measured steps):
- `mod-64-w` (set_control)
- `mod-64-h` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 144-149 | — / 111KB-122KB | — / 96KB-100KB |
| `esp32p4-eth` | — / 438-486 | — / 33194KB-33210KB | — / 376KB |
| `esp32s3-n16r8` | — / 119-162 | — / 8307KB-8317KB | — / 92KB-112KB |
| `pc-macos` | — / 9,434-71,429 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-26
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-25
- `pc-macos`: observed 2026-06-17 → 2026-07-01

#### `measure-mod-128` (measure)  📏

**Setup** (preceding non-measured steps):
- `mod-128-w` (set_control)
- `mod-128-h` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 29.8-35.1 | — / 36KB-47KB | — / 24KB-26KB |
| `esp32p4-eth` | — / 86.3-102 | — / 33089KB-33105KB | — / 376KB |
| `esp32s3-n16r8` | — / 16.8-35.6 | — / 8202KB-8212KB | — / 92KB-112KB |
| `pc-macos` | — / 3,378-16,393 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-25
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-25
- `pc-macos`: observed 2026-06-17 → 2026-07-01

### scenario_perf_light

`test/scenarios/light/scenario_perf_light.json` — Fast incremental performance check: start from the bare minimum render pipeline and add one thing at a time, measuring the tick/heap delta each step, so a regression shows up as a per-step jump. The LIGHT companion to scenario_perf_full — it stays small (≤64²) and driver-free so it runs in seconds. Mutate mode + canvas-preparing: the steps clear_children whatever Layouts/Layers/Drivers the device already had (the pre-wired apparatus like PreviewDriver/Board survives — clear_children only drops user-editable children) and rebuild a known tree, so it runs from any starting state and always measures the same minimal pipeline. Order: (1) minimal = Grid(16²)+Layer+a LIGHT effect (Spiral, a light effect), no modifier/driver/audio/discovery; (2) +MultiplyModifier (adds the mapping LUT — the heavy memory path); (3) +PreviewDriver; (4) swap to a HEAVY effect (Noise) to bracket the compute range; (5) grid 16²→32²→64² to show the size scaling. Full 128²/16K sweep, real LED/network drivers, audio+discovery cost: see scenario_perf_full.

**Mode**: `mutate` · **Also touches**: Layouts, GridLayout, Drivers, PreviewDriver, SpiralEffect, NoiseEffect, MultiplyModifier

#### `measure-minimal` (measure)  📏

Bare minimum: Grid(16²) + Layer + Spiral (light effect). No modifier, no driver. The render floor everything else is measured against.

**Setup** (preceding non-measured steps):
- `disable-audio` (set_control) — Quiet I2S sampling so it can't pollute the tick (optional — device only).
- `disable-devices` (set_control) — Stop the blocking HTTP discovery sweep (optional — device only).
- `clear-layers` (clear_children) — Drop whatever effects/modifiers the device had — start clean.
- `clear-layouts` (clear_children)
- `clear-drivers` (clear_children) — Drop any output driver; the pre-wired PreviewDriver is non-deletable and survives.
- `build-grid` (add_module)
- `build-layer` (add_module)
- `build-fx` (add_module)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 6,173-8,850 | — / 125KB-147KB | — / 108KB |
| `esp32p4-eth` | — / 13,699-18,519 | — / 33228KB-33246KB | — / 376KB |
| `esp32s3-n16r8` | — / 5,814-8,850 | — / 8316KB-8347KB | — / 80KB-104KB |
| `pc-macos` | — / 125,000-— | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-25
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-25
- `pc-macos`: observed 2026-06-17 → 2026-07-01

#### `measure-with-modifier` (measure)  📏

Cost of the modifier + LUT over the minimal pipeline. Heap delta vs measure-minimal is the LUT allocation.

**Setup** (preceding non-measured steps):
- `add-modifier` (add_module) — +MultiplyModifier: allocates the mapping LUT (the heavy memory path vs the 1:1 no-LUT case).

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 3,077-9,709 | — / 131KB-147KB | — / 108KB |
| `esp32p4-eth` | — / 8,621-10,309 | — / 33226KB-33243KB | — / 376KB |
| `esp32s3-n16r8` | — / 3,195-4,032 | — / 8330KB-8345KB | — / 92KB-100KB |
| `pc-macos` | — / 500,000-— | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-25
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-25
- `pc-macos`: observed 2026-06-17 → 2026-07-01

#### `measure-with-preview` (measure)  📏

PreviewDriver is the pre-wired apparatus — it survives clear_children and is already attached, so the measures above already include it (no add step needed; adding a second Preview is rejected). This is a stable repeat of the effect+modifier config for run-to-run variance.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 3,067-9,804 | — / 132KB-146KB | — / 108KB |
| `esp32p4-eth` | — / 10,417-10,753 | — / 33226KB-33243KB | — / 376KB |
| `esp32s3-n16r8` | — / 3,802-4,274 | — / 8330KB-8345KB | — / 84KB-100KB |
| `pc-macos` | — / 333,333-— | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-25
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-25
- `pc-macos`: observed 2026-06-17 → 2026-07-01

#### `measure-heavy-16` (measure)  📏

**Setup** (preceding non-measured steps):
- `swap-heavy-fx` (replace_module) — Swap the light effect for a HEAVY one (Noise — simplex per pixel) to bracket the compute range at the same 16².

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 1,142-3,268 | — / 131KB-146KB | — / 108KB |
| `esp32p4-eth` | — / 5,556-6,494 | — / 33224KB-33243KB | — / 376KB |
| `esp32s3-n16r8` | — / 2,299-2,506 | — / 8332KB-8342KB | — / 88KB-100KB |
| `pc-macos` | — / 200,000-1,000,000 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-25
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-25
- `pc-macos`: observed 2026-06-17 → 2026-07-01

#### `measure-heavy-32` (measure)  📏

**Setup** (preceding non-measured steps):
- `grid-32-w` (set_control)
- `grid-32-h` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 265-826 | — / 130KB-144KB | — / 108KB |
| `esp32p4-eth` | — / 1,603-1,880 | — / 33221KB-33237KB | — / 376KB |
| `esp32s3-n16r8` | — / 562-715 | — / 8328KB-8333KB | — / 84KB-104KB |
| `pc-macos` | — / 55,556-333,333 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-25
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-25
- `pc-macos`: observed 2026-06-17 → 2026-07-01

#### `measure-heavy-64` (measure)  📏

**Setup** (preceding non-measured steps):
- `grid-64-w` (set_control)
- `grid-64-h` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 77.1-227 | — / 111KB-135KB | — / 88KB-108KB |
| `esp32p4-eth` | — / 411-491 | — / 33195KB-33210KB | — / 376KB |
| `esp32s3-n16r8` | — / 129-162 | — / 8302KB-8317KB | — / 92KB-108KB |
| `pc-macos` | — / 13,514-71,429 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-17 → 2026-06-25
- `esp32p4-eth`: observed 2026-06-17 → 2026-06-25
- `esp32s3-n16r8`: observed 2026-06-17 → 2026-06-25
- `pc-macos`: observed 2026-06-17 → 2026-07-01

## Layers

### scenario_Layers_composition

`test/scenarios/light/scenario_Layers_composition.json` — Multi-layer composition end-to-end: Layouts→Grid, TWO Layers under one Layers container (bottom Spiral, top Rainbow), Drivers→NetworkSendDriver. Proves the Drivers composite loop builds, allocates its output buffer, blends both enabled layers and feeds the result to the driver without crashing, and gates the bounded FPS so the N-pass composite cost is tracked. The exact alpha/additive blend math and the disable-drops-to-single-layer path are pinned by the unit tests (unit_BlendMap, unit_Layers_container); construct-mode set_control can't apply controls (built post-scheduler), so this scenario uses each Layer's default blend (alpha, full opacity) and asserts wired liveness + tick, not per-byte blend output.

**Mode**: `construct` · **Also touches**: Layer, GridLayout, RainbowEffect, SpiralEffect, Drivers, NetworkSendDriver

#### `add-artnet` (add_module)  📏

Add NetworkSendDriver and run the bounded FPS measurement over the two-layer composite (min_pct gates on hardware; skipped with a WARN in the desktop runner).

**Setup** (preceding non-measured steps):
- `add-layout-group` (add_module) — Top-level Layouts container.
- `add-grid` (add_module) — 128x128 GridLayout under Layouts (above host clock resolution so the composite tick is measurable).
- `add-layers-group` (add_module) — Top-level Layers container — the multi-layer composition host.
- `add-bottom-layer` (add_module) — Bottom Layer (composited first — clears + overwrites the output buffer). RGB.
- `add-bottom-effect` (add_module) — A Spiral base as the bottom layer's effect.
- `add-top-layer` (add_module) — Top Layer (composited second — blends onto the bottom with its default blend). RGB.
- `add-top-effect` (add_module) — Rainbow as the top layer's effect — composited over the Spiral base.
- `add-driver-group` (add_module) — Top-level Drivers container wired to the Layers container (composites all enabled layers into its output buffer).

**Bounds**:
- FPS ≥ 80% of baseline
- FPS × lights ≥ 294,912

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 1,361-19,231 | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-25 → 2026-07-01

## Layouts

### scenario_Layouts_mutation

`test/scenarios/light/scenario_Layouts_mutation.json` — Tree mutation on the Layouts container while the pipeline runs: add a second layout (multiple layouts under one Layouts), replace a layout with a different type, and remove a layout. The check is that each mutation leaves the pipeline RENDERING — Layer + Drivers re-wire via buildState and the buffer stays non-null and non-zero. Mirrors the HTTP add/replace/delete handlers; exercises the runner's add_module / replace_module / remove_module ops. NOTE: the Layer renders a dense bounding-box buffer sized by the layouts' coordinate EXTENT, not the summed light count — layouts that overlap in coordinate space share voxels (two 64x64 grids both occupy x,y in 0..63). There are no per-layout coordinate offsets, so multiple layouts share the same coordinate box; these steps assert liveness, not buffer-size arithmetic. Grids are 64x64 so the tick stays above the host's microsecond clock at every step.

**Mode**: `mutate` · **Also touches**: GridLayout, SphereLayout, Layer, RainbowEffect, Drivers, NetworkSendDriver

#### `measure-one-layout` (measure)  📏

Baseline: a single 64x64 grid layout drives the pipeline.

**Bounds**:
- FPS ≥ 1 (absolute)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 41,667 | — / 224KB | — / 108KB |
| `pc-macos` | — / 15,873-125,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 32,258-37,037 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-08
- `pc-macos`: observed 2026-06-05 → 2026-07-01
- `pc-windows`: observed 2026-06-07

#### `measure-two-layouts` (measure)  📏

Pipeline still renders with two layouts wired (buffer non-null, fps measurable).

**Setup** (preceding non-measured steps):
- `add-second-layout` (add_module) — Add a SECOND layout (a 64x64 grid) under Layouts — two layouts now live under one container. buildState re-runs; the pipeline must still render. (Both grids share the 0..63 coordinate box, so the Layer buffer stays 64x64 — see the scenario NOTE.)

**Bounds**:
- FPS ≥ 1 (absolute)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 37,037 | — / 223KB | — / 108KB |
| `pc-macos` | — / 3,953-111,111 | — / unlimited | — / unlimited |
| `pc-windows` | — / 16,393-23,810 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-08
- `pc-macos`: observed 2026-06-05 → 2026-06-27
- `pc-windows`: observed 2026-06-07

#### `measure-after-replace` (measure)  📏

Pipeline still renders after replacing a grid with a sphere (different layout type, same slot) — buffer re-wires without crashing.

**Setup** (preceding non-measured steps):
- `replace-second-layout` (replace_module) — Replace the second grid with a SphereLayout (different type, same slot). The first grid is untouched; the pipeline re-wires to the new layout's light count.

**Bounds**:
- FPS ≥ 1 (absolute)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 38,462 | — / 223KB | — / 108KB |
| `pc-macos` | — / 1,957-100,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 5,848-9,009 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-08
- `pc-macos`: observed 2026-06-05 → 2026-06-27
- `pc-windows`: observed 2026-06-07

#### `measure-after-remove` (measure)  📏

Pipeline renders with the single remaining grid, same as the baseline.

**Setup** (preceding non-measured steps):
- `remove-second-layout` (remove_module) — Remove the sphere — back to a single grid layout. Layer/Drivers shrink their buffers via buildState.

**Bounds**:
- FPS ≥ 1 (absolute)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 41,667 | — / 224KB | — / 108KB |
| `pc-macos` | — / 6,623-125,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 33,333-38,462 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-08
- `pc-macos`: observed 2026-06-05 → 2026-07-01
- `pc-windows`: observed 2026-06-07

## MoonLiveEffect

### scenario_MoonLiveEffect_controls

`test/scenarios/light/scenario_MoonLiveEffect_controls.json` — Exercise MoonLive Stage-1 CONTROLS end-to-end as a wired module. A script declares a control (`uint8_t speed = 7; // @control 0..15`) and uses it (`setRGB(speed, ...)`); the engine surfaces the control, the binding creates a real uint8 MoonModule control bound to the live control-values arena slot. The scenario: add the effect with a control script (the control appears, renders), change the CONTROL value live (a slider move — must NOT recompile; the arena byte updates and the next tick reads it), edit the SOURCE to add a second control (recompile re-derives the set, existing slider value preserved by the stable-address grow-only arena), edit the source to remove a control (the orphaned value drops), push a broken script (compile fails, renders dark, status shows the diagnostic, no crash), recover, and remove + re-add (resource teardown + re-acquire). A crash in the LoadCtrl codegen, a dangling arena pointer across a recompile, or a value change that wrongly triggers a recompile all show up as a failed measure or a tick spike. The codegen + live-read contract is pinned by unit_moonlive_ir / unit_moonlive_compiler; this is the wired-module gate.

**Mode**: `mutate` · **Also touches**: Layouts, GridLayout, Layers, Layer, Drivers, NetworkSendDriver

#### `add-control-script` (add_module)  📏

Add a MoonLiveEffect whose source declares a `speed` control and uses it. The control appears bound to the arena slot (seeded to its default 7); the wired effect renders one pixel.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 250,000-— | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-28 → 2026-07-01

#### `set-source-with-control` (set_control)  📏

Edit the source to the control script. A source edit recompiles (controlChangeTriggersBuildState gates on `source`); the engine derives the `speed` control and the binding surfaces it.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 1,000,000-— | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-28 → 2026-07-01

#### `change-control-live` (set_control)  📏

Change the `speed` control value (a slider move). This must NOT recompile — controlChangeTriggersBuildState returns false for a scripted control; the arena byte updates and the next render tick reads it. Tick stays cheap (a recompile would spike it).

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 1,000,000-— | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-28 → 2026-07-01

#### `edit-source-two-controls` (set_control)  📏

Edit the source to add a second control. The recompile re-derives the control set; the stable-address grow-only arena keeps `speed`'s live value while seeding the new slot.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 1,000,000-— | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-28 → 2026-07-01

#### `edit-source-shrink-to-one-control` (set_control)  📏

Edit the source back to a single control. The control set shrinks 2 -> 1: `speed` stays bound (its live value kept), the removed `hue`'s value is dropped, and the value change path is exercised without an unexpected recompile crash.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 1,000,000-— | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-28 → 2026-07-01

#### `edit-source-broken` (set_control)  📏

Push a broken script. Compile fails, the previous code is freed, the effect renders dark and the parse error surfaces in status — no crash.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 125,000-— | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-28 → 2026-07-01

#### `edit-source-recover` (set_control)  📏

Recover with a valid control script — the effect compiles and renders again.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 1,000,000-— | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-28 → 2026-07-01

#### `re-add-control-effect` (add_module)  📏

Re-add a fresh effect after the remove — exec memory + control arena re-acquired clean (it renders its default fill on add). Control re-acquisition itself is proven by the add-control-script step at the top: a freshly-added effect compiling a control source surfaces + seeds its control; construct-mode set_control can't apply a dynamically-added scripted control as the final asserted render, so the gate here is the bare re-add's liveness.

**Setup** (preceding non-measured steps):
- `remove-control-effect` (remove_module) — Remove the effect — the engine releases its exec block AND its control arena (teardown).

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 1,000,000-— | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-28 → 2026-07-01

### scenario_MoonLiveEffect_livescript

`test/scenarios/light/scenario_MoonLiveEffect_livescript.json` — Exercise a scripted MoonLiveEffect as a wired MoonModule end-to-end — the integration layer the unit tests can't reach. The effect compiles its `source` text to native code on-device and renders it into the Layer buffer each tick. Prepares its own canvas: Layout(Grid 16x16) + Layer + MoonLiveEffect, measures the default compile, then edits `source` live (a new fill colour recompiles and keeps rendering), pushes a BROKEN script (compile fails, the previous code is freed, the effect renders dark and the parse error surfaces in status, no crash), recovers with a valid script, and finally removes + re-adds the effect (add/remove robustness in any order). A crash in the JIT/emit path, a failed recompile that wedges the tick, or a buffer overrun on an odd grid all show up as a failed measure. The compiler + emit golden bytes are pinned by unit_moonlive_compiler / unit_moonlive_fill; this is the live wired-module gate.

**Mode**: `mutate` · **Also touches**: Layouts, GridLayout, Layers, Layer, Drivers, NetworkSendDriver

#### `add-moonlive` (add_module)  📏

Add a MoonLiveEffect to the Layer. Its default source (random pixels) compiles on-device to native code; measure that the wired effect renders.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32p4-eth` | — / 88.6 | — / 33211KB | — / 376KB |
| `esp32s3-n16r8` | — / 249 | — / 8341KB | — / 104KB |
| `pc-macos` | — / 2,545-— | — / unlimited | — / unlimited |

- `esp32p4-eth`: observed 2026-06-27
- `esp32s3-n16r8`: observed 2026-06-27
- `pc-macos`: observed 2026-06-26 → 2026-06-27

#### `edit-source-red` (set_control)  📏

Live-edit the script source to a new colour. A source edit triggers a recompile (controlChangeTriggersBuildState gates on `source`); the new native code swaps in and keeps rendering.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32p4-eth` | — / 98.4 | — / 33213KB | — / 376KB |
| `esp32s3-n16r8` | — / 225 | — / 8341KB | — / 104KB |
| `pc-macos` | — / 2,513-— | — / unlimited | — / unlimited |

- `esp32p4-eth`: observed 2026-06-27
- `esp32s3-n16r8`: observed 2026-06-27
- `pc-macos`: observed 2026-06-26 → 2026-06-27

#### `edit-source-broken` (set_control)  📏

Push a script that fails to parse. The compile fails, the engine reports the diagnostic in the module status and renders dark, but the device keeps running (robust, no reboot) — the script-editor failure path. The measure passes because the pipeline still ticks.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32p4-eth` | — / 94.6 | — / 33209KB | — / 376KB |
| `esp32s3-n16r8` | — / 229 | — / 8340KB | — / 104KB |
| `pc-macos` | — / 3,745-— | — / unlimited | — / unlimited |

- `esp32p4-eth`: observed 2026-06-27
- `esp32s3-n16r8`: observed 2026-06-27
- `pc-macos`: observed 2026-06-26 → 2026-06-27

#### `edit-source-recover` (set_control)  📏

Push a valid script again. The engine recompiles cleanly and rendering resumes — a broken edit is fully recoverable.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32p4-eth` | — / 93.4 | — / 33212KB | — / 376KB |
| `esp32s3-n16r8` | — / 248 | — / 8340KB | — / 100KB |
| `pc-macos` | — / 2,415-— | — / unlimited | — / unlimited |

- `esp32p4-eth`: observed 2026-06-27
- `esp32s3-n16r8`: observed 2026-06-27
- `pc-macos`: observed 2026-06-26 → 2026-06-27

#### `shrink-grid-1x1` (set_control)  📏

Resize the canvas to 1x1 while the scripted effect renders — the smallest non-empty grid. The native fill loops over a single light; the run guards (non-null buffer, cpl>=3) keep it in-bounds. Pins the 'runs at every grid size' hard rule for the JIT'd routine.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32p4-eth` | — / 862 | — / 33215KB | — / 376KB |
| `esp32s3-n16r8` | — / 868 | — / 8341KB | — / 100KB |
| `pc-macos` | — / 1,000,000-— | — / unlimited | — / unlimited |

- `esp32p4-eth`: observed 2026-06-27
- `esp32s3-n16r8`: observed 2026-06-27
- `pc-macos`: observed 2026-06-26 → 2026-06-27

#### `grow-grid-back` (set_control)  📏

Resize back to a wider grid; the effect keeps rendering across the live dimension change (the no-reboot reconfiguration contract applied to scripted code).

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32p4-eth` | — / 97.0 | — / 33209KB | — / 376KB |
| `esp32s3-n16r8` | — / 136 | — / 8333KB | — / 100KB |
| `pc-macos` | — / 71,429-— | — / unlimited | — / unlimited |

- `esp32p4-eth`: observed 2026-06-27
- `esp32s3-n16r8`: observed 2026-06-27
- `pc-macos`: observed 2026-06-26 → 2026-06-27

#### `remove-moonlive` (remove_module)  📏

Remove the scripted effect. teardown frees the exec block; the Layer keeps rendering (now empty). Measures add/remove robustness.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32p4-eth` | — / 88.2 | — / 33209KB | — / 376KB |
| `esp32s3-n16r8` | — / 135 | — / 8333KB | — / 100KB |
| `pc-macos` | — / 100,000-— | — / unlimited | — / unlimited |

- `esp32p4-eth`: observed 2026-06-27
- `esp32s3-n16r8`: observed 2026-06-27
- `pc-macos`: observed 2026-06-26 → 2026-06-27

#### `re-add-moonlive` (add_module)  📏

Re-add a MoonLiveEffect after removal — the exec memory is re-acquired fresh, no leak, no stale pointer. The scripted effect renders again.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32p4-eth` | — / 90.9 | — / 33209KB | — / 376KB |
| `esp32s3-n16r8` | — / 121 | — / 8332KB | — / 100KB |
| `pc-macos` | — / 62,500-— | — / unlimited | — / unlimited |

- `esp32p4-eth`: observed 2026-06-27
- `esp32s3-n16r8`: observed 2026-06-27
- `pc-macos`: observed 2026-06-26 → 2026-06-27

## MoonModule

### scenario_MoonModule_control_change

`test/scenarios/core/scenario_MoonModule_control_change.json` — Measure the cost of control changes on a running pipeline. Toggles MultiplyModifier's mirrorX/Y at different points and verifies each change is applied without freezing the render loop. Companion to the MoonModule control-change gate unit tests (unit_MoonModule_control_change_gate.cpp) — this is the live equivalent.

**Mode**: `mutate` · **Also touches**: MultiplyModifier, NoiseEffect

#### `baseline` (set_control)  📏

Set NoiseEffect.scale=4 and measure baseline FPS (mirror on). Effect controls don't rebuild the pipeline — slider stutter check.

**Setup** (preceding non-measured steps):
- `canvas-clear-layers` (clear_children) — Self-canvas: clear+rebuild the pipeline this scenario assumes, so it runs from any device state (order-independent in a chained live run). Pre-wired apparatus survives clear_children; replaces the old fixture+reset model.
- `canvas-clear-layouts` (clear_children)
- `canvas-clear-drivers` (clear_children)
- `canvas-grid` (add_module)
- `canvas-layer` (add_module)
- `canvas-noise` (add_module)
- `canvas-mirror` (add_module)
- `canvas-artnet` (add_module)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 3.9 | — / 88KB | — / 48KB |
| `esp32-eth` | — / 10.5-10.6 | — / 133KB | — / 48KB-50KB |
| `esp32-eth-wifi` | ≥ 10.0 / 12.2 | ≥ 103KB / 94KB | — / 48KB |
| `esp32p4-eth` | — / 4,926-6,250 | — / 33238KB | — / 376KB |
| `pc-macos` | ≥ 8,333 / 3,165-10,309 | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 4,000-4,405 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `esp32p4-eth`: observed 2026-06-17
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-25
- `pc-windows`: observed 2026-06-07

#### `disable-mirrorX` (set_control)  📏

Disable mirrorX. Modifier control triggers a pipeline rebuild — measures the rebuilt path.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 4.8 | — / 88KB | — / 48KB |
| `esp32-eth` | — / 10.4 | — / 132KB | — / 48KB-50KB |
| `esp32-eth-wifi` | ≥ 10.0 / 12.0 | ≥ 103KB / 94KB | — / 48KB |
| `esp32p4-eth` | — / 5,952-6,135 | — / 33238KB | — / 376KB |
| `pc-macos` | ≥ 5,000 / 2,857-9,259 | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 2,024-2,392 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `esp32p4-eth`: observed 2026-06-17
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-07-01
- `pc-windows`: observed 2026-06-07

#### `disable-mirrorY` (set_control)  📏

Disable mirrorY. Mirror is now fully off — should land on the no-LUT path.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 4.4 | — / 88KB | — / 48KB |
| `esp32-eth` | — / 8.9-9.0 | — / 132KB | — / 48KB-50KB |
| `esp32-eth-wifi` | ≥ 10.0 / 11.1 | ≥ 103KB / 94KB | — / 48KB |
| `esp32p4-eth` | — / 5,587-6,061 | — / 33238KB | — / 376KB |
| `pc-macos` | ≥ 2,500 / 1,916-9,346 | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 1,082-1,305 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `esp32p4-eth`: observed 2026-06-17
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-15
- `pc-windows`: observed 2026-06-07

#### `re-enable-mirrorY` (set_control)  📏

Re-enable mirrorY and measure — the heavy LUT path must recover (FPS within 50% of baseline) without staying degraded.

**Setup** (preceding non-measured steps):
- `re-enable-mirrors` (set_control) — Re-enable mirrorX (rebuild back to LUT path).

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 4.4 | — / 88KB | — / 48KB |
| `esp32-eth` | — / 10.5-10.6 | — / 132KB | — / 48KB-50KB |
| `esp32-eth-wifi` | ≥ 10.0 / 12.1 | ≥ 103KB / 94KB | — / 48KB |
| `esp32p4-eth` | — / 5,319-6,098 | — / 33238KB | — / 376KB |
| `pc-macos` | ≥ 8,333 / 3,356-10,417 | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 4,065-4,854 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `esp32p4-eth`: observed 2026-06-17
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-30
- `pc-windows`: observed 2026-06-07

## MultiplyModifier

### scenario_MultiplyModifier_memory_lut

`test/scenarios/light/scenario_MultiplyModifier_memory_lut.json` — Verify that adding a MultiplyModifier allocates both the mapping LUT and the driver buffer (the heavy memory path). Companion to scenario_Layer_memory_1to1, which verifies the no-LUT path.

**Mode**: `construct` · **Also touches**: Layer, MappingLUT, BlendMap

#### `add-artnet` (add_module)  📏

Add NetworkSendDriver and run the bounded FPS measurement on the LUT path.

**Setup** (preceding non-measured steps):
- `add-layout-group` (add_module) — Create the top-level Layouts container.
- `add-grid` (add_module) — Add a 16x16 GridLayout.
- `add-layer` (add_module) — Add a Layer wired to Layouts (RGB).
- `add-noise` (add_module) — Add NoiseEffect as the Layer's effect.
- `add-mirror` (add_module) — Add MultiplyModifier — triggers LUT and driver-buffer allocation.
- `add-driver-group` (add_module) — Add a Drivers container wired to the Layer.

**Bounds**:
- FPS ≥ 80% of baseline

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | ≥ 8,333 / 3,322-1,000,000 | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 166,667-333,333 | — / unlimited | — / unlimited |

- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-05
- `pc-windows`: observed 2026-06-07

### scenario_MultiplyModifier_pipeline

`test/scenarios/light/scenario_MultiplyModifier_pipeline.json` — Pipeline with a mirror modifier: NoiseEffect renders one quadrant, MultiplyModifier reflects across X and Y to produce a kaleidoscope. Used to verify the MultiplyModifier wires into Layer cleanly and that the full pipeline still meets its FPS bound.

**Mode**: `construct` · **Also touches**: Layer, NoiseEffect, NetworkSendDriver

#### `add-artnet` (add_module)  📏

Add NetworkSendDriver and run the bounded FPS measurement (mirror + LUT path must stay at >=80% of the rated FPS).

**Setup** (preceding non-measured steps):
- `add-layout-group` (add_module) — Create the top-level Layouts container.
- `add-grid` (add_module) — Add a 128x128 GridLayout child to Layouts. Set explicitly (the module default is 16x16x1) so the tick is measurable above the host's microsecond clock.
- `add-layer` (add_module) — Add a Layer wired to Layouts (RGB).
- `add-noise` (add_module) — Add NoiseEffect as the Layer's effect.
- `add-mirror` (add_module) — Add MultiplyModifier so logical pixels reflect across X and Y in the physical grid.
- `add-driver-group` (add_module) — Add a Drivers container wired to the Layer's output buffer.

**Bounds**:
- FPS ≥ 80% of baseline

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | ≥ 8,333 / 3,676-1,000,000 | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 3,953-4,444 | — / unlimited | — / unlimited |

- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-25
- `pc-windows`: observed 2026-06-07

## NetworkModule

### scenario_NetworkModule_eth_reconfigure

`test/scenarios/core/scenario_NetworkModule_eth_reconfigure.json` — Cycle the Ethernet PHY type (ethType: None/LAN8720/IP101/W5500) live on a running device and confirm the render loop survives every transition. Pins the robustness principle for the runtime Ethernet config: changing ethType reshapes the platform eth config (NetworkModule.syncEthLive → setEthConfig, with a live ethStop+ethInit on W5500), and no value or transition order may crash or wedge the tick. Runs live only — the eth controls exist only on hasEthernet ESP32 builds, so the in-process desktop runner SKIPs it; on a device it drives the controls over HTTP. On RMII boards (Olimex) the change saves + asks for restart (no hot re-init), so the live HTTP connection is undisturbed; the W5500 path hot-reinits but the SPI bus teardown keeps the netif alive enough to keep serving.

**Mode**: `mutate` · **live-only** (skipped in-process)

#### `ethType-lan8720` (set_control)  📏

LAN8720 (RMII) — the classic-ESP32 default. Baseline: device renders with Ethernet RMII selected.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 133-146 | — / 165KB | — / 108KB |

- `esp32`: observed 2026-06-15

#### `ethType-none` (set_control)  📏

Switch to None (no Ethernet) live. The eth pin rows hide; the device must keep rendering and stay reachable.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 145-147 | — / 165KB | — / 108KB |

- `esp32`: observed 2026-06-15

#### `ethType-w5500` (set_control)  📏

Switch to W5500 (SPI) live. On an S3 this hot-reinits eth (ethStop + ethInit); on RMII boards it saves + flags restart. Either way the render loop must survive.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 133-135 | — / 165KB | — / 108KB |

- `esp32`: observed 2026-06-15

#### `ethType-ip101` (set_control)  📏

Switch to IP101 (RMII) live — the P4 PHY. Exercises the last dropdown value; render loop must survive.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 146-147 | — / 165KB | — / 108KB |

- `esp32`: observed 2026-06-15

#### `ethType-back-to-lan8720` (set_control)  📏

Return to LAN8720 — confirms the cycle is reversible and leaves the device in a sane state.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 134-145 | — / 165KB | — / 108KB |

- `esp32`: observed 2026-06-15

### scenario_NetworkModule_mdns_toggle

`test/scenarios/core/scenario_NetworkModule_mdns_toggle.json` — Toggle the mDNS responder on and off and measure render-FPS impact. Validates that mDNS announcement traffic doesn't degrade the render loop more than 20% on the busiest tick.

**Mode**: `mutate` · **live-only** (skipped in-process)

#### `baseline-mdns-on` (set_control)  📏

mDNS on (default) — captures the baseline FPS for the next two steps.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 4.4 | — / 88KB | — / 48KB |
| `esp32-eth` | — / 10.5-10.6 | — / 132KB | — / 48KB-50KB |
| `esp32-eth-wifi` | ≥ 10.0 / 12.2 | ≥ 103KB / 93KB | — / 48KB |
| `esp32p4-eth` | — / 47,619 | — / 33245KB | — / 376KB |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `esp32p4-eth`: observed 2026-06-17

#### `mdns-off` (set_control)  📏

mDNS off — measured. Expected to match or exceed the baseline.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 3.6 | — / 88KB | — / 48KB |
| `esp32-eth` | — / 10.3-10.5 | — / 137KB | — / 48KB-52KB |
| `esp32-eth-wifi` | ≥ 10.0 / 12.0 | ≥ 93KB / 98KB | — / 48KB |
| `esp32p4-eth` | — / 47,619 | — / 33245KB | — / 376KB |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "shared heap budget; cumulative sweep state reduces standalone-mDNS-off heap by ~15KB" · observed 2026-06-02
- `esp32p4-eth`: observed 2026-06-17

#### `mdns-on-again` (set_control)  📏

mDNS on again — measured with a bound: FPS must stay within 20% of the baseline (proves toggling doesn't leave the network task in a degraded state).

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 4.3 | — / 83KB | — / 48KB |
| `esp32-eth` | — / 9.1 | — / 132KB | — / 48KB-52KB |
| `esp32-eth-wifi` | ≥ 10.0 / 10.6 | ≥ 103KB / 93KB | — / 48KB |
| `esp32p4-eth` | — / 45,455-50,000 | — / 33245KB | — / 376KB |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `esp32p4-eth`: observed 2026-06-17

## NetworkSendDriver

### scenario_Driver_mutation

`test/scenarios/light/scenario_Driver_mutation.json` — Add / remove output drivers under the Drivers container while the render pipeline runs, proving the robustness rule for driver deletion (the LED-driver delete path the product owner asked to cover; on the host RmtLed/LcdLed/Parlio are inert via platform constants, so the portable NetworkSendDriver + PreviewDriver exercise the SAME generic add/remove lifecycle through the Scheduler that the hardware drivers use). Drivers are consumers of the Layer's output buffer, added/removed at runtime in any order. The checks assert the pipeline keeps RENDERING (buffer non-null, fps measurable) through each mutation: adding a second driver, removing it, and crucially removing a driver while the pipeline is still live (a driver teardown must release its resources without stranding the buffer or the other drivers — the same end-to-end Scheduler path the Audio producer/consumer scenario proves for peripherals, here for the output stage). Grid is 64x64 so the tick stays above the host microsecond clock at every step.

**Mode**: `mutate` · **Also touches**: GridLayout, Layer, RainbowEffect, Drivers, PreviewDriver

#### `measure-one-driver` (measure)  📏

Baseline: the pipeline renders with one driver (Preview) wired.

**Bounds**:
- FPS ≥ 1 (absolute)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 13,699-125,000 | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-13 → 2026-06-30

#### `measure-two-drivers` (measure)  📏

Pipeline renders with both drivers wired.

**Setup** (preceding non-measured steps):
- `add-second-driver` (add_module) — Add a NetworkSendDriver beside the Preview driver — two consumers of the same Layer output buffer. The add must not disturb the running pipeline.

**Bounds**:
- FPS ≥ 1 (absolute)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 10,204-125,000 | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-13 → 2026-07-01

#### `measure-three-drivers` (measure)  📏

Pipeline renders with three drivers wired.

**Setup** (preceding non-measured steps):
- `add-third-driver` (add_module) — Add a second NetworkSendDriver — three drivers now share the Layer output buffer (Preview + two ArtNet). Stacking editable drivers before tearing them down in sequence.

**Bounds**:
- FPS ≥ 1 (absolute)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 13,333-125,000 | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-13 → 2026-07-01

#### `measure-after-first-remove` (measure)  📏

One ArtNet gone, Preview + ArtNet2 remain: pipeline keeps rendering (buffer non-null, fps measurable). No crash from the mid-list teardown.

**Setup** (preceding non-measured steps):
- `remove-first-added-driver` (remove_module) — Remove the FIRST-added NetworkSendDriver while the others (Preview + ArtNet2) are still live — delete a middle consumer, not the last. Its teardown must release its resources without stranding the buffer or the surviving drivers.

**Bounds**:
- FPS ≥ 1 (absolute)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 15,152-125,000 | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-13 → 2026-07-01

#### `measure-back-to-one-driver` (measure)  📏

Both added drivers gone, back to the single Preview baseline, still rendering — the add/remove cycle leaves the pipeline coherent.

**Setup** (preceding non-measured steps):
- `remove-second-added-driver` (remove_module) — Remove the remaining editable driver (ArtNet2) too — back to just the boot-wired Preview. Repeated driver teardown leaves no residue; the pipeline still renders. (PreviewDriver is userEditable=false, so it stays — drivers always have at least the wired output consumer; this mirrors the live API, which forbids deleting code-wired modules.)

**Bounds**:
- FPS ≥ 1 (absolute)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 15,873-125,000 | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-13 → 2026-07-01
