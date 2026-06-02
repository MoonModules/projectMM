# Scenario Tests

Auto-generated from `test/scenarios/{core,light}/scenario_*.json` by `scripts/docs/generate_test_docs.py`. **Do not edit by hand** — update the JSON file's top-level `module` / `also` / `description` and per-step `description` fields instead, then regenerate.

Scenario tests are the integration tier in the [test strategy](../testing.md): each one is a JSON script that drives the full pipeline (PC or live ESP32) and captures bounded FPS / heap measurements per step. Run them with `scripts/scenario/run_scenario.py` (PC) or `scripts/scenario/run_live_scenario.py` (live device).

## GridLayout

### scenario_GridLayout_resize

`test/scenarios/light/scenario_GridLayout_resize.json` — Resize the grid via REST while the pipeline is running and verify it reallocates cleanly under memory pressure. Lowers to 128x64 (release memory), increases to 128x128 (heaviest config: mirror + LUT). Each measured step captures tick/FPS/heap so the live runner reports the degrade behaviour.

*Also touches: MirrorModifier, Layer.*

- **reset-mirrorX** (set_control) — Make sure the mirror is enabled on X before measuring (the test assumes the heaviest LUT path).
- **reset-mirrorY** (set_control) — Make sure the mirror is enabled on Y before measuring.
- **size-128x128** (set_control) — Set grid height to 128 (alongside default width 128). Measures the heaviest config as the baseline for the next two steps.
- **shrink-to-128x64** (set_control) — Shrink to 128x64. Measured: FPS must stay within 20% of the baseline (proves the pipeline reallocs cleanly and there's no leak path).
- **grow-to-128x128** (set_control) — Grow back to 128x128. Measured: confirms the heap can return to the heavy baseline after a shrink.

## Layer

### scenario_Layer_base_pipeline

`test/scenarios/light/scenario_Layer_base_pipeline.json` — Core pipeline: build Layouts→Grid→Layer→RainbowEffect→Drivers→ArtNetSendDriver from scratch and verify each module wires correctly. Drives the bounded FPS check at the end so a render-path regression is caught.

*Also touches: GridLayout, RainbowEffect, Drivers, ArtNetSendDriver.*

- **add-layout-group** (add_module) — Create the top-level Layouts container.
- **add-grid** (add_module) — Add a GridLayout child to Layouts (default 16x16x1).
- **add-layer** (add_module) — Add a top-level Layer wired to the Layouts container, RGB (3 channels per light).
- **add-rainbow** (add_module) — Add RainbowEffect as the Layer's only effect.
- **add-driver-group** (add_module) — Add a top-level Drivers container wired to the Layer's output buffer.
- **add-artnet** (add_module) — Add ArtNetSendDriver and run the bounded FPS measurement (must stay at >=80% of the rated FPS for grid size 16x16).

### scenario_Layer_memory_1to1

`test/scenarios/light/scenario_Layer_memory_1to1.json` — Verify that an unshuffled 1:1 mapping (no modifier) uses no LUT and no driver buffer. Catches a regression where Layer would allocate a passthrough LUT for the identity case.

*Also touches: MappingLUT, BlendMap.*

- **add-layout-group** (add_module) — Create the top-level Layouts container.
- **add-grid** (add_module) — Add a 16x16 GridLayout.
- **add-layer** (add_module) — Add a Layer wired to Layouts (RGB).
- **add-rainbow** (add_module) — Add RainbowEffect as the Layer's effect.
- **add-driver-group** (add_module) — Add a Drivers container wired to the Layer.
- **add-artnet** (add_module) — Add ArtNetSendDriver and run the bounded FPS measurement on the no-LUT path.

## MirrorModifier

### scenario_MirrorModifier_memory_lut

`test/scenarios/light/scenario_MirrorModifier_memory_lut.json` — Verify that adding a MirrorModifier allocates both the mapping LUT and the driver buffer (the heavy memory path). Companion to scenario_Layer_memory_1to1, which verifies the no-LUT path.

*Also touches: Layer, MappingLUT, BlendMap.*

- **add-layout-group** (add_module) — Create the top-level Layouts container.
- **add-grid** (add_module) — Add a 16x16 GridLayout.
- **add-layer** (add_module) — Add a Layer wired to Layouts (RGB).
- **add-noise** (add_module) — Add NoiseEffect as the Layer's effect.
- **add-mirror** (add_module) — Add MirrorModifier — triggers LUT and driver-buffer allocation.
- **add-driver-group** (add_module) — Add a Drivers container wired to the Layer.
- **add-artnet** (add_module) — Add ArtNetSendDriver and run the bounded FPS measurement on the LUT path.

### scenario_MirrorModifier_pipeline

`test/scenarios/light/scenario_MirrorModifier_pipeline.json` — Pipeline with a mirror modifier: NoiseEffect renders one quadrant, MirrorModifier reflects across X and Y to produce a kaleidoscope. Used to verify the MirrorModifier wires into Layer cleanly and that the full pipeline still meets its FPS bound.

*Also touches: Layer, NoiseEffect, ArtNetSendDriver.*

- **add-layout-group** (add_module) — Create the top-level Layouts container.
- **add-grid** (add_module) — Add a GridLayout child to Layouts.
- **add-layer** (add_module) — Add a Layer wired to Layouts (RGB).
- **add-noise** (add_module) — Add NoiseEffect as the Layer's effect.
- **add-mirror** (add_module) — Add MirrorModifier so logical pixels reflect across X and Y in the physical grid.
- **add-driver-group** (add_module) — Add a Drivers container wired to the Layer's output buffer.
- **add-artnet** (add_module) — Add ArtNetSendDriver and run the bounded FPS measurement (mirror + LUT path must stay at >=80% of the rated FPS).

## MoonModule

### scenario_MoonModule_control_change

`test/scenarios/core/scenario_MoonModule_control_change.json` — Measure the cost of control changes on a running pipeline. Toggles MirrorModifier's mirrorX/Y at different points and verifies each change is applied without freezing the render loop. Companion to the MoonModule control-change gate unit tests (unit_MoonModule_control_change_gate.cpp) — this is the live equivalent.

*Also touches: MirrorModifier, NoiseEffect.*

- **reset-mirrorX** (set_control) — Ensure mirrorX is enabled before baseline.
- **reset-mirrorY** (set_control) — Ensure mirrorY is enabled before baseline.
- **baseline** (set_control) — Set NoiseEffect.scale=4 and measure baseline FPS (mirror on). Effect controls don't rebuild the pipeline — slider stutter check.
- **disable-mirrorX** (set_control) — Disable mirrorX. Modifier control triggers a pipeline rebuild — measures the rebuilt path.
- **disable-mirrorY** (set_control) — Disable mirrorY. Mirror is now fully off — should land on the no-LUT path.
- **re-enable-mirrors** (set_control) — Re-enable mirrorX (rebuild back to LUT path).
- **re-enable-mirrorY** (set_control) — Re-enable mirrorY and measure — the heavy LUT path must recover (FPS within 50% of baseline) without staying degraded.

## NetworkModule

### scenario_NetworkModule_mdns_toggle

`test/scenarios/core/scenario_NetworkModule_mdns_toggle.json` — Toggle the mDNS responder on and off and measure render-FPS impact. Validates that mDNS announcement traffic doesn't degrade the render loop more than 20% on the busiest tick.

- **baseline-mdns-on** (set_control) — mDNS on (default) — captures the baseline FPS for the next two steps.
- **mdns-off** (set_control) — mDNS off — measured. Expected to match or exceed the baseline.
- **mdns-on-again** (set_control) — mDNS on again — measured with a bound: FPS must stay within 20% of the baseline (proves toggling doesn't leave the network task in a degraded state).

## PreviewDriver

### scenario_PreviewDriver_detail

`test/scenarios/light/scenario_PreviewDriver_detail.json` — Toggle the Preview driver's detail and decompress controls on a running system and measure the render-FPS impact. detail 2/3 have a known, accepted downsample cost on the render task; decompress is purely client-side and cannot affect the render tick (see performance.md). All steps assert a relative bound (min_pct) only — a single ESP32 scenario step swings too much for an absolute FPS floor to be meaningful (the absolute throughput floor is enforced in collect_kpi.py --commit, which uses a settled reading). detail 3 gets a looser bound because its downsample cost is real and accepted.

- **detail-1-coarse** (set_control) — detail=1 (coarsest, 16x16 downsample on a 128 grid). Cheapest preview render.
- **detail-2-medium** (set_control) — detail=2 (medium, 32x32 downsample). Known accepted cost — must still hit 80% of baseline.
- **detail-3-fine** (set_control) — detail=3 (finest, 43x43 downsample). Looser bound (70%) because the downsample cost is real and accepted.
- **decompress-on** (set_control) — decompress=true. Client-side hint — must not affect the render tick.
- **decompress-off** (set_control) — decompress=false. Same as above — pure client-side, no render impact expected.
