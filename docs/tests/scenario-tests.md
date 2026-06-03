# Scenario Tests

Auto-generated from `test/scenarios/{core,light}/scenario_*.json` by `scripts/docs/generate_test_docs.py`. **Do not edit by hand** — update the JSON file's top-level fields and per-step `description` / `bounds` / `contract` / `observed` instead, then regenerate.

Scenario tests are the integration tier in the [test strategy](../testing.md): each one is a JSON script that drives the full pipeline (PC or live ESP32) and captures tick / heap per step against per-target contracts. Run them with `scripts/scenario/run_scenario.py` (PC) or `scripts/scenario/run_live_scenario.py` (live device). See [testing.md § Performance contracts](../testing.md#performance-contracts-contracttarget) for the contract semantics.

## GridLayout

### scenario_GridLayout_grid_sizes

`test/scenarios/light/scenario_GridLayout_grid_sizes.json` — Walk the grid through 16x16 → 32x32 → 64x64 → 128x128 and assert a per-size FPS floor.

**Mode**: `mutate` · **Also touches**: Layer, MirrorModifier, NoiseEffect, Drivers, ArtNetSendDriver, PreviewDriver

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
| `pc-macos` | ≥ 200,000 / 200,000-1,000,000 | — / — | — / — |

- `esp32`: observed 2026-06-02
- `esp32-eth`: contract set 2026-06-02 "anti-regression floor; LUT-fit telemetry baseline" · observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

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
| `pc-macos` | ≥ 100,000 / 142,857-166,667 | — / — | — / — |

- `esp32`: observed 2026-06-02
- `esp32-eth`: contract set 2026-06-02 "anti-regression floor; LUT-fit telemetry baseline" · observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

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
| `pc-macos` | ≥ 33,333 / 35,714-43,478 | — / — | — / — |

- `esp32`: observed 2026-06-02
- `esp32-eth`: contract set 2026-06-02 "anti-regression floor; LUT-fit telemetry baseline" · observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

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
| `pc-macos` | ≥ 8,333 / 8,197-10,000 | — / — | — / — |

- `esp32`: observed 2026-06-02
- `esp32-eth`: contract set 2026-06-02 "anti-regression floor; LUT-fit telemetry baseline" · observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

### scenario_GridLayout_resize

`test/scenarios/light/scenario_GridLayout_resize.json` — Resize the grid while the pipeline is running and verify it reallocates cleanly under memory pressure. Lowers to 128x64 (release memory), increases to 128x128 (heaviest config: mirror + LUT). Each measured step captures tick/FPS/heap so the runner reports the degrade behaviour.

**Mode**: `mutate` · **Also touches**: MirrorModifier, Layer

#### `size-128x128` (set_control)  📏

Set grid height to 128 (alongside default width 128). Measures the heaviest config as the baseline for the next two steps.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 4.5 | — / 83KB | — / 48KB |
| `esp32-eth` | — / 10.7-10.8 | — / 132KB | — / 48KB-52KB |
| `esp32-eth-wifi` | ≥ 10.0 / 12.4 | ≥ 103KB / 93KB | — / 48KB |
| `pc-macos` | ≥ 8,333 / 3,534-9,901 | — / — | — / — |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

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
| `pc-macos` | ≥ 16,667 / 5,208-21,277 | — / — | — / — |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

#### `grow-to-128x128` (set_control)  📏

Grow back to 128x128. Measured: confirms the heap can return to the heavy baseline after a shrink.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 4.0 | — / 83KB | — / 52KB |
| `esp32-eth` | — / 10.4 | — / 132KB | — / 48KB |
| `esp32-eth-wifi` | ≥ 10.0 / 12.2 | ≥ 103KB / 93KB | — / 52KB |
| `pc-macos` | ≥ 8,333 / 4,237-9,901 | — / — | — / — |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

## Layer

### scenario_Layer_base_pipeline

`test/scenarios/light/scenario_Layer_base_pipeline.json` — Core pipeline: build Layouts→Grid→Layer→RainbowEffect→Drivers→ArtNetSendDriver from scratch and verify each module wires correctly. Drives the bounded FPS check at the end so a render-path regression is caught.

**Mode**: `construct` · **Also touches**: GridLayout, RainbowEffect, Drivers, ArtNetSendDriver

#### `add-artnet` (add_module)  📏

Add ArtNetSendDriver and run the bounded FPS measurement (must stay at >=80% of the rated FPS for grid size 16x16).

**Setup** (preceding non-measured steps):
- `add-layout-group` (add_module) — Create the top-level Layouts container.
- `add-grid` (add_module) — Add a GridLayout child to Layouts (default 16x16x1).
- `add-layer` (add_module) — Add a top-level Layer wired to the Layouts container, RGB (3 channels per light).
- `add-rainbow` (add_module) — Add RainbowEffect as the Layer's only effect.
- `add-driver-group` (add_module) — Add a top-level Drivers container wired to the Layer's output buffer.

**Bounds**:
- FPS ≥ 80% of baseline
- FPS × lights ≥ 294,912

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | ≥ 20,000 / 20,408-28,571 | — / — | — / — |

- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

### scenario_Layer_buildup

`test/scenarios/light/scenario_Layer_buildup.json` — Start empty, add modules step by step, measure tick + heap after each meaningful pipeline state. Surfaces 'how much does each module cost?' so a regression in any one module shows up as a per-step delta instead of a single end-to-end number. Heap bounds catch unintended allocations: each step's delta vs the previous step is asserted against max_delta_bytes (only meaningful on ESP32 where freeHeap() returns a real value).

**Mode**: `construct` · **Also touches**: Layouts, GridLayout, RainbowEffect, MirrorModifier, Drivers, ArtNetSendDriver

#### `measure-minimum` (measure)  📏

Baseline: 16x16 grid + Rainbow only. No Drivers yet (Layer renders into its own buffer).

**Setup** (preceding non-measured steps):
- `add-layout-group` (add_module) — Top-level Layouts container — no children yet, no lights, no buffer.
- `add-grid-16` (add_module) — 16x16 grid under Layouts. Smallest realistic display.
- `add-layer` (add_module) — Layer wired to Layouts (RGB, 3 channels per light).
- `add-rainbow` (add_module) — RainbowEffect as the only effect. Renderable from this point on.

**Bounds**:
- FPS ≥ 1 (absolute)

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | ≥ 20,000 / 8,197-28,571 | — / — | — / — |

- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

#### `measure-full-16x16` (measure)  📏

Full pipeline at 16x16. Heap delta vs previous measure-minimum step should stay within +8KB on ESP32 (Drivers + ArtNet overhead, no LUT yet).

**Setup** (preceding non-measured steps):
- `add-drivers` (add_module) — Drivers container wired to the Layer.
- `add-artnet` (add_module) — ArtNetSendDriver under Drivers. Full pipeline now end-to-end.

**Bounds**:
- FPS ≥ 1 (absolute)
- heap growth ≤ 8192B vs previous measure step

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | ≥ 20,000 / 5,464-28,571 | — / — | — / — |

- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

#### `measure-with-lut-16x16` (measure)  📏

Mirror is on: Layer has a LUT, Drivers has an output buffer. min_fps_led_product asserts the throughput floor scales correctly to the logical grid size (post-mirror).

**Setup** (preceding non-measured steps):
- `add-mirror` (add_module) — MirrorModifier under Layer. Triggers a LUT build + Drivers output buffer allocation (the heavy memory path).

**Bounds**:
- FPS × lights ≥ 100,000

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | ≥ 16,667 / 7,407-22,222 | — / — | — / — |

- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

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
| `pc-macos` | ≥ 16,667 / 18,868-22,727 | — / — | — / — |

- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

### scenario_Layer_memory_1to1

`test/scenarios/light/scenario_Layer_memory_1to1.json` — Verify that an unshuffled 1:1 mapping (no modifier) uses no LUT and no driver buffer. Catches a regression where Layer would allocate a passthrough LUT for the identity case.

**Mode**: `construct` · **Also touches**: MappingLUT, BlendMap

#### `add-artnet` (add_module)  📏

Add ArtNetSendDriver and run the bounded FPS measurement on the no-LUT path.

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
| `pc-macos` | ≥ 20,000 / 12,500-28,571 | — / — | — / — |

- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

## MirrorModifier

### scenario_MirrorModifier_memory_lut

`test/scenarios/light/scenario_MirrorModifier_memory_lut.json` — Verify that adding a MirrorModifier allocates both the mapping LUT and the driver buffer (the heavy memory path). Companion to scenario_Layer_memory_1to1, which verifies the no-LUT path.

**Mode**: `construct` · **Also touches**: Layer, MappingLUT, BlendMap

#### `add-artnet` (add_module)  📏

Add ArtNetSendDriver and run the bounded FPS measurement on the LUT path.

**Setup** (preceding non-measured steps):
- `add-layout-group` (add_module) — Create the top-level Layouts container.
- `add-grid` (add_module) — Add a 16x16 GridLayout.
- `add-layer` (add_module) — Add a Layer wired to Layouts (RGB).
- `add-noise` (add_module) — Add NoiseEffect as the Layer's effect.
- `add-mirror` (add_module) — Add MirrorModifier — triggers LUT and driver-buffer allocation.
- `add-driver-group` (add_module) — Add a Drivers container wired to the Layer.

**Bounds**:
- FPS ≥ 80% of baseline

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | ≥ 8,333 / 3,322-9,901 | — / — | — / — |

- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

### scenario_MirrorModifier_pipeline

`test/scenarios/light/scenario_MirrorModifier_pipeline.json` — Pipeline with a mirror modifier: NoiseEffect renders one quadrant, MirrorModifier reflects across X and Y to produce a kaleidoscope. Used to verify the MirrorModifier wires into Layer cleanly and that the full pipeline still meets its FPS bound.

**Mode**: `construct` · **Also touches**: Layer, NoiseEffect, ArtNetSendDriver

#### `add-artnet` (add_module)  📏

Add ArtNetSendDriver and run the bounded FPS measurement (mirror + LUT path must stay at >=80% of the rated FPS).

**Setup** (preceding non-measured steps):
- `add-layout-group` (add_module) — Create the top-level Layouts container.
- `add-grid` (add_module) — Add a GridLayout child to Layouts.
- `add-layer` (add_module) — Add a Layer wired to Layouts (RGB).
- `add-noise` (add_module) — Add NoiseEffect as the Layer's effect.
- `add-mirror` (add_module) — Add MirrorModifier so logical pixels reflect across X and Y in the physical grid.
- `add-driver-group` (add_module) — Add a Drivers container wired to the Layer's output buffer.

**Bounds**:
- FPS ≥ 80% of baseline

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `pc-macos` | ≥ 8,333 / 8,850-9,901 | — / — | — / — |

- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

## MoonModule

### scenario_MoonModule_control_change

`test/scenarios/core/scenario_MoonModule_control_change.json` — Measure the cost of control changes on a running pipeline. Toggles MirrorModifier's mirrorX/Y at different points and verifies each change is applied without freezing the render loop. Companion to the MoonModule control-change gate unit tests (unit_MoonModule_control_change_gate.cpp) — this is the live equivalent.

**Mode**: `mutate` · **Also touches**: MirrorModifier, NoiseEffect

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
| `pc-macos` | ≥ 8,333 / 5,952-9,901 | — / — | — / — |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

#### `disable-mirrorX` (set_control)  📏

Disable mirrorX. Modifier control triggers a pipeline rebuild — measures the rebuilt path.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 4.8 | — / 88KB | — / 48KB |
| `esp32-eth` | — / 10.4 | — / 132KB | — / 48KB-50KB |
| `esp32-eth-wifi` | ≥ 10.0 / 12.0 | ≥ 103KB / 94KB | — / 48KB |
| `pc-macos` | ≥ 5,000 / 4,184-5,405 | — / — | — / — |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

#### `disable-mirrorY` (set_control)  📏

Disable mirrorY. Mirror is now fully off — should land on the no-LUT path.

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 4.4 | — / 88KB | — / 48KB |
| `esp32-eth` | — / 8.9-9.0 | — / 132KB | — / 48KB-50KB |
| `esp32-eth-wifi` | ≥ 10.0 / 11.1 | ≥ 103KB / 94KB | — / 48KB |
| `pc-macos` | ≥ 2,500 / 2,532-2,849 | — / — | — / — |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

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
| `pc-macos` | ≥ 8,333 / 9,009-10,000 | — / — | — / — |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

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

## PreviewDriver

### scenario_PreviewDriver_detail

`test/scenarios/light/scenario_PreviewDriver_detail.json` — Toggle the Preview driver's detail and decompress controls and measure the render-FPS impact. detail 2/3 have a known, accepted downsample cost on the render task; decompress is purely client-side and cannot affect the render tick (see performance.md). All steps assert a relative bound (min_pct) only — a single ESP32 scenario step swings too much for an absolute FPS floor to be meaningful (the absolute throughput floor is enforced in collect_kpi.py --commit, which uses a settled reading). detail 3 gets a looser bound because its downsample cost is real and accepted.

**Mode**: `mutate`

#### `detail-1-coarse` (set_control)  📏

detail=1 (coarsest, 16x16 downsample on a 128 grid). Cheapest preview render.

**Bounds**:
- FPS ≥ 80% of baseline

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 4.0 | — / 83KB | — / 48KB |
| `esp32-eth` | — / 10.5-10.6 | — / 132KB | — / 50KB-52KB |
| `esp32-eth-wifi` | ≥ 10.0 / 12.2 | ≥ 103KB / 93KB | — / 48KB |
| `pc-macos` | ≥ 3,333 / 2,070-3,322 | — / — | — / — |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

#### `detail-2-medium` (set_control)  📏

detail=2 (medium, 32x32 downsample). Known accepted cost — still hits 80% of baseline.

**Bounds**:
- FPS ≥ 80% of baseline

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 4.9 | — / 83KB | — / 48KB |
| `esp32-eth` | — / 10.4-10.5 | — / 132KB | — / 50KB-52KB |
| `esp32-eth-wifi` | ≥ 10.0 / 12.0 | ≥ 103KB / 93KB | — / 48KB |
| `pc-macos` | ≥ 3,333 / 3,003-3,289 | — / — | — / — |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

#### `detail-3-fine` (set_control)  📏

detail=3 (finest, 43x43 downsample). Looser bound (70%) because the downsample cost is real and accepted.

**Bounds**:
- FPS ≥ 70% of baseline

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 4.5 | — / 83KB | — / 48KB |
| `esp32-eth` | — / 9.5-9.6 | — / 132KB | — / 50KB-52KB |
| `esp32-eth-wifi` | ≥ 10.0 / 12.6 | ≥ 103KB / 93KB | — / 48KB |
| `pc-macos` | ≥ 3,125 / 2,967-3,333 | — / — | — / — |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

#### `decompress-on` (set_control)  📏

decompress=true. Client-side hint — does not affect the render tick.

**Bounds**:
- FPS ≥ 80% of baseline

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 4.6 | — / 83KB | — / 48KB |
| `esp32-eth` | — / 10.5-10.6 | — / 132KB | — / 50KB-52KB |
| `esp32-eth-wifi` | ≥ 10.0 / 12.2 | ≥ 103KB / 93KB | — / 48KB |
| `pc-macos` | ≥ 3,333 / 3,040-3,333 | — / — | — / — |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02

#### `decompress-off` (set_control)  📏

decompress=false. Same as above — pure client-side, no render impact expected.

**Bounds**:
- FPS ≥ 80% of baseline

**Performance** (contract / observed) — tick stored, FPS shown:

| Board | FPS | heap | block |
|---|---|---|---|
| `esp32` | — / 4.8 | — / 83KB | — / 48KB |
| `esp32-eth` | — / 10.3 | — / 132KB | — / 50KB-52KB |
| `esp32-eth-wifi` | ≥ 10.0 / 12.0 | ≥ 103KB / 93KB | — / 48KB |
| `pc-macos` | ≥ 3,333 / 2,985-3,311 | — / — | — / — |

- `esp32`: observed 2026-06-02
- `esp32-eth`: observed 2026-06-02
- `esp32-eth-wifi`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
- `pc-macos`: contract set 2026-06-02 "initial contract" · observed 2026-06-02
