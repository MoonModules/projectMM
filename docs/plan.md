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

## 5b. 3D WebGL Preview

Add Preview driver that streams binary light data via WebSocket. 3D point-cloud renderer in the browser UI. Binary frame format: `[0x02][w16][h16][d16][RGB...]`.

## 8. Live scenario testing

Python scenario runner that replays scenario JSON files via HTTP against a running device (desktop or ESP32). Same JSON format as in-process runner. MoonDeck Live tab: device discovery (subnet scan + /api/state probe), device selection, run scenarios against selected device. Adapts projectMM v1's `deploy/scenario.py` for v3's REST API.

## 9. System MoonModule + Network MoonModules

Add System MoonModule (reverse engineer from projectMM v1, MoonLight, in that order). System-level info, diagnostics, heap reporting. Add Ethernet and WiFi MoonModules — when Ethernet is available, WiFi doesn't need to run. Needs UI to be useful.
