# Scenario Tests

Auto-generated from `test/scenarios/{core,light}/scenario_*.json` by `scripts/docs/generate_test_docs.py`. **Do not edit by hand** — update the JSON file's top-level fields and per-step `description` / `bounds` / `contract` / `observed` instead, then regenerate.

Scenario tests are the integration tier in the [test strategy](../testing.md): each one is a JSON script that drives the full pipeline (PC or live ESP32) and captures tick / heap per step against per-target contracts. Run them with `scripts/scenario/run_scenario.py` (PC) or `scripts/scenario/run_live_scenario.py` (live device). See [testing.md § Performance contracts](../testing.md#performance-contracts-contracttarget) for the contract semantics.

## AudioModule

### scenario_Audio_mutation

`test/scenarios/light/scenario_Audio_mutation.json` — Add / configure / remove the AudioModule peripheral and an audio-reactive effect while the render pipeline runs, proving the robustness rule for the audio producer/consumer pair. AudioModule is a Peripheral (it sits beside the pipeline, publishing an AudioFrame), and the audio effects read it through the static AudioModule::latestFrame() accessor, NOT a boot-time pointer — so add/remove can happen in any order at runtime. The checks assert the pipeline keeps RENDERING (buffer non-null, fps measurable) through each mutation: adding the mic, setting its pins (the user-configures-then-runs flow), adding a consumer effect, and crucially REMOVING the mic while a consumer is still live (the consumer must fall back to a silent frame, never deref a dangling pointer — the bug the boot-loop fix and the unit lifecycle tests pin, here proven end-to-end through the Scheduler). On the host the mic is inert (hasI2sMic false), so this exercises the wiring/lifecycle, not real capture; capture is proven on hardware. Grid is 64x64 so the tick stays above the host microsecond clock at every step.

**Mode**: `mutate` · **Also touches**: SystemModule, Layouts, GridLayout, Layer, RainbowEffect, AudioVolumeEffect, AudioSpectrumEffect, Drivers, PreviewDriver

#### `measure-pipeline-only` (measure)  📏

Baseline: the render pipeline runs with no audio module present.

**Bounds**:
- FPS ≥ 1 (absolute)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 100,000-111,111 | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-12

#### `measure-audio-added` (measure)  📏

Pipeline still renders with the (idle, unconfigured) mic added.

**Setup** (preceding non-measured steps):
- `add-audio-module` (add_module) — Add the AudioModule peripheral under SystemModule (where the user adds it, beside the board). Pins default unset, so it stays idle; the pipeline must keep rendering.

**Bounds**:
- FPS ≥ 1 (absolute)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 111,111 | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-12

#### `measure-consumer-live` (measure)  📏

Pipeline renders with the producer + consumer both wired.

**Setup** (preceding non-measured steps):
- `configure-audio-pins` (set_control) — Set a mic pin — the user-configures-then-runs flow. On host the mic is inert, but the buildState rebuild must not disturb the running pipeline.
- `add-audio-consumer` (add_module) — Add an AudioVolumeEffect consumer under the Layer. It reads the mic via the static accessor; with the mic present it gets the live (silent, on host) frame.

**Bounds**:
- FPS ≥ 1 (absolute)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 100,000-111,111 | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-12

#### `measure-after-mic-removed` (measure)  📏

Mic gone, consumer remains: pipeline keeps rendering on silent audio (buffer non-null, fps measurable). No crash from the orphaned consumer.

**Setup** (preceding non-measured steps):
- `remove-audio-module` (remove_module) — Remove the mic while the consumer is STILL live. The consumer must fall back to AudioModule::latestFrame()'s static silence — no dangling pointer, no crash. This is the robustness rule's hardest case for this pair.

**Bounds**:
- FPS ≥ 1 (absolute)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 111,111 | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-12

#### `measure-back-to-baseline` (measure)  📏

Both audio modules gone: back to the pipeline-only baseline, still rendering.

**Setup** (preceding non-measured steps):
- `remove-audio-consumer` (remove_module) — Remove the orphaned consumer too — clean teardown, pipeline still live.

**Bounds**:
- FPS ≥ 1 (absolute)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | — / 100,000-111,111 | — / unlimited | — / unlimited |

- `pc-macos`: observed 2026-06-12

## GridLayout

### scenario_GridLayout_grid_sizes

`test/scenarios/light/scenario_GridLayout_grid_sizes.json` — Walk the grid through 16x16 → 32x32 → 64x64 → 128x128 and assert a per-size FPS floor.

**Mode**: `mutate` · **Also touches**: Layer, MultiplyModifier, NoiseEffect, Drivers, NetworkSendDriver, PreviewDriver

#### `size-16x16` (set_control)  📏

16x16 (256 lights) measured — smallest realistic display. Should hit the device's max FPS.

**Setup** (preceding non-measured steps):
- `size-16x16-width` (set_control) — Start of the 16x16 case: set width first (height still carries over from the reset / previous step). The measurement happens on the NEXT step, after height is also set — otherwise we'd be measuring an N×128 stripe.

**Bounds**:
- FPS ≥ 80% of baseline

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 1,337 | — / 129KB | — / 48KB |
| `esp32-eth` | ≥ 1,429 / 1,845-1,848 | ≥ 166KB / 178KB | ≥ 88KB / 96KB-100KB |
| `esp32-eth-wifi` | ≥ 1,429 / 1,821 | ≥ 146KB / 139KB | ≥ 49KB / 52KB |
| `esp32s3-n16r8` | — / 1,672 | — / 8360KB | — / 160KB |
| `pc-macos` | ≥ 200,000 / 200,000-1,000,000 | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 142,857-333,333 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-02
- `esp32-eth`: contract set 2026-06-02 "anti-regression floor; LUT-fit telemetry baseline" · observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `esp32s3-n16r8`: observed 2026-06-04
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `pc-windows`: observed 2026-06-07

#### `size-32x32` (set_control)  📏

32x32 measured. ~4x more lights than 16x16.

**Setup** (preceding non-measured steps):
- `size-32x32-width` (set_control) — 32x32 (1024 lights).

**Bounds**:
- FPS ≥ 80% of baseline

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 147 | — / 121KB | — / 48KB |
| `esp32-eth` | ≥ 303 / 379-381 | ≥ 161KB / 172KB | ≥ 78KB / 92KB |
| `esp32-eth-wifi` | ≥ 400 / 390 | ≥ 142KB / 132KB | ≥ 49KB / 50KB |
| `esp32s3-n16r8` | — / 288 | — / 8349KB | — / 140KB |
| `pc-macos` | ≥ 100,000 / 76,923-200,000 | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 71,429-90,909 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-02
- `esp32-eth`: contract set 2026-06-02 "anti-regression floor; LUT-fit telemetry baseline" · observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `esp32s3-n16r8`: observed 2026-06-04
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-11
- `pc-windows`: observed 2026-06-07

#### `size-64x64` (set_control)  📏

64x64 measured. Real-world mid size. Target: 60 FPS on a fast Ethernet device.

**Setup** (preceding non-measured steps):
- `size-64x64-width` (set_control) — 64x64 (4096 lights).

**Bounds**:
- FPS ≥ 80% of baseline

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 17.5 | — / 97KB | — / 48KB |
| `esp32-eth` | ≥ 55.6 / 74.5-74.7 | ≥ 137KB / 147KB | ≥ 54KB / 62KB |
| `esp32-eth-wifi` | ≥ 76.9 / 85.7 | ≥ 117KB / 108KB | ≥ 44KB / 48KB |
| `esp32s3-n16r8` | — / 25.9 | — / 8310KB | — / 152KB |
| `pc-macos` | ≥ 33,333 / 4,484-43,478 | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 17,857-22,727 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-02
- `esp32-eth`: contract set 2026-06-02 "anti-regression floor; LUT-fit telemetry baseline" · observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `esp32s3-n16r8`: observed 2026-06-04
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-11
- `pc-windows`: observed 2026-06-07

#### `size-128x128` (set_control)  📏

128x128 measured. Real-world full-room size. Target: 20 FPS on a typical Ethernet device. Looser bound (min_pct 70) reflects the wider variance at the largest payload.

**Setup** (preceding non-measured steps):
- `size-128x128-width` (set_control) — 128x128 (16384 lights) — maximum supported size.

**Bounds**:
- FPS ≥ 70% of baseline

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 4.4 | — / 83KB | — / 52KB |
| `esp32-eth` | ≥ 9.1 / 10.5-10.6 | ≥ 122KB / 132KB | ≥ 47KB / 48KB |
| `esp32-eth-wifi` | ≥ 10.0 / 54.5 | ≥ 103KB / 129KB | ≥ 44KB / 52KB |
| `esp32s3-n16r8` | — / 6.1 | — / 8163KB | — / 164KB |
| `pc-macos` | ≥ 8,333 / 4,902-10,204 | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 3,676-4,505 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-02
- `esp32-eth`: contract set 2026-06-02 "anti-regression floor; LUT-fit telemetry baseline" · observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `esp32s3-n16r8`: observed 2026-06-04
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-11
- `pc-windows`: observed 2026-06-07

### scenario_GridLayout_resize

`test/scenarios/light/scenario_GridLayout_resize.json` — Resize the grid while the pipeline is running and verify it reallocates cleanly under memory pressure. Lowers to 128x64 (release memory), increases to 128x128 (heaviest config: mirror + LUT). Each measured step captures tick/FPS/heap so the runner reports the degrade behaviour.

**Mode**: `mutate` · **Also touches**: MultiplyModifier, Layer

#### `size-128x128` (set_control)  📏

Set grid height to 128 (alongside default width 128). Measures the heaviest config as the baseline for the next two steps.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 4.5 | — / 83KB | — / 48KB |
| `esp32-eth` | — / 10.7-10.8 | — / 132KB | — / 48KB-52KB |
| `esp32-eth-wifi` | ≥ 10.0 / 12.4 | ≥ 103KB / 93KB | — / 48KB |
| `pc-macos` | ≥ 8,333 / 3,534-10,526 | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 3,413-4,566 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-03
- `pc-windows`: observed 2026-06-07

#### `shrink-to-128x64` (set_control)  📏

Shrink to 128x64. Measured: FPS must stay within 20% of the baseline (proves the pipeline reallocs cleanly and there's no leak path).

**Bounds**:
- FPS ≥ 80% of baseline

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 11.1 | — / 63KB | — / 17KB |
| `esp32-eth` | — / 26.4-26.5 | — / 114KB | — / 48KB |
| `esp32-eth-wifi` | ≥ 22.2 / 31.8 | ≥ 83KB / 75KB | — / 24KB |
| `pc-macos` | ≥ 16,667 / 4,695-21,739 | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 7,299-10,638 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
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
| `pc-macos` | ≥ 8,333 / 3,257-10,204 | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 3,436-4,608 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-03
- `pc-windows`: observed 2026-06-07

## Layer

### scenario_AllEffects_grid_sizes

`test/scenarios/light/scenario_AllEffects_grid_sizes.json` — Sweep every effect (no modifier) across 16/32/64/128 square grids and measure tick/FPS, free internal heap, max internal block per (effect, size). The scenario prepares its own canvas: clear_children wipes whatever layouts/layers/drivers the device had, then it rebuilds exactly one Layout(Grid) + one Layer + one effect (no modifier) + NetworkSendDriver, so the measurement is each effect's raw cost over the full grid through the real output driver, on any starting device state. PreviewDriver is apparatus (non-deletable) so it survives the clear. Effects are swapped via replace_module at a fixed Layer child slot; grid resized via set_control (width then height, measuring after height so we never measure an N x 128 stripe).

**Mode**: `mutate` · **Also touches**: Layouts, GridLayout, Drivers, NetworkSendDriver, PreviewDriver, LinesEffect, RainbowEffect, NoiseEffect, PlasmaEffect, PlasmaPaletteEffect, MetaballsEffect, FireEffect, ParticlesEffect, GlowParticlesEffect, CheckerboardEffect, SpiralEffect, RingsEffect, LavaLampEffect, GameOfLifeEffect

#### `LinesEffect-16x16` (set_control)  📏

LinesEffect at 16x16 (256 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `shrink-grid-w` (set_control)
- `shrink-grid-h` (set_control)
- `clear-layers` (clear_children)
- `clear-layouts` (clear_children)
- `clear-drivers` (clear_children)
- `build-grid` (add_module)
- `build-layer` (add_module)
- `build-fx` (add_module)
- `build-artnet` (add_module)
- `LinesEffect-pre-w` (set_control)
- `LinesEffect-pre-h` (set_control)
- `fx-LinesEffect` (replace_module)
- `LinesEffect-16x16-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 12,658-14,925 | — / 221KB | — / 108KB |
| `pc-macos` | — / — | — / unlimited | — / unlimited |
| `pc-windows` | — / — | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `LinesEffect-32x32` (set_control)  📏

LinesEffect at 32x32 (1024 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `LinesEffect-32x32-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 7,634-10,000 | — / 215KB-216KB | — / 108KB |
| `pc-macos` | — / — | — / unlimited | — / unlimited |
| `pc-windows` | — / — | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `LinesEffect-64x64` (set_control)  📏

LinesEffect at 64x64 (4096 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `LinesEffect-64x64-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 2,304-2,755 | — / 195KB-198KB | — / 108KB |
| `pc-macos` | — / 1,000,000-— | — / unlimited | — / unlimited |
| `pc-windows` | — / 1,000,000 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `LinesEffect-128x128` (set_control)  📏

LinesEffect at 128x128 (16384 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `LinesEffect-128x128-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 23.3-26.8 | — / 126KB | — / 62KB |
| `pc-macos` | — / 1,000,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 37,037-250,000 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `RainbowEffect-16x16` (set_control)  📏

RainbowEffect at 16x16 (256 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `RainbowEffect-pre-w` (set_control)
- `RainbowEffect-pre-h` (set_control)
- `fx-RainbowEffect` (replace_module)
- `RainbowEffect-16x16-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 3,831-3,937 | — / 173KB | — / 92KB-108KB |
| `pc-macos` | — / 1,000,000-— | — / unlimited | — / unlimited |
| `pc-windows` | — / 250,000-500,000 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-10
- `pc-windows`: observed 2026-06-07

#### `RainbowEffect-32x32` (set_control)  📏

RainbowEffect at 32x32 (1024 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `RainbowEffect-32x32-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 968-1,020 | — / 168KB-171KB | — / 88KB-108KB |
| `pc-macos` | — / 500,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 90,909-166,667 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `RainbowEffect-64x64` (set_control)  📏

RainbowEffect at 64x64 (4096 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `RainbowEffect-64x64-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 143-176 | — / 159KB-162KB | — / 76KB-108KB |
| `pc-macos` | — / 71,429-125,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 34,483-40,000 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-12
- `pc-windows`: observed 2026-06-07

#### `RainbowEffect-128x128` (set_control)  📏

RainbowEffect at 128x128 (16384 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `RainbowEffect-128x128-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 20.2-22.6 | — / 126KB | — / 62KB-108KB |
| `pc-macos` | — / 24,390-28,571 | — / unlimited | — / unlimited |
| `pc-windows` | — / 6,098-8,929 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-12
- `pc-windows`: observed 2026-06-07

#### `NoiseEffect-16x16` (set_control)  📏

NoiseEffect at 16x16 (256 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `NoiseEffect-pre-w` (set_control)
- `NoiseEffect-pre-h` (set_control)
- `fx-NoiseEffect` (replace_module)
- `NoiseEffect-16x16-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 1,117-1,172 | — / 172KB-173KB | — / 92KB-108KB |
| `pc-macos` | — / 250,000-333,333 | — / unlimited | — / unlimited |
| `pc-windows` | — / 83,333-111,111 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `NoiseEffect-32x32` (set_control)  📏

NoiseEffect at 32x32 (1024 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `NoiseEffect-32x32-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 276-324 | — / 168KB-171KB | — / 88KB-108KB |
| `pc-macos` | — / 58,824-71,429 | — / unlimited | — / unlimited |
| `pc-windows` | — / 25,000-29,412 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-10
- `pc-windows`: observed 2026-06-07

#### `NoiseEffect-64x64` (set_control)  📏

NoiseEffect at 64x64 (4096 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `NoiseEffect-64x64-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 45.3-71.8 | — / 159KB-162KB | — / 76KB-108KB |
| `pc-macos` | — / 12,500-15,625 | — / unlimited | — / unlimited |
| `pc-windows` | — / 4,739-6,757 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-11
- `pc-windows`: observed 2026-06-07

#### `NoiseEffect-128x128` (set_control)  📏

NoiseEffect at 128x128 (16384 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `NoiseEffect-128x128-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 12.3-17.1 | — / 126KB | — / 62KB-108KB |
| `pc-macos` | — / 1,357-3,268 | — / unlimited | — / unlimited |
| `pc-windows` | — / 1,190-1,437 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-12
- `pc-windows`: observed 2026-06-07

#### `PlasmaEffect-16x16` (set_control)  📏

PlasmaEffect at 16x16 (256 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `PlasmaEffect-pre-w` (set_control)
- `PlasmaEffect-pre-h` (set_control)
- `fx-PlasmaEffect` (replace_module)
- `PlasmaEffect-16x16-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 3,021-3,195 | — / 173KB-174KB | — / 92KB-108KB |
| `pc-macos` | — / 1,000,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 500,000 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `PlasmaEffect-32x32` (set_control)  📏

PlasmaEffect at 32x32 (1024 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `PlasmaEffect-32x32-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 830-863 | — / 171KB | — / 92KB-108KB |
| `pc-macos` | — / 200,000-333,333 | — / unlimited | — / unlimited |
| `pc-windows` | — / 142,857-166,667 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-12
- `pc-windows`: observed 2026-06-07

#### `PlasmaEffect-64x64` (set_control)  📏

PlasmaEffect at 64x64 (4096 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `PlasmaEffect-64x64-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 135-181 | — / 162KB | — / 84KB-108KB |
| `pc-macos` | — / 62,500-90,909 | — / unlimited | — / unlimited |
| `pc-windows` | — / 35,714-43,478 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-12
- `pc-windows`: observed 2026-06-07

#### `PlasmaEffect-128x128` (set_control)  📏

PlasmaEffect at 128x128 (16384 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `PlasmaEffect-128x128-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 18.3-24.3 | — / 126KB | — / 62KB-108KB |
| `pc-macos` | — / 17,241-22,727 | — / unlimited | — / unlimited |
| `pc-windows` | — / 7,874-9,709 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-10
- `pc-windows`: observed 2026-06-07

#### `PlasmaPaletteEffect-16x16` (set_control)  📏

PlasmaPaletteEffect at 16x16 (256 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `PlasmaPaletteEffect-pre-w` (set_control)
- `PlasmaPaletteEffect-pre-h` (set_control)
- `fx-PlasmaPaletteEffect` (replace_module)
- `PlasmaPaletteEffect-16x16-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 6,024-6,369 | — / 170KB-173KB | — / 92KB-108KB |
| `pc-macos` | — / — | — / unlimited | — / unlimited |
| `pc-windows` | — / 500,000-1,000,000 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `PlasmaPaletteEffect-32x32` (set_control)  📏

PlasmaPaletteEffect at 32x32 (1024 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `PlasmaPaletteEffect-32x32-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 1,733-2,237 | — / 168KB-171KB | — / 88KB-108KB |
| `pc-macos` | — / 500,000-1,000,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 250,000-333,333 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-10
- `pc-windows`: observed 2026-06-07

#### `PlasmaPaletteEffect-64x64` (set_control)  📏

PlasmaPaletteEffect at 64x64 (4096 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `PlasmaPaletteEffect-64x64-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 268-481 | — / 161KB-162KB | — / 80KB-108KB |
| `pc-macos` | — / 142,857-200,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 50,000-71,429 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `PlasmaPaletteEffect-128x128` (set_control)  📏

PlasmaPaletteEffect at 128x128 (16384 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `PlasmaPaletteEffect-128x128-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 21.8-32.1 | — / 126KB | — / 62KB-108KB |
| `pc-macos` | — / 32,258-50,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 12,346-18,868 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-12
- `pc-windows`: observed 2026-06-07

#### `MetaballsEffect-16x16` (set_control)  📏

MetaballsEffect at 16x16 (256 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `MetaballsEffect-pre-w` (set_control)
- `MetaballsEffect-pre-h` (set_control)
- `fx-MetaballsEffect` (replace_module)
- `MetaballsEffect-16x16-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 2,016-2,062 | — / 173KB-174KB | — / 92KB-108KB |
| `pc-macos` | — / 500,000-1,000,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 200,000-250,000 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-12
- `pc-windows`: observed 2026-06-07

#### `MetaballsEffect-32x32` (set_control)  📏

MetaballsEffect at 32x32 (1024 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `MetaballsEffect-32x32-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 522-727 | — / 171KB | — / 92KB-108KB |
| `pc-macos` | — / 200,000-250,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 50,000-62,500 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `MetaballsEffect-64x64` (set_control)  📏

MetaballsEffect at 64x64 (4096 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `MetaballsEffect-64x64-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 89.3-103 | — / 162KB | — / 84KB-108KB |
| `pc-macos` | — / 38,462-62,500 | — / unlimited | — / unlimited |
| `pc-windows` | — / 12,500-15,385 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-12
- `pc-windows`: observed 2026-06-07

#### `MetaballsEffect-128x128` (set_control)  📏

MetaballsEffect at 128x128 (16384 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `MetaballsEffect-128x128-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 16.3-18.7 | — / 126KB | — / 62KB-108KB |
| `pc-macos` | — / 5,263-15,873 | — / unlimited | — / unlimited |
| `pc-windows` | — / 2,786-3,636 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-12
- `pc-windows`: observed 2026-06-07

#### `FireEffect-16x16` (set_control)  📏

FireEffect at 16x16 (256 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `FireEffect-pre-w` (set_control)
- `FireEffect-pre-h` (set_control)
- `fx-FireEffect` (replace_module)
- `FireEffect-16x16-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 2,710-2,762 | — / 173KB | — / 96KB-108KB |
| `pc-macos` | — / 1,000,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 333,333-500,000 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `FireEffect-32x32` (set_control)  📏

FireEffect at 32x32 (1024 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `FireEffect-32x32-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 678-784 | — / 170KB | — / 92KB-108KB |
| `pc-macos` | — / 142,857-333,333 | — / unlimited | — / unlimited |
| `pc-windows` | — / 100,000-125,000 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-12
- `pc-windows`: observed 2026-06-07

#### `FireEffect-64x64` (set_control)  📏

FireEffect at 64x64 (4096 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `FireEffect-64x64-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 122-160 | — / 158KB | — / 76KB-108KB |
| `pc-macos` | — / 27,778-76,923 | — / unlimited | — / unlimited |
| `pc-windows` | — / 27,027-33,333 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-12
- `pc-windows`: observed 2026-06-07

#### `FireEffect-128x128` (set_control)  📏

FireEffect at 128x128 (16384 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `FireEffect-128x128-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 18.1-21.5 | — / 110KB | — / 62KB |
| `pc-macos` | — / 6,803-19,231 | — / unlimited | — / unlimited |
| `pc-windows` | — / 6,452-7,194 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-12
- `pc-windows`: observed 2026-06-07

#### `ParticlesEffect-16x16` (set_control)  📏

ParticlesEffect at 16x16 (256 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `ParticlesEffect-pre-w` (set_control)
- `ParticlesEffect-pre-h` (set_control)
- `fx-ParticlesEffect` (replace_module)
- `ParticlesEffect-16x16-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 4,566-4,717 | — / 172KB | — / 80KB-108KB |
| `pc-macos` | — / 1,000,000-— | — / unlimited | — / unlimited |
| `pc-windows` | — / 500,000 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `ParticlesEffect-32x32` (set_control)  📏

ParticlesEffect at 32x32 (1024 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `ParticlesEffect-32x32-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 1,481-1,848 | — / 168KB | — / 80KB-108KB |
| `pc-macos` | — / 333,333-500,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 166,667-250,000 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `ParticlesEffect-64x64` (set_control)  📏

ParticlesEffect at 64x64 (4096 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `ParticlesEffect-64x64-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 262-425 | — / 150KB | — / 68KB-108KB |
| `pc-macos` | — / 111,111-142,857 | — / unlimited | — / unlimited |
| `pc-windows` | — / 52,632-71,429 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `ParticlesEffect-128x128` (set_control)  📏

ParticlesEffect at 128x128 (16384 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `ParticlesEffect-128x128-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 22.8-30.8 | — / 78KB | — / 34KB-62KB |
| `pc-macos` | — / 27,027-35,714 | — / unlimited | — / unlimited |
| `pc-windows` | — / 12,987-15,873 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-08
- `pc-windows`: observed 2026-06-07

#### `GlowParticlesEffect-16x16` (set_control)  📏

GlowParticlesEffect at 16x16 (256 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `GlowParticlesEffect-pre-w` (set_control)
- `GlowParticlesEffect-pre-h` (set_control)
- `fx-GlowParticlesEffect` (replace_module)
- `GlowParticlesEffect-16x16-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 1,689-1,706 | — / 173KB-174KB | — / 84KB-108KB |
| `pc-macos` | — / 500,000-1,000,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 142,857-166,667 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `GlowParticlesEffect-32x32` (set_control)  📏

GlowParticlesEffect at 32x32 (1024 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `GlowParticlesEffect-32x32-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 411-586 | — / 171KB | — / 84KB-108KB |
| `pc-macos` | — / 52,632-250,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 35,714-45,455 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `GlowParticlesEffect-64x64` (set_control)  📏

GlowParticlesEffect at 64x64 (4096 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `GlowParticlesEffect-64x64-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 77.9-128 | — / 162KB | — / 80KB-108KB |
| `pc-macos` | — / 37,037-55,556 | — / unlimited | — / unlimited |
| `pc-windows` | — / 8,850-10,638 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `GlowParticlesEffect-128x128` (set_control)  📏

GlowParticlesEffect at 128x128 (16384 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `GlowParticlesEffect-128x128-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 14.3-15.0 | — / 126KB | — / 62KB-108KB |
| `pc-macos` | — / 7,752-14,493 | — / unlimited | — / unlimited |
| `pc-windows` | — / 1,949-2,370 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-10
- `pc-windows`: observed 2026-06-07

#### `CheckerboardEffect-16x16` (set_control)  📏

CheckerboardEffect at 16x16 (256 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `CheckerboardEffect-pre-w` (set_control)
- `CheckerboardEffect-pre-h` (set_control)
- `fx-CheckerboardEffect` (replace_module)
- `CheckerboardEffect-16x16-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 8,475-9,009 | — / 173KB | — / 96KB-108KB |
| `pc-macos` | — / — | — / unlimited | — / unlimited |
| `pc-windows` | — / 500,000 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `CheckerboardEffect-32x32` (set_control)  📏

CheckerboardEffect at 32x32 (1024 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `CheckerboardEffect-32x32-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 2,618-2,809 | — / 168KB-171KB | — / 88KB-108KB |
| `pc-macos` | — / 1,000,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 142,857-166,667 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `CheckerboardEffect-64x64` (set_control)  📏

CheckerboardEffect at 64x64 (4096 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `CheckerboardEffect-64x64-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 397-563 | — / 159KB-162KB | — / 72KB-108KB |
| `pc-macos` | — / 200,000-250,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 34,483-45,455 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-08
- `pc-windows`: observed 2026-06-07

#### `CheckerboardEffect-128x128` (set_control)  📏

CheckerboardEffect at 128x128 (16384 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `CheckerboardEffect-128x128-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 21.2-31.5 | — / 123KB-126KB | — / 62KB-108KB |
| `pc-macos` | — / 45,455-62,500 | — / unlimited | — / unlimited |
| `pc-windows` | — / 8,475-10,638 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `SpiralEffect-16x16` (set_control)  📏

SpiralEffect at 16x16 (256 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `SpiralEffect-pre-w` (set_control)
- `SpiralEffect-pre-h` (set_control)
- `fx-SpiralEffect` (replace_module)
- `SpiralEffect-16x16-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 2,404-2,427 | — / 170KB-173KB | — / 88KB-108KB |
| `pc-macos` | — / 1,000,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 250,000-500,000 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `SpiralEffect-32x32` (set_control)  📏

SpiralEffect at 32x32 (1024 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `SpiralEffect-32x32-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 572-691 | — / 170KB-171KB | — / 88KB-108KB |
| `pc-macos` | — / 166,667-250,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 100,000-125,000 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-11
- `pc-windows`: observed 2026-06-07

#### `SpiralEffect-64x64` (set_control)  📏

SpiralEffect at 64x64 (4096 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `SpiralEffect-64x64-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 87.0-140 | — / 161KB-162KB | — / 76KB-108KB |
| `pc-macos` | — / 22,222-62,500 | — / unlimited | — / unlimited |
| `pc-windows` | — / 23,810-27,027 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `SpiralEffect-128x128` (set_control)  📏

SpiralEffect at 128x128 (16384 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `SpiralEffect-128x128-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 15.5-22.7 | — / 123KB-126KB | — / 62KB-108KB |
| `pc-macos` | — / 9,901-13,889 | — / unlimited | — / unlimited |
| `pc-windows` | — / 5,102-6,579 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `RingsEffect-16x16` (set_control)  📏

RingsEffect at 16x16 (256 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `RingsEffect-pre-w` (set_control)
- `RingsEffect-pre-h` (set_control)
- `fx-RingsEffect` (replace_module)
- `RingsEffect-16x16-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 1,119-1,129 | — / 170KB-173KB | — / 92KB-108KB |
| `pc-macos` | — / 250,000-1,000,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 100,000-125,000 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-11
- `pc-windows`: observed 2026-06-07

#### `RingsEffect-32x32` (set_control)  📏

RingsEffect at 32x32 (1024 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `RingsEffect-32x32-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 284-413 | — / 168KB-171KB | — / 88KB-108KB |
| `pc-macos` | — / 83,333-250,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 38,462-47,619 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-10
- `pc-windows`: observed 2026-06-07

#### `RingsEffect-64x64` (set_control)  📏

RingsEffect at 64x64 (4096 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `RingsEffect-64x64-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 45.0-96.6 | — / 161KB-162KB | — / 80KB-108KB |
| `pc-macos` | — / 30,303-58,824 | — / unlimited | — / unlimited |
| `pc-windows` | — / 12,048-15,152 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-10
- `pc-windows`: observed 2026-06-07

#### `RingsEffect-128x128` (set_control)  📏

RingsEffect at 128x128 (16384 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `RingsEffect-128x128-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 12.2-13.0 | — / 125KB-126KB | — / 62KB-108KB |
| `pc-macos` | — / 7,937-13,889 | — / unlimited | — / unlimited |
| `pc-windows` | — / 3,067-3,831 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-12
- `pc-windows`: observed 2026-06-07

#### `LavaLampEffect-16x16` (set_control)  📏

LavaLampEffect at 16x16 (256 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `LavaLampEffect-pre-w` (set_control)
- `LavaLampEffect-pre-h` (set_control)
- `fx-LavaLampEffect` (replace_module)
- `LavaLampEffect-16x16-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 3,030-3,106 | — / 170KB-173KB | — / 92KB-108KB |
| `pc-macos` | — / 1,000,000-— | — / unlimited | — / unlimited |
| `pc-windows` | — / 250,000-500,000 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `LavaLampEffect-32x32` (set_control)  📏

LavaLampEffect at 32x32 (1024 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `LavaLampEffect-32x32-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 756-801 | — / 170KB-171KB | — / 88KB-108KB |
| `pc-macos` | — / 333,333-500,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 66,667-111,111 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `LavaLampEffect-64x64` (set_control)  📏

LavaLampEffect at 64x64 (4096 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `LavaLampEffect-64x64-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 113-136 | — / 161KB-162KB | — / 72KB-108KB |
| `pc-macos` | — / 100,000-142,857 | — / unlimited | — / unlimited |
| `pc-windows` | — / 23,810-29,412 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-08
- `pc-windows`: observed 2026-06-07

#### `LavaLampEffect-128x128` (set_control)  📏

LavaLampEffect at 128x128 (16384 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `LavaLampEffect-128x128-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 18.2-19.0 | — / 125KB-126KB | — / 62KB-108KB |
| `pc-macos` | — / 22,222-33,333 | — / unlimited | — / unlimited |
| `pc-windows` | — / 4,926-6,757 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-11
- `pc-windows`: observed 2026-06-07

#### `GameOfLifeEffect-16x16` (set_control)  📏

GameOfLifeEffect at 16x16 (256 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `GameOfLifeEffect-pre-w` (set_control)
- `GameOfLifeEffect-pre-h` (set_control)
- `fx-GameOfLifeEffect` (replace_module)
- `GameOfLifeEffect-16x16-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 6,803-7,092 | — / 171KB-173KB | — / 88KB-108KB |
| `pc-macos` | — / — | — / unlimited | — / unlimited |
| `pc-windows` | — / 500,000-1,000,000 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `GameOfLifeEffect-32x32` (set_control)  📏

GameOfLifeEffect at 32x32 (1024 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `GameOfLifeEffect-32x32-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 1,520-1,653 | — / 166KB-169KB | — / 84KB-108KB |
| `pc-macos` | — / 333,333-1,000,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 166,667-200,000 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-11
- `pc-windows`: observed 2026-06-07

#### `GameOfLifeEffect-64x64` (set_control)  📏

GameOfLifeEffect at 64x64 (4096 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `GameOfLifeEffect-64x64-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 227-235 | — / 151KB-154KB | — / 68KB-108KB |
| `pc-macos` | — / 142,857-200,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 38,462-47,619 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-11
- `pc-windows`: observed 2026-06-07

#### `GameOfLifeEffect-128x128` (set_control)  📏

GameOfLifeEffect at 128x128 (16384 lights) — measure tick/FPS, free internal heap, max internal block.

**Setup** (preceding non-measured steps):
- `GameOfLifeEffect-128x128-w` (set_control)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 11.5-13.9 | — / 91KB-94KB | — / 46KB-62KB |
| `pc-macos` | — / 4,975-28,571 | — / unlimited | — / unlimited |
| `pc-windows` | — / 8,696-9,174 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-11
- `pc-windows`: observed 2026-06-07

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
| `pc-macos` | ≥ 20,000 / 7,576-— | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 7,874-8,475 | — / unlimited | — / unlimited |

- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-05
- `pc-windows`: observed 2026-06-07

### scenario_Layer_buildup

`test/scenarios/light/scenario_Layer_buildup.json` — Start empty, add modules step by step, measure tick + heap after each meaningful pipeline state. Surfaces 'how much does each module cost?' so a regression in any one module shows up as a per-step delta instead of a single end-to-end number. Heap bounds catch unintended allocations: each step's delta vs the previous step is asserted against max_delta_bytes (only meaningful on ESP32 where freeHeap() returns a real value).

**Mode**: `construct` · **Also touches**: Layouts, GridLayout, RainbowEffect, MultiplyModifier, Drivers, NetworkSendDriver

#### `measure-minimum` (measure)  📏

Baseline: 16x16 grid + Rainbow only. No Drivers yet (Layer renders into its own buffer). No fps floor asserted — a 16x16 grid renders in <1us on desktop, flooring the integer-us tick (and thus FPS) to 0; the per-target tick contract is the meaningful check here (heap deltas are asserted on the later buildup steps that add Drivers/LUT).

**Setup** (preceding non-measured steps):
- `add-layout-group` (add_module) — Top-level Layouts container — no children yet, no lights, no buffer.
- `add-grid-16` (add_module) — 16x16 grid under Layouts. Smallest realistic display.
- `add-layer` (add_module) — Layer wired to Layouts (RGB, 3 channels per light).
- `add-rainbow` (add_module) — RainbowEffect as the only effect. Renderable from this point on.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | ≥ 20,000 / 8,197-— | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 333,333-1,000,000 | — / unlimited | — / unlimited |

- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-05
- `pc-windows`: observed 2026-06-07

#### `measure-full-16x16` (measure)  📏

Full pipeline at 16x16. Heap delta vs previous measure-minimum step should stay within +8KB on ESP32 (Drivers + ArtNet overhead, no LUT yet). No fps floor — 16x16 ticks below the host's microsecond resolution on desktop; heap delta is the check here.

**Setup** (preceding non-measured steps):
- `add-drivers` (add_module) — Drivers container wired to the Layer.
- `add-artnet` (add_module) — NetworkSendDriver under Drivers. Full pipeline now end-to-end.

**Bounds**:
- heap growth ≤ 8192B vs previous measure step

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | ≥ 20,000 / 5,464-— | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 200,000-500,000 | — / unlimited | — / unlimited |

- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-05
- `pc-windows`: observed 2026-06-07

#### `measure-with-lut-16x16` (measure)  📏

Mirror is on: Layer has a LUT, Drivers has an output buffer. min_fps_led_product asserts the throughput floor scales correctly to the logical grid size (post-mirror).

**Setup** (preceding non-measured steps):
- `add-mirror` (add_module) — MultiplyModifier under Layer. Triggers a LUT build + Drivers output buffer allocation (the heavy memory path).

**Bounds**:
- FPS × lights ≥ 100,000

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | ≥ 16,667 / 6,667-— | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 333,333-1,000,000 | — / unlimited | — / unlimited |

- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-05
- `pc-windows`: observed 2026-06-07

#### `measure-full-128x128` (measure)  📏

Production-size grid with the full pipeline. Final tick + cumulative heap delta — the line you compare against future commits to catch regressions across the whole pipeline.

**Setup** (preceding non-measured steps):
- `grow-to-128x128-width` (set_control) — Grow the grid: 128 wide.
- `grow-to-128x128-height` (set_control) — Grow the grid: 128 tall. Layer reallocates buffer; with mirror on, LUT also grows. Heap delta caught by max_delta_bytes.

**Bounds**:
- FPS ≥ 1 (absolute)
- heap growth ≤ 1048576B vs previous measure step

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | ≥ 16,667 / 5,882-23,256 | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 10,000-13,158 | — / unlimited | — / unlimited |

- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-03
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
| `esp32-eth` | — / 1,580-7,752 | — / 172KB-225KB | — / 76KB-108KB |
| `pc-macos` | — / 76,923-166,667 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-11

#### `checkerboard` (measure)  📏

Checkerboard modifier active — masks half the lights; pipeline stays live (driver buffer non-null).

**Setup** (preceding non-measured steps):
- `swap-to-checker` (replace_module)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 769-990 | — / 170KB-225KB | — / 76KB-108KB |
| `pc-macos` | — / 17,544-58,824 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-11

#### `multiply-2` (measure)  📏

Back to Multiply — replace round-trips cleanly, pipeline live again.

**Setup** (preceding non-measured steps):
- `swap-to-multiply` (replace_module)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32-eth` | — / 1,587-2,278 | — / 169KB-225KB | — / 76KB-108KB |
| `pc-macos` | — / 66,667-166,667 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-07 → 2026-06-08
- `pc-macos`: observed 2026-06-07 → 2026-06-11

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
| `pc-macos` | — / 29,412-125,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 32,258-37,037 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-08
- `pc-macos`: observed 2026-06-05
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
| `pc-macos` | — / 21,739-111,111 | — / unlimited | — / unlimited |
| `pc-windows` | — / 16,393-23,810 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-08
- `pc-macos`: observed 2026-06-05 → 2026-06-11
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
| `pc-macos` | — / 5,747-100,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 5,848-9,009 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-08
- `pc-macos`: observed 2026-06-05 → 2026-06-11
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
| `pc-macos` | — / 16,949-125,000 | — / unlimited | — / unlimited |
| `pc-windows` | — / 33,333-38,462 | — / unlimited | — / unlimited |

- `esp32-eth`: observed 2026-06-08
- `pc-macos`: observed 2026-06-05
- `pc-windows`: observed 2026-06-07

## MoonModule

### scenario_MoonModule_control_change

`test/scenarios/core/scenario_MoonModule_control_change.json` — Measure the cost of control changes on a running pipeline. Toggles MultiplyModifier's mirrorX/Y at different points and verifies each change is applied without freezing the render loop. Companion to the MoonModule control-change gate unit tests (unit_MoonModule_control_change_gate.cpp) — this is the live equivalent.

**Mode**: `mutate` · **Also touches**: MultiplyModifier, NoiseEffect

#### `baseline` (set_control)  📏

Set NoiseEffect.scale=4 and measure baseline FPS (mirror on). Effect controls don't rebuild the pipeline — slider stutter check.

**Bounds**:
- FPS ≥ 80% of baseline

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 3.9 | — / 88KB | — / 48KB |
| `esp32-eth` | — / 10.5-10.6 | — / 133KB | — / 48KB-50KB |
| `esp32-eth-wifi` | ≥ 10.0 / 12.2 | ≥ 103KB / 94KB | — / 48KB |
| `pc-macos` | ≥ 8,333 / 4,505-10,309 | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 4,000-4,405 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-07
- `pc-windows`: observed 2026-06-07

#### `disable-mirrorX` (set_control)  📏

Disable mirrorX. Modifier control triggers a pipeline rebuild — measures the rebuilt path.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 4.8 | — / 88KB | — / 48KB |
| `esp32-eth` | — / 10.4 | — / 132KB | — / 48KB-50KB |
| `esp32-eth-wifi` | ≥ 10.0 / 12.0 | ≥ 103KB / 94KB | — / 48KB |
| `pc-macos` | ≥ 5,000 / 3,636-9,174 | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 2,024-2,392 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-08
- `pc-windows`: observed 2026-06-07

#### `disable-mirrorY` (set_control)  📏

Disable mirrorY. Mirror is now fully off — should land on the no-LUT path.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 4.4 | — / 88KB | — / 48KB |
| `esp32-eth` | — / 8.9-9.0 | — / 132KB | — / 48KB-50KB |
| `esp32-eth-wifi` | ≥ 10.0 / 11.1 | ≥ 103KB / 94KB | — / 48KB |
| `pc-macos` | ≥ 2,500 / 1,916-9,091 | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 1,082-1,305 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-10
- `pc-windows`: observed 2026-06-07

#### `re-enable-mirrorY` (set_control)  📏

Re-enable mirrorY and measure — the heavy LUT path must recover (FPS within 50% of baseline) without staying degraded.

**Setup** (preceding non-measured steps):
- `re-enable-mirrors` (set_control) — Re-enable mirrorX (rebuild back to LUT path).

**Bounds**:
- FPS ≥ 50% of baseline

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 4.4 | — / 88KB | — / 48KB |
| `esp32-eth` | — / 10.5-10.6 | — / 132KB | — / 48KB-50KB |
| `esp32-eth-wifi` | ≥ 10.0 / 12.1 | ≥ 103KB / 94KB | — / 48KB |
| `pc-macos` | ≥ 8,333 / 5,348-10,417 | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 4,065-4,854 | — / unlimited | — / unlimited |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-03
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
| `pc-macos` | ≥ 8,333 / 4,065-1,000,000 | unlimited / unlimited | — / unlimited |
| `pc-windows` | — / 3,953-4,444 | — / unlimited | — / unlimited |

- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02 → 2026-06-05
- `pc-windows`: observed 2026-06-07

## NetworkModule

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

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

#### `mdns-off` (set_control)  📏

mDNS off — measured. Expected to match or exceed the baseline.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 3.6 | — / 88KB | — / 48KB |
| `esp32-eth` | — / 10.3-10.5 | — / 137KB | — / 48KB-52KB |
| `esp32-eth-wifi` | ≥ 10.0 / 12.0 | ≥ 93KB / 98KB | — / 48KB |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "shared heap budget; cumulative sweep state reduces standalone-mDNS-off heap by ~15KB" · observed 2026-06-02

#### `mdns-on-again` (set_control)  📏

mDNS on again — measured with a bound: FPS must stay within 20% of the baseline (proves toggling doesn't leave the network task in a degraded state).

**Bounds**:
- FPS ≥ 80% of baseline

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 4.3 | — / 83KB | — / 48KB |
| `esp32-eth` | — / 9.1 | — / 132KB | — / 48KB-52KB |
| `esp32-eth-wifi` | ≥ 10.0 / 10.6 | ≥ 103KB / 93KB | — / 48KB |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
