# What to build next

Completed items are removed. This file is deleted when empty.

## 11. Config persistence (control-list-driven JSON)

Save each MoonModule's controls to flash so settings survive reboot. Storage is one **flat JSON file per top-level MoonModule**; children are encoded with a `<index>.` key prefix so the parser stays flat. Reuses the existing `parseJsonString/Int/Bool` helpers — no new parser.

Plan-09 attempted JSON and overshot (nested objects, scoped parser, 4-phase scheduler, multi-pass control rebuild). The minimal version below stays simple by accepting two trade-offs: flat dotted keys instead of nested objects, and modules build their full control set unconditionally with a `hidden` flag for UI-only conditional visibility.

**File format** — flat, one file per top-level module:
```
/.config/
  System.json       → {"deviceName":"MM-3A7F","enabled":true}
  Network.json      → {"ssid":"home","password":"...","addressing":1,"ip":"...","enabled":true}
  Layer.json        → {"channelsPerLight":3,"enabled":true,
                       "0.type":"NoiseEffect","0.scale":12,"0.bpm":60,"0.enabled":true,
                       "1.type":"MirrorModifier","1.mirrorX":false,"1.mirrorY":true,"1.enabled":true}
```

**Conditional controls become `hidden`, not absent.** ControlList grows a `bool hidden` per descriptor. NetworkModule unconditionally adds the static-IP fields and sets them `hidden = (addressing != Static)`. UI honors the flag. Persistence load works because every key is always bound. Toggling the addressing dropdown flips visibility — no `clear()+onBuildControls()` race.

**Lifecycle** — one phase swap in `Scheduler::setup()`:
```
phase 1: onBuildControls()   (bind variables → ControlList descriptors)
phase 2: load                (FilesystemModule reads files, overlays values onto bound variables)
phase 3: setup()             (modules' own init runs with persisted values in their variables)
phase 4: onAllocateMemory()  (buffers sized to final values)
```
Single load pass. No re-load, no rebuildControls during boot. Modules don't know persistence exists.

**Save** — `HttpServerModule::handleSetControl` already calls `module->markDirty()` (foundation commit 7f9afa3). `FilesystemModule::loop1s()` debounces 2s, walks the tree, serializes any subtree containing a dirty descendant, atomic write-and-rename. Clears dirty flags on success.

**Scope:**
- New `src/core/FilesystemModule.h` — ~200 lines including JSON writers (parser reused from HttpServerModule)
- `src/core/Control.h` — add `bool hidden` to `ControlDescriptor` (+ optional setter)
- `src/core/Scheduler.h` — swap setup phase order
- `src/core/NetworkModule.h` — convert "conditional add" to "always add + hidden flag"
- `src/ui/app.js` — skip rendering controls with `hidden: true`
- One doctest that round-trips a module via `platform::fsSetRoot()` for test isolation
- One live scenario that round-trips deviceName through a reboot

**Properties:**
- Human-readable, hand-editable, shareable
- Forward-compatible (unknown keys ignored, missing keys keep defaults)
- No POD constraint on modules — `std::string`/containers/inheritance work fine in any future module
- External tools can read/edit files
- Persistence touches only what was declared as a control (not cached state, timing counters, accumulator fields)

**Structural reconciliation is in scope.** The `<idx>.type` field per child isn't just informational — at load time the FilesystemModule factory-creates a child of the JSON type when the live tree's child at the same position has a different type, and trims children present live but absent in the JSON. This means a user who hot-swaps an effect via the UI and triggers a save will see that effect (not the compile-time default) on the next boot. `MoonModule::replaceChildAt(i, fresh)` is the new primitive.

**What this design avoids vs blob:** the POD-only constraint and the "presets need JSON anyway" problem. **What it avoids vs plan-09:** the nested parser, the 4-phase setup, the rebuildControls-during-setup loop, and per-module `loadInto(this)` calls.

## 11.5. Light pipeline free-then-allocate rebuild

Layer + DriverGroup currently rebuild in-place (allocate-before-free) which produces a heap fragmentation cycle under memory pressure: free heap drops to ~60KB but max contiguous block shrinks to ~15KB, lwIP can't allocate new TCBs, HTTP refuses connections, scenarios fail intermittently. Plan-09 tried defensive guards (~5 patches across BlendMap, DriverGroup, Layer) but the right fix is structural.

**Approach:** add a `Pipeline` coordinator (or modify Scheduler::rebuild) that calls a new two-phase rebuild:
1. **Phase A — free all**: every module's `onAllocateMemory()` is split into `onFreeMemory()` + `onAllocateMemory()`. Phase A walks the tree calling `onFreeMemory` (release LUT, output buffer, layer buffer). After phase A, max heap block is genuinely the post-free state.
2. **Phase B — allocate fresh**: walk again calling `onAllocateMemory`. `canAllocate` sees true heap, degrade decisions are deterministic and consistent across modules.

**Eliminates:**
- Stride-mismatch bugs (LUT references logical=N but buffer count=M)
- Zombie state (buffer empty while LUT thinks it's alive)
- Half-built state where DriverGroup output buffer is freed but Layer thinks LUT is active

**Also consider — LUT build cost:** `Layer::rebuildLUT` runs the triple-nested loop calling `mod->mapToPhysical` for every logical light. For 128×64 logical (mirror XY at 128×128 physical) that's 8192 iterations. For 128×128 logical (no mirror) it's 16384. On ESP32 this takes 500ms–1s, runs synchronously inside the HTTP handler, and blocks rendering. Observed as control-change scenario showing one or two `FPS=1` ticks right after a Mirror toggle. The pipeline rework is a good time to either (a) yield between rows so the render keeps ticking, or (b) move the LUT build off the HTTP task entirely. Either approach is small once free-then-allocate is in place.

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

## Multi-layer pipeline (backlog)

Today `DriverGroup` holds one `Layer*` and `DriverBase::setLayer()` takes one layer. The architecture (`docs/architecture-light.md`) plans for multiple layers feeding one DriverGroup, with per-layer LUTs blended into a single output buffer. The number of active layers depends on available memory — a device with PSRAM can run many; a device without may be limited to one.

When picked up:
- `DriverGroup::passBufferToDrivers` composes/blends N layer buffers upstream (Buffer + Buffer with per-layer blend mode and opacity).
- `DriverBase::setLayer` stays as-is — drivers still output to one physical fixture and need that fixture's dimensions; the *active* layer is what they query. Multi-layer composition happens upstream of drivers.
- Per-layer enable/disable from the UI (already supported by `MoonModule::enabled`); ordering via existing child-array order.
- Memory-aware allocator: decide at `onAllocateMemory` time how many layers actually fit, degrade gracefully if PSRAM is unavailable.
- Persistence (item 11) already encodes layers + their children positionally — adding more siblings to a LayoutGroup just works on the file-format side.
