# Testing

What we test and how. Each section corresponds to a test file or scenario. Module specs link here so end users can see what is covered.

See [architecture.md](architecture.md#testing) for the testing strategy and categories.

## Module Tests

Module tests verify individual MoonModules in isolation. Run with `./build/test/mm_tests -s` or via MoonDeck (PC → Test).

### Color Math (`test/test_color.cpp`) {#color}

Tests `hsvToRgb` and `scale8` in `src/core/color.h`.

- hsvToRgb at cardinal hues (h=0 red, h=85 green, h=170 blue)
- White output when saturation is zero
- Black output when value is zero
- Intermediate hues produce mixed channels
- hsvToRgb is constexpr
- scale8 at boundary values (0, 128, 255)
- scale8 is constexpr

### MoonModule + Control (`test/test_moonmodule.cpp`) {#moonmodule}

Tests `MoonModule` lifecycle and `Control` binding in `src/core/MoonModule.h` and `src/core/Control.h`.

- Lifecycle methods called (setup, teardown)
- Name get/set
- `typeName` independent of `name` — factory sets typeName once, name can be overridden
- Parent get/set
- Dirty flag set/clear (future persistence consumer)
- Control binding via ControlList (uint8_t, bool)
- Pointer binding: changing variable updates control, changing via pointer updates variable
- ControlList clear and rebuild
- ReadOnly control: binds to char buffer, reflects updates
- Select control: binds to uint8_t index, stores options pointer in aux, option count in max
- Progress control: binds to uint32_t value, total stored in aux

### SystemModule (`test/test_system_module.cpp`) {#systemmodule}

Tests `SystemModule` in `src/core/SystemModule.h`.

- MAC-to-deviceName: desktop MAC DE:AD:BE:EF:CA:FE produces "MM-CAFE"
- Controls include deviceName as Text, uptime/fps/chip as ReadOnly

### Buffer (`test/test_buffer.cpp`) {#buffer}

Tests `Buffer` in `src/light/Buffer.h`.

- Allocate and verify count, channelsPerLight, bytes, data pointer, span
- Clear zeros all data
- Move constructor transfers ownership, source becomes null
- Move assignment transfers ownership
- Double free is safe (no crash)
- Allocate with zero count or zero channels returns false

### GridLayout (`test/test_grid_layout.cpp`) {#gridlayout}

Tests `GridLayout` and `LayoutGroup` in `src/light/GridLayout.h` and `src/light/LayoutGroup.h`.

- 4x4x1 grid yields 16 coordinates in row-major order (x fastest)
- Correct (idx, x, y, z) at first, middle, and last positions
- 2x2x2 grid yields 8 coordinates with correct z values
- 1x1x1 edge case
- LayoutGroup with one layout: totalLightCount and forEachCoord
- LayoutGroup with two layouts: physical indices offset correctly

### RainbowEffect (`test/test_rainbow.cpp`) {#rainbow}

Tests `RainbowEffect` in `src/light/RainbowEffect.h`.

- Buffer contains non-zero RGB data after render
- Pixel (0,0) has at least one channel at 255 (full value from hsvToRgb)
- Different positions produce different hues (spatial variation)

### ArtNet Packet (`test/test_artnet_packet.cpp`) {#artnet}

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

### Noise Effect (`test/test_noise.cpp`) {#noise}

Tests `NoiseEffect` in `src/light/NoiseEffect.h`.

- Buffer contains non-zero RGB data after render
- Different positions produce different colors (spatial variation)
- Produces different output than RainbowEffect

### MappingLUT (`test/test_mapping_lut.cpp`) {#mappinglut}

Tests `MappingLUT` in `src/light/MappingLUT.h`.

- Default state is oneToOne
- setOneToOne: forEachDestination returns logical index
- Build with 1:N mappings: verify correct destinations for each logical index
- Free and rebuild: clean reset, can re-allocate

### Mirror Modifier (`test/test_mirror.cpp`) {#mirror}

Tests `MirrorModifier` in `src/light/MirrorModifier.h`.

- logicalDimensions: 128x128 with mirrorXY → 64x64
- logicalDimensions: odd grid (127x127) → 64x64 (ceiling division)
- Corner pixel (0,0) produces 4 positions with mirrorXY
- Centre pixel on odd grid: deduplication (1 position, not 4)
- No mirrors: 1 position
- mirrorX only: 2 positions

### BlendMap (`test/test_blend_map.cpp`) {#blendmap}

Tests `blendMap()` in `src/light/BlendMap.h`.

- oneToOne: output equals input (memcpy path)
- 1:N mapping: logical pixel appears at multiple physical positions
- Additive blend with clamping

### MoonModule::moveChildTo (`test/test_movechild.cpp`) {#movechild}

Tests `MoonModule::moveChildTo(child, newIndex)` in `src/core/MoonModule.h`.

- No-op when already at target index returns false
- Forward move (toward end) shifts intervening siblings left
- Backward move (toward start) shifts intervening siblings right
- One-position swap (matches up/down buttons in the UI)
- Out-of-range newIndex rejected without modifying the tree
- Non-child argument rejected without modifying the tree
- Middle-to-middle move

### ModuleFactory (`test/test_module_factory.cpp`) {#module-factory}

Tests `ModuleFactory` in `src/core/ModuleFactory.h`.

- `registerType<T>()` captures `T::role()` via a stack-allocated probe instance — no per-type boilerplate
- `create()` returns a module of the registered type with `typeName()` set
- `create()` returns nullptr for unknown or null type names
- `typeName()` and `typeRole()` are bounds-safe (out-of-range returns nullptr / Generic)
- Dynamic capacity grows past the initial 4 — registering 10 throwaway types all succeed and remain discoverable

### Filesystem persistence (`test/test_filesystem_persistence.cpp`) {#filesystem-persistence}

Four test cases for [FilesystemModule](moonmodules/core/FilesystemModule.md):

- **Value round-trip**: set `deviceName` on a running Scheduler+SystemModule, wait for the 2s debounce flush, recreate a fresh Scheduler+modules, and assert the deviceName is loaded from disk.
- **Structural reconciliation**: hand-write a `Layer.json` describing a one-child tree (`RainbowEffect`). Build a live two-child tree (`NoiseEffect` + `MirrorModifier`) and run `scheduler.setup()`. Assert the live tree was reconciled — NoiseEffect swapped to RainbowEffect, Mirror trimmed.
- **Valid JSON with children**: flush a Layer subtree containing two children (each with controls) and assert the raw file contains `"MirrorModifier",` / `"NoiseEffect",` — guards against the missing-comma bug between a child's `"N.type"` field and its first control.
- **Singleton survives probe lifecycle**: construct the real FS instance + register via `setScheduler` (which now binds the singleton), factory-construct+destruct a probe instance (mimicking what `/api/types` does to capture defaults), and assert that `flushPending()` still saves a marked-dirty subtree after the probe is gone — guards against the bug where the probe's destructor cleared the static singleton, silently breaking persistence for the rest of the device's life.

All four use `platform::fsSetRoot()` to redirect the config root into `/tmp/mm_*_test_<ms>/` for isolation. Combined wall time ~2.3s (the debounce window in the first test dominates).

## Scenario Tests

Scenario tests verify the integrated pipeline. Defined as JSON in `test/scenarios/`. Run with `./build/test/mm_scenarios` or via MoonDeck (PC → 01 - Pipeline).

### base-pipeline (`test/scenarios/base-pipeline.json`) {#scenario-pipeline}

Sets up the core pipeline: LayoutGroup → GridLayout → Layer → RainbowEffect → DriverGroup → ArtNetSendDriver.

- Buffer allocated after setup
- Buffer size matches layout light count
- Buffer contains non-zero data after rendering 200 frames
- FPS >= 30 (performance bound)

### mirror (`test/scenarios/mirror.json`) {#scenario-mirror}

Pipeline with mirror modifier: LayoutGroup → GridLayout → Layer → NoiseEffect → MirrorModifier → DriverGroup → ArtNetSendDriver. Tests the full LUT-based pipeline with 1:N mapping.

- Buffer allocated after setup (logical size, not physical)
- Buffer contains non-zero data after rendering 200 frames
- FPS >= 30 (performance bound)

### memory-1to1 (`test/scenarios/memory-1to1.json`) {#scenario-memory-1to1}

Verifies 1:1 unshuffled mapping (no modifiers) uses zero intermediate buffers.

- 16×16 grid, rainbow effect, no modifier
- Asserts: LUT is 1:1, DriverGroup dynamicBytes = 0
- Reports per-module sizeof and heap allocation

### memory-lut (`test/scenarios/memory-lut.json`) {#scenario-memory-lut}

Verifies modifier with LUT allocates mapping table and driver buffer.

- 16×16 grid, noise effect + mirror modifier (1:N multimap)
- Asserts: LUT allocated (`hasLUT()`), DriverGroup has output buffer
- Reports LUT size, buffer sizes, per-module metrics

## Live Scenario Tests

Live scenarios run against a running device via HTTP REST API. Same JSON format as in-process scenarios. Run with `scripts/scenario/run_live_scenario.py`.

### control-change (`test/scenarios/control-change.json`) {#scenario-control-change}

Tests performance impact of control changes on a running system.

- Measures FPS/heap after each control change
- Toggles mirror X/Y and measures impact
- Checks FPS bounds
- Supports baseline comparison for regression detection

### mdns-toggle (`test/scenarios/mdns-toggle.json`) {#scenario-mdns-toggle}

Measures FPS impact of enabling/disabling mDNS on a running device.

- Toggles mDNS on → off → on, measures FPS at each step
- Final step asserts `fps.min_pct: 80` — re-enabling mDNS keeps FPS within 20% of baseline

### grid-resize (`test/scenarios/grid-resize.json`) {#scenario-grid-resize}

Exercises the light-pipeline adaptive allocation path. Useful for catching regressions in the Layer/DriverGroup allocate path under memory pressure (see plan 11.5 in `docs/plan.md`).

- Mirror state reset to known values at start (mirrorX=true, mirrorY=true) so subsequent runs don't contaminate each other
- Sizes the grid to 128×128, then shrinks to 128×64, then grows back to 128×128
- Only the shrink step asserts a bound (`fps.min_pct: 80`) because shrinks always release memory and should match or beat baseline. Grow steps deliberately have no bound — they're inherently slower at a larger size.

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
