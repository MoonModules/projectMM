# Testing

What we test and how. Each section corresponds to a test file or scenario. Module specs link here so contributors can see what is covered.

## Testing strategy

Three test categories, each with a clear purpose:

- **Module tests** (desktop, `test/test_*.cpp`) — test individual MoonModules in isolation. Each module has its own test file. Run via doctest (`ctest` or `./build/test/mm_tests -s`). Verify a module's API, edge cases, and output are correct — independent of how the module is wired into a pipeline. Module specs in `docs/moonmodules/` link to their test sections here.
- **Scenario tests** (desktop, `test/scenarios/*.json`) — test the system as an integrated pipeline. Each scenario is a declarative JSON file with a sequence of steps (`add_module`, `set_control`) and optional performance bounds. The scenario runner (`test/scenario_runner.cpp`) replays steps in-process and checks output and timing. The same JSON files run against a live device through the HTTP API (see Live Scenario Tests below).
- **Regression tests** — when a bug is found, the fix includes a new test (module test or scenario) that reproduces the bug. References the bug in a comment so the connection stays traceable.

**Performance checks** verify architectural rules at runtime:

- **Zero-allocation render loop** — run N frames, intercept `malloc` / `free` (via overriding or platform allocator hooks), fail if any allocation occurs during steady-state rendering.
- **Frame time bounds** — scenario tests include `"bounds": {"fps": {"min": N}}` to catch performance regressions.

**Live system tests** (on-device) cover what desktop can't:

- Memory stays within bounds over long runs (no leaks, no fragmentation drift).
- Light output produces correct signal (protocol-level, verified with logic analyser or known-good reference).
- Multi-device sync achieves sub-millisecond accuracy.

These are semi-automated for now. Automation strategy will emerge with the hardware.

Per-module timing, memory, and sizeof measurements per platform live in [performance.md](performance.md).

## Module Tests

Module tests verify individual MoonModules in isolation. Run with `./build/test/mm_tests -s` or via MoonDeck (PC → Test).

### color

`test/test_color.cpp`
Tests `hsvToRgb` and `scale8` in `src/core/color.h`.

- hsvToRgb at cardinal hues (h=0 red, h=85 green, h=170 blue)
- White output when saturation is zero
- Black output when value is zero
- Intermediate hues produce mixed channels
- hsvToRgb is constexpr
- scale8 at boundary values (0, 128, 255)
- scale8 is constexpr

### moonmodule

`test/test_moonmodule.cpp`
Tests `MoonModule` lifecycle and `Control` binding in `src/core/MoonModule.h` and `src/core/Control.h`.

- Lifecycle methods called (setup, teardown)
- Name get/set
- Parent get/set
- Control binding via ControlList (uint8_t, bool)
- Pointer binding: changing variable updates control, changing via pointer updates variable
- ControlList clear and rebuild

### lifecycle-propagation

`test/test_lifecycle_propagation.cpp`
Pins the `MoonModule` base-default behaviour for `loop` / `loop20ms` / `loop1s`: a container with children gets per-child dispatch + gating + timing automatically; a leaf module (no children) is a safe no-op. Regression target: before this propagation existed, every container had to write the same 5-line per-child loop by hand — one missing block meant a child's tick callback silently never ran.

- `loop` default ticks each enabled child exactly once
- `loop` default skips children with `enabled() == false`
- `loop` default still ticks children whose `respectsEnabled()` returns false (diagnostics-style modules keep running)
- `loop20ms` and `loop1s` follow the same gating + dispatch rules
- Leaf module (childCount_ == 0): default tick callbacks are safe no-ops, no timing accumulated
- Per-child timing is accumulated on each child's `accumUs_`, surfaced via `publishTiming` → `loopTimeUs()`

### tree-mutation

`test/test_movechild.cpp`, `test/test_replacechild.cpp`
Tests the child-array mutations in `src/core/MoonModule.h` that back the UI's reorder and replace operations.

- `moveChildTo`: forward/backward/one-step reorder, no-op when already at target, out-of-range and non-child rejected
- `replaceChildAt`: swap at the same position with siblings intact, old child detached and replacement parented, out-of-range and null replacement rejected
- Replace lifecycle: the replacement is built → set up → allocated, then the old module torn down — the order `HttpServerModule::handleReplaceModule` runs

### buffer

`test/test_buffer.cpp`
Tests `Buffer` in `src/light/layers/Buffer.h`.

