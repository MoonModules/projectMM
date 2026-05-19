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
- Parent get/set
- Control binding via ControlList (uint8_t, bool)
- Pointer binding: changing variable updates control, changing via pointer updates variable
- ControlList clear and rebuild

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

## Scenario Tests

Scenario tests verify the integrated pipeline. Defined as JSON in `test/scenarios/`. Run with `./build/test/mm_scenarios` or via MoonDeck (PC → 01 - Pipeline).

### base-pipeline (`test/scenarios/base-pipeline.json`) {#scenario-pipeline}

Sets up the core pipeline: LayoutGroup → GridLayout → Layer → RainbowEffect → DriverGroup → ArtNetSendDriver.

- Buffer allocated after setup
- Buffer size matches layout light count
- Buffer contains non-zero data after rendering 200 frames
- FPS >= 30 (performance bound)

## Hardware Verification

### ESP32 — Olimex ESP32-Gateway Rev G (no PSRAM)

Verified working:
- 128x128 grid (16,384 lights) — renders and sends ArtNet at 23+ FPS
- 215KB free heap after buffer allocation (49KB buffer)
- Rainbow effect flows correctly on hub75 panel via ArtNet receiver
- Ethernet (LAN8720 RMII) connects and obtains IP via DHCP
- Stable operation (no crashes, no memory leaks over extended runs)

## Adding Tests

**Module test:** add a `TEST_CASE` to the appropriate `test/test_*.cpp` file. If the module doesn't have a test file yet, create one and add it to `test/CMakeLists.txt`. Update the matching section in this file.

**Scenario test:** create a JSON file in `test/scenarios/`. The scenario runner auto-discovers all `.json` files in that directory.

**Regression test:** when fixing a bug, add a test that reproduces it. Include a comment referencing the bug (e.g. `// Regression: phase overflow after ~5.5 minutes`). Add a note in the relevant section of this file.
