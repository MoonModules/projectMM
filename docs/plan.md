# What to build next

Completed items are removed. This file is deleted when empty.

## Each commit delivers

- Source code (src/)
- Unit tests (test/) — all passing
- Integration test if pipeline is involved
- Platform boundary check passing
- Zero warnings (`-Wall -Wextra -Werror`)
- Updated MoonModule specs (docs/moonmodules/) for what was built
- Tested on hardware where applicable

## 1. Core pipeline on desktop — lights on panel via ArtNet

Grid layout → Rainbow effect → ArtNet driver → lights visible on panel (via ArtNet receiver).

Modules to review and promote:
- core: MoonModule, Control, Scheduler
- light: Buffer, MappingLUT, Layer, LayoutGroup, DriverGroup, EffectBase, BlendMap, Pixel
- modules: GridLayout, RainbowEffect, ArtNetSendDriver
- platform: Alloc, Timing, UdpSocket

Also: CMake build system, platform desktop, doctest, integration test.

## 2. ESP32 deployment

Same pipeline running on ESP32dev. Proves platform abstraction works, esp32/ CMake wrapper, ESP-IDF build, flash, run. ArtNet output from ESP32 to panel.

Add a System MoonModule (reverse engineer from projectMM v1, MoonLight, in that order) — system-level info, diagnostics, heap reporting.

Modules: platform esp32 (Alloc, Timing, UdpSocket implementations), SystemModule.

## 3. Second effect

Add Noise effect. Proves effect switching via API.

## 4. Mirror modifier

Add Mirror modifier. Proves modifiers, virtual interface, 1:N kaleidoscope mapping, LUT rebuild on control change.

## 5. WebSocket + Preview

Add WebSocket server and Preview driver. Proves system MoonModules, binary frame streaming, 3D browser preview.

## 6. Web UI (tree view)

Add embedded web UI with tree view. Proves MoonModule-driven UI, auto-rendered controls, effect/modifier switching from browser.