- Allocate and verify count, channelsPerLight, bytes, data pointer, span
- Clear zeros all data
- Move constructor transfers ownership, source becomes null
- Move assignment transfers ownership
- Double free is safe (no crash)
- Allocate with zero count or zero channels returns false

### gridlayout

`test/test_grid_layout.cpp`
Tests `GridLayout` and `Layouts` in `src/light/layouts/GridLayout.h` and `src/light/layouts/Layouts.h`.

- 4x4x1 grid yields 16 coordinates in row-major order (x fastest)
- Correct (idx, x, y, z) at first, middle, and last positions
- 2x2x2 grid yields 8 coordinates with correct z values
- 1x1x1 edge case
- Layouts with one layout: totalLightCount and forEachCoord
- Layouts with two layouts: physical indices offset correctly

### rainbow

`test/test_rainbow.cpp`
Tests `RainbowEffect` in `src/light/RainbowEffect.h`.

- Buffer contains non-zero RGB data after render
- Pixel (0,0) has at least one channel at 255 (full value from hsvToRgb)
- Different positions produce different hues (spatial variation)

### artnet

`test/test_artnet_packet.cpp`
Tests `ArtNetSendDriver::buildPacket` in `src/light/ArtNetSendDriver.h`.

- Header: "Art-Net\0" at offset 0
- OpCode 0x5000 little-endian at offset 8
- Protocol version 14 big-endian at offset 10
- Sequence field at offset 12
- Universe little-endian at offset 14
- Length big-endian at offset 16
- Data starts at offset 18
- Non-zero universe encoding
- Universe splitting: 256 RGB lights → 2 universes (510 + 258 bytes)
- Length field big-endian for 510 bytes

### noise

`test/test_noise.cpp`
Tests `NoiseEffect` and `PlasmaEffect` in `src/light/`.

- Buffer contains non-zero RGB data after render
- Different positions produce different colors (spatial variation)
- Produces different output than RainbowEffect
- **3D depth:** NoiseEffect produces different output per z-slice when `depth > 1` (8×8×8 grid; adjacent and distant slices both differ — `noise3d` actually samples the third axis)
- **3D depth:** PlasmaEffect produces different output per z-slice when `depth > 1` (the fifth z-driven sine actually contributes)

### plasma

`test/test_plasma.cpp`
Tests `PlasmaEffect` in `src/light/PlasmaEffect.h`.

- Buffer contains non-zero RGB data after render
- Different positions produce different colors (spatial variation)
- Produces different output than NoiseEffect

### metaballs

`test/test_metaballs.cpp`
Tests `MetaballsEffect` in `src/light/MetaballsEffect.h`.

- Buffer contains non-zero RGB data after render
- Different positions produce different colors (spatial variation)

### fire

`test/test_fire.cpp`
Tests `FireEffect` in `src/light/FireEffect.h`.

- Heat buffer allocated to `width * height` bytes when enabled
- Buffer non-zero after ~50 frames of high sparking
- Heat buffer freed when disabled via `setEnabled(false)`

### particles

`test/test_particles.cpp`
Tests `ParticlesEffect` in `src/light/ParticlesEffect.h`.

- Trail buffer allocated to `width * height * cpl` bytes when enabled
- Buffer non-zero after a single render (particles draw immediately)
- Trail buffer freed when disabled via `setEnabled(false)`

### stateless-effects

`test/test_stateless_effects.cpp`
Tests zero-heap effects: `CheckerboardEffect`, `SpiralEffect`, `PlasmaPaletteEffect`, `RipplesEffect`, `GlowParticlesEffect`, `LavaLampEffect`.

Each effect: buffer contains non-zero RGB after render; corners produce different colours (spatial variation).

### mappinglut

`test/test_mapping_lut.cpp`
Tests `MappingLUT` in `src/light/layers/MappingLUT.h`.

- Default state is oneToOne
- setOneToOne: forEachDestination returns logical index
- Build with 1:N mappings: verify correct destinations for each logical index
- Free and rebuild: clean reset, can re-allocate

### mirror

`test/test_mirror.cpp`
Tests `MirrorModifier` in `src/light/modifiers/MirrorModifier.h`.

- `dimensions()` returns `Dim::D3` (advertises 3D capability — pins ModifierBase default)
- logicalDimensions: 128x128 with mirrorXY → 64x64
- logicalDimensions: odd grid (127x127) → 64x64 (ceiling division)
- Corner pixel (0,0) produces 4 positions with mirrorXY
- Centre pixel on odd grid: deduplication (1 position, not 4)
- No mirrors: 1 position
- mirrorX only: 2 positions

