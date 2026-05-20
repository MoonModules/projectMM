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
- Pre-commit checklist passed (8 steps including Reviewer agent)

## 8. Live scenario testing

Python scenario runner that replays scenario JSON files via HTTP against a running device (desktop or ESP32). Same JSON format as in-process runner. MoonDeck Live tab: device discovery (subnet scan + /api/state probe), device selection, run scenarios against selected device. First because: all subsequent work goes through the live test pipeline.

## 9. System MoonModule

System-level diagnostics as a MoonModule: heap free/used, FPS, uptime, chip info, firmware version. Visible in the Web UI. Simpler than WiFi — useful for debugging while building subsequent features. Reverse engineer from projectMM v1, MoonLight.

## 10. WiFi MoonModule

Add WiFi MoonModule (STA + AP fallback). Controls: SSID, password, status. When Ethernet is available, WiFi doesn't need to run. Proves network as a MoonModule. Reverse engineer from projectMM v1.

## 11. Config persistence

Save/load control values to filesystem. Settings survive reboot. Format: one file per module or one file for all — decide based on ESP32 filesystem constraints. Platform filesystem abstraction (LittleFS on ESP32, std::filesystem on desktop).

## 12. Effect/module switching from UI

Add/remove/switch effects and modifiers from the browser. Type picker with category filtering. Lifecycle-aware add/remove (setup/teardown called at runtime).

## 13. README + quick-start

Update README with: what it does now, how to build/flash, how to connect and open the UI. Include screenshots.

---

## Release 1.0 — "connect, open browser, see lights"

Milestone after items 8-13. An end user with an ESP32 can flash the firmware, connect via WiFi, open a browser, see the 3D preview, change effects and controls, and have settings persist across reboots.

---

## Remarks

- Live scenarios that use `add_module` create temporary modules on the running device (cleaned up after each scenario). Scenarios like `base-pipeline` and `memory-1to1` add a `Rainbow` effect because the running device has `Noise` — the names don't match. This is harmless (cleanup deletes it), but the measurement runs with both effects active. For pure non-destructive live testing, scenarios should match the running device's module names, or use `set_control`-only steps that don't modify the pipeline.
