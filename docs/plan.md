# What to build next

Completed items are removed. This file is deleted when empty.

## 11. Config persistence (blob-based)

Save each MoonModule's per-instance state to flash so settings survive reboot. Storage is **binary blob per module instance** — one file per top-level subtree, the file is a recursive dump of each module's post-base member memory plus a header. No JSON parsing, no schema definition, no per-module persistence code.

**Why blob and not JSON** (plan-09 attempted JSON and was abandoned — see `docs/history/plan-09.md`):
- ~80 lines vs ~800 lines of code
- Zero per-module boilerplate
- Save/load is one `memcpy` per module — sub-millisecond
- Files exactly `classSize` bytes — trivial budgeting
- Format-version mismatch (class layout changed) → discard, defaults apply

**File format:**
```
header (16 bytes): magic 'MMBL' | uint16 version | uint16 typeNameHash | uint16 classSize | uint16 childCount
controls blob: classSize - sizeof(MoonModule) bytes of memcpy(this + sizeof(MoonModule))
children: recursive (each child has its own header + blob)
```

**Boot flow:**
1. FilesystemModule::setup() mounts LittleFS, reads /.config/<TypeName>[.N].blob into stack buffer
2. For each top-level module: validate header (magic, hash, classSize). If valid, memcpy bytes back over (this + sizeof(MoonModule)) and recurse into children. If mismatch, discard — defaults stand.
3. Other modules' setup() runs with their member variables already overlaid.

**Save flow:**
- HttpServerModule sets `module->markDirty()` on control changes (already done in pile A).
- FilesystemModule::loop1s() checks dirty flags with 2s debounce, walks subtree, serializes (header + memcpy + recurse), atomic write-and-rename.

**Constraints to enforce at compile time:**
- `static_assert(std::is_trivially_copyable_v<T>)` for any factory-registered type. Modules must only contain POD members. No pointers, no `std::string`, no `std::vector`.
- Children handled separately (their own files), not part of parent's blob.
- Cross-platform binary differs (desktop 64-bit vs ESP32 32-bit) — persistence files are per-device only. Use `MM_DEVICE_TAG` byte in header so wrong-platform files are rejected.

**Estimated scope:** new `FilesystemModule.h` (~80 lines), modify `MoonModule` slightly for class-size introspection (already have `classSize_`), no other module changes. One unit test that round-trips a module.

## 11.5. Light pipeline free-then-allocate rebuild

Layer + DriverGroup currently rebuild in-place (allocate-before-free) which produces a heap fragmentation cycle under memory pressure: free heap drops to ~60KB but max contiguous block shrinks to ~15KB, lwIP can't allocate new TCBs, HTTP refuses connections, scenarios fail intermittently. Plan-09 tried defensive guards (~5 patches across BlendMap, DriverGroup, Layer) but the right fix is structural.

**Approach:** add a `Pipeline` coordinator (or modify Scheduler::rebuild) that calls a new two-phase rebuild:
1. **Phase A — free all**: every module's `onAllocateMemory()` is split into `onFreeMemory()` + `onAllocateMemory()`. Phase A walks the tree calling `onFreeMemory` (release LUT, output buffer, layer buffer). After phase A, max heap block is genuinely the post-free state.
2. **Phase B — allocate fresh**: walk again calling `onAllocateMemory`. `canAllocate` sees true heap, degrade decisions are deterministic and consistent across modules.

**Eliminates:**
- Stride-mismatch bugs (LUT references logical=N but buffer count=M)
- Zombie state (buffer empty while LUT thinks it's alive)
- Half-built state where DriverGroup output buffer is freed but Layer thinks LUT is active

**Scope:** Layer, DriverGroup, Scheduler::rebuild, possibly Buffer/MappingLUT. ~half a day. Self-contained — doesn't touch the persistence story.

## 12. Effect/module switching from UI

Add/remove/switch effects and modifiers from the browser. Type picker with category filtering. Lifecycle-aware add/remove (setup/teardown called at runtime).

## 13. README + quick-start

Update README with: what it does now, how to build/flash, how to connect and open the UI. Include screenshots.

---

## Release 1.0 — "connect, open browser, see lights"

Milestone after items 11-13. An end user with an ESP32 can flash the firmware, connect via WiFi, open a browser, see the 3D preview, change effects and controls, and have settings persist across reboots.

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

## WiFi runtime disable (backlog)

Postponed. Single firmware binary ships the WiFi stack regardless (the 1.75 MB app partition has plenty of room for it to live unused). When and how WiFi controls are exposed in the UI gets revisited after persistence (item 11) lands.

Open design question to address when this is picked up: can the platform detect at runtime whether Ethernet hardware is present (PHY responds on MDIO during `esp_eth_driver_install`)? If yes, the UI can hide WiFi controls — and skip `wifiStaInit()` — when Ethernet hardware is detected. That's a behavior-driven gate rather than a user toggle. Some ESP32 variants (e.g. ESP32-C2, ESP32-H2) don't have WiFi hardware at all, so the gate also needs to handle "WiFi not present" cleanly. Both detections live in `src/platform/`.