### extrude

`test/test_extrude.cpp`
Pins the contract that `EffectBase::dimensions()` and `Layer::extrude` form: a D2 effect writes only the z=0 slice and the framework duplicates it across z; a D1 effect writes only the y=0,z=0 row and the framework fills y then z; a D3 effect iterates the layer itself. The effect must honour the layer's runtime dimensions even when the layer has fewer axes than the effect's declared D.

- D2 effect (`RainbowEffect`) on 3D grid (8×8×4): every z>0 slice byte-equals z=0 (extrude copied the plane).
- D1 stub effect on 3D grid (8×4×3): every y>0 row byte-equals y=0 within z=0, then every z>0 slice byte-equals z=0.
- D3 effects (`NoiseEffect`, `PlasmaEffect`) on a 2D layer (8×8×1): buffer is exactly `w*h*cpl` bytes and is non-zero (proves the effect iterated correctly with depth=1; protects against hardcoded depth bounds).
- D3 effects (`NoiseEffect`, `PlasmaEffect`) on a 1D layer (16×1×1): same shape, even tighter (`w*cpl` bytes).
- D2 effects (`CheckerboardEffect`, `FireEffect`, `ParticlesEffect`) on 3D grid (8×8×3): every z>0 slice byte-equals z=0 — proves extrude fills z for all three styles (stateless, stateful with heat grid, stateful with trail buffer). Catches a future regression where a D2 effect's `onBuildState` resizes the dynamic buffer to the full 3D shape but the loop still writes only z=0.

### layers-container

`test/test_layers_container.cpp`
Pins the contract that the new top-level `Layers` container is a thin pass-through when it holds one layer (byte-identical to the pre-container single-layer pipeline), and that with two layers both child loops run and write their own buffers (composition not yet wired — covered separately when composition lands).

- Single child: Layers + one Layer + RainbowEffect produces the same shape and a populated buffer that matches the bare-Layer reference. Bytes can't be exact-compared (RainbowEffect's phase uses `platform::millis()`, advancing between the two `loop()` calls), so structure (size, non-zero) is checked.
- Two children: Layers + Layer(RainbowEffect) + Layer(CheckerboardEffect) — both child buffers are populated after one `loop()`. Confirms each Layer's `loop()` runs in order.
- `Layers::activeLayer()` returns the first enabled child, the first child when none are enabled (fallback for dimension queries during boot/toggle-all-off), and `nullptr` when the container is empty.

### drivers-container

`test/test_drivers_container.cpp`
Pins that `Drivers::loop()` honours each child driver's `enabled` flag — regression for a bug where toggling an individual driver (ArtNet, Preview) in the UI was a no-op because the container ran every child unconditionally.

- Both children enabled: both `loop()`s run on each container tick.
- Disable one child: only the other ticks; the disabled child's `loop()` is not called.
- Disable both: neither ticks.
- Re-enable a previously-disabled child: it resumes ticking from the next call.

### layouts-container

`test/test_layouts_container.cpp`
Pins that `Layouts::totalLightCount()` and `Layouts::forEachCoord()` skip disabled children, and that the physical indices of subsequent enabled layouts shift down to close the gap (no holes).

- Both enabled (4 + 2 lights): total = 6, indices 0..5 in order.
- Disable the first child: total = 2, second child's lights now at indices 0..1.
- Disable both: total = 0, no callback invocations.
- Re-enable both: original 0..5 ordering restored.

### blendmap

`test/test_blend_map.cpp`
Tests `blendMap()` in `src/light/layers/BlendMap.h`.

- oneToOne: output equals input (memcpy path)
- 1:N mapping: logical pixel appears at multiple physical positions
- Additive blend with clamping

### correction

`test/test_correction.cpp`
Tests `Correction` in `src/light/drivers/Correction.h` (per-driver output correction: brightness LUT, channel reorder, RGBW white) and the selective control-change rebuild gate.

- Brightness LUT: full brightness (255) is identity; half (128) halves each value (scale8: 255→128, 128→64)
- RGB preset: apply is identity at full brightness
- GRB / BGR presets: channels reordered correctly, 3 output channels
- RGBW preset: 4 output channels, white = `min(r,g,b)`
- GRBW preset: reordered RGB + white
- Brightness applied *before* white derivation (white is min of the scaled channels)
- rebuild switches output channel count when toggling RGB↔RGBW presets
- `controlChangeTriggersBuildState`: Layout (`width`) and Modifier (`mirrorX`) opt in (return true); effects (`scale`) and Drivers (`brightness`) do not (return false) — pins the fluent-slider gate

