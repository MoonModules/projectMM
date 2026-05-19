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

## 5. WebSocket + Preview

Add WebSocket server and Preview driver. Proves system MoonModules, binary frame streaming, 3D browser preview.

## 6. Web UI (tree view)

Add embedded web UI with tree view. Proves MoonModule-driven UI, auto-rendered controls, effect/modifier switching from browser. HTTP API enables live scenario testing against running systems.

Also: MoonDeck device discovery — scan subnet, probe `/api/system`, checkboxes to select devices for live testing. Needs HTTP API on devices first (see projectMM v2's `scan_subnet()` approach).

## 7. System MoonModule + Network MoonModules

Add System MoonModule (reverse engineer from projectMM v1, MoonLight, in that order). System-level info, diagnostics, heap reporting. Add Ethernet and WiFi MoonModules — when Ethernet is available, WiFi doesn't need to run. Needs UI to be useful.
