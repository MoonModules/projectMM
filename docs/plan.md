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

## WiFi performance testing (pending)

Need to measure FPS over WiFi STA vs Ethernet at different LED counts. The leaked WiFi task caused 8 FPS (fixed via `esp_wifi_deinit()`), but actual WiFi operation may still be slower than Ethernet due to encryption overhead and management frames. Test matrix:

- WiFi STA 128x128 (16K LEDs, 97 ArtNet universes) — may be too many packets for WiFi
- WiFi STA 64x64 (4K LEDs, 24 universes) — should be feasible
- WiFi STA 32x32 (1K LEDs, 6 universes) — baseline
- Compare each with Ethernet at the same grid size

This determines the practical LED limit for WiFi-only boards. If WiFi can't handle 128x128, document the maximum and recommend Ethernet for large installations.

## Additional testing (pending)

- **UI page load time**: add a scenario step that measures HTTP response time for `/` (index.html), `/api/state`, `/api/system` using the live runner's HTTP client. Verifies the web UI loads within acceptable time on ESP32.
- **Module teardown memory**: add a scenario that tears down all modules (`DELETE /api/modules/*`) and verifies heap returns to pre-setup baseline. Confirms no memory leaks in the full lifecycle.

## mDNS toggle (evaluate)

The mDNS checkbox in NetworkModule was added as a diagnostic tool during performance investigation. Testing showed mDNS has zero FPS impact (the issue was a leaked WiFi task, not mDNS). Evaluate whether to keep the toggle (useful for debugging on other boards) or remove it (unnecessary complexity). Decision after WiFi performance testing.

## ESP-IDF version pinning (pending)

The `setup_esp_idf.py` script currently clones or pulls the latest from the ESP-IDF repo. Need to check: does it pin to a specific commit/tag, or does it always get latest? If latest, running "Setup ESP-IDF" in MoonDeck will silently change the IDF version, potentially breaking the build. Should pin to the tested version (`v6.1-dev-399-gd1b91b79b`) in the setup script or document that updates require re-testing.

## Flash partition scheme (pending)

With WiFi included, firmware grew from ~365KB to ~879KB. The default ESP32 partition table (`default.csv`) has a 1MB app partition. At 879KB / 1024KB = 86% full — little room for growth. Need to adopt a custom partition table (like projectMM v1) with a larger app partition. Options:
- Single OTA: 2MB app partition (no OTA rollback, but maximum space)
- Dual OTA: 2x 1.5MB app partitions (OTA updates with rollback, needs 4MB flash)
- Filesystem partition for config persistence (item 11)

Reference projectMM v1's partition scheme for a proven layout.