## Scenario Tests

Scenario tests verify the integrated pipeline. Defined as JSON in `test/scenarios/`. Run with `./build/test/mm_scenarios` or via MoonDeck (PC → 01 - Pipeline).

### scenario-pipeline

`test/scenarios/base-pipeline.json`
Sets up the core pipeline: Layouts → GridLayout → Layer → RainbowEffect → Drivers → ArtNetSendDriver.

- Buffer allocated after setup
- Buffer size matches layout light count
- Buffer contains non-zero data after rendering 200 frames
- FPS >= 30 (performance bound)

### scenario-mirror

`test/scenarios/mirror.json`
Pipeline with mirror modifier: Layouts → GridLayout → Layer → NoiseEffect → MirrorModifier → Drivers → ArtNetSendDriver. Tests the full LUT-based pipeline with 1:N mapping.

- Buffer allocated after setup (logical size, not physical)
- Buffer contains non-zero data after rendering 200 frames
- FPS >= 30 (performance bound)

### scenario-memory-1to1

`test/scenarios/memory-1to1.json`
Verifies 1:1 unshuffled mapping (no modifiers) uses zero intermediate buffers.

- 16×16 grid, rainbow effect, no modifier
- Asserts: LUT is 1:1, Drivers dynamicBytes = 0
- Reports per-module sizeof and heap allocation

### scenario-memory-lut

`test/scenarios/memory-lut.json`
Verifies modifier with LUT allocates mapping table and driver buffer.

- 16×16 grid, noise effect + mirror modifier (1:N multimap)
- Asserts: LUT allocated (`hasLUT()`), Drivers has output buffer
- Reports LUT size, buffer sizes, per-module metrics

## Live Scenario Tests

Live scenarios run against a running device via HTTP REST API. Same JSON format as in-process scenarios. Run with `scripts/scenario/run_live_scenario.py`.

### scenario-control-change

`test/scenarios/control-change.json`
Tests performance impact of control changes on a running system.

- Measures FPS/heap after each control change
- Toggles mirror X/Y and measures impact
- Checks FPS bounds
- Supports baseline comparison for regression detection

### Running live scenarios

```bash
uv run scripts/scenario/run_live_scenario.py --host localhost:8080       # desktop
uv run scripts/scenario/run_live_scenario.py --host 192.168.1.210        # ESP32
uv run scripts/scenario/run_live_scenario.py --update-baseline           # save
uv run scripts/scenario/run_live_scenario.py --compare-baseline          # check
```

### Live scenario behavior

All scenarios use relative FPS bounds (`min_pct`) so they pass on any device — desktop at 10K FPS or ESP32 at 17 FPS. Settle time is 3 seconds to let the pipeline stabilize after rebuilds.

Scenarios that add modules (`base-pipeline`, `memory-1to1`) create temporary modules on the running device. These are cleaned up after each scenario (`- Rainbow (cleanup)`). Modules that already exist show `=` instead of `+`.

Memory tracking works on ESP32: `freeHeap` and `freeInternalHeap` report real values. Desktop returns 0 (unlimited). The control-change scenario verifies no memory leaks by checking heap returns to baseline after mirror toggle.

## Hardware Verification

All 5 live scenarios pass on both desktop and ESP32 with `min_pct: 80` relative bounds. Per-module timing, memory allocation, and sizeof measurements for each platform are in [performance.md](performance.md).

### ESP32 — Olimex ESP32-Gateway Rev G (no PSRAM)

- 128x128 grid (16,384 lights) — all live scenarios pass
- Memory tracking verified: mirror toggle shows heap changes, returns to baseline (no leaks)
- Ethernet (LAN8720 RMII) connects via DHCP
- Device discovery from MoonDeck finds ESP32 on port 80

## Adding Tests

**Module test:** add a `TEST_CASE` to the appropriate `test/test_*.cpp` file. If the module doesn't have a test file yet, create one and add it to `test/CMakeLists.txt`. Update the matching section in this file.

**Scenario test:** create a JSON file in `test/scenarios/`. The scenario runner auto-discovers all `.json` files in that directory.

**Regression test:** when fixing a bug, add a test that reproduces it. Include a comment referencing the bug (e.g. `// Regression: phase overflow after ~5.5 minutes`). Add a note in the relevant section of this file.
