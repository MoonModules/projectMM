# What to build next

Completed items are removed. This file is deleted when empty.

---

## Distribution

### Windows desktop port (blocker for 1.0 Windows binary)

`src/platform/desktop/platform_desktop.cpp` won't compile under MSVC — it uses POSIX socket headers (`sys/socket.h`, `sendmsg`, `fcntl`, …) with no MSVC equivalent. The `build-windows` CI job and the `dist/projectMM-*.zip` upload are disabled in `release.yml` until this lands.

Two approaches:
1. **Conditional includes in `platform_desktop.cpp`** — `#ifdef _WIN32` blocks around every socket call. Smallest diff; local damage inside `src/platform/`.
2. **Split into `platform_desktop_posix.cpp` + `platform_desktop_windows.cpp`** — CMake picks per host. Mirrors the ESP32/desktop split already in place.

Either path: ~2–3 h translation + Windows-side testing. Once green, `build-windows` flips and the v1.0.0 Windows zip ships automatically.

### Release 2.0 — distribution catches up to the source tree

1.0 ships ESP32 firmware (4 variants) + macOS arm64. Still to add:

- **ESP32-P4** board variant — new chip target, new sdkconfig fragment, fits the existing `BOARDS` table in `build_esp32.py`.
- **Linux desktop binary** — third desktop job in `release.yml`, static-linked libstdc++.
- **Teensy 4.1** — toolchain-file build, `.hex` for Teensy Loader.
- **Raspberry Pi** — ARM64, cross-built or native.
- **macOS code-signing** — drops the Gatekeeper "downloaded from internet" prompt.
- **Runtime PHY / pin config for Ethernet** — replaces build-time Olimex-pin baking in `sdkconfig.defaults.eth` with a runtime picker via `platform::ethPresent()` / `platform::wifiPresent()`. Once landed, `esp32-eth*` variants stop being Olimex-specific; NetworkModule's `onBuildControls()` flips the `hidden` flag on absent-interface controls.
- **Installer UX polish** — clear "Pre-release (beta)" warning on RC/latest picks, yank-by-asset-tag instead of yank-by-release-deletion.

---

## ESP32 performance and memory

### WiFi ArtNet performance (pending investigation)

128×128 WiFi ArtNet measurements exist (see [performance.md](performance.md) "ArtNet over WiFi" and "Build-variant WiFi comparison"). Remaining matrix:

- WiFi STA 64×64 (4K LEDs, 24 universes)
- WiFi STA 32×32 (1K LEDs, 6 universes)

This determines the practical LED limit for WiFi-only boards. Until the `sdkconfig.defaults` TX-buffer fix lands (identified in the build-variant table), **use `esp32-eth-wifi` for any ArtNet workload on classic ESP32** even if Ethernet isn't physically connected.

### mDNS toggle (evaluate)

Added as a diagnostic tool during performance investigation; testing showed mDNS has zero FPS impact. Evaluate whether to keep (useful for debugging on other boards) or remove (unnecessary complexity). Decide after WiFi performance testing above.

### Memory ceiling on non-PSRAM ESP32 with eth-wifi (backlog)

On `esp32-eth-wifi`, default 128×128 grid, free heap at boot is ~28 KB — not enough for `esp_wifi_init` (needs ~16 KB RX buffers) after the light pipeline allocates ~210 KB. The device stays running but WiFi init fails silently.

Fix options in increasing scope:
- **Cap the default grid** — drop to 64×64 on `esp32-eth-wifi` (Layer ~32 KB + LUT ~16 KB = 48 KB, comfortably under). Simplest.
- **PSRAM for Layer buffer + LUT** — ESP32-Gateway has 4 MB PSRAM unused on non-S3 builds. Moving the 49 KB pixel buffer + 64 KB LUT out of DRAM frees ~110 KB for radios. Cost: ~25% FPS hit (PSRAM bandwidth ~12 MB/s vs DRAM ~80 MB/s); needs measurement. See [decisions.md](history/decisions.md) "Adaptive memory allocation design" for the allocation rules.
- **Lazy WiFi init** — skip `esp_wifi_init` when `ssid_` is empty and no AP-fallback is pending. Helps only when credentials exist but the network is unreachable — niche.

### Preview memory optimizations (backlog)

- **Defer Preview allocation** — allocate the preview buffer on first WebSocket client connect; free on last disconnect. Saves ~5 KB when no browser is open.
- **Cap `detail` at 2 by default** — `detail = 3` adds ~14 ms/tick (see [performance.md](performance.md) "Preview detail cost"). Straightforward guard in PreviewDriver.

### Task core-pinning (backlog)

No FreeRTOS tasks are pinned today. At 16K LEDs the render task takes ~52 ms/tick; if OTA download or Improv scan causes tick-variance spikes, pin render → core 1, OTA/Improv → core 0 (where WiFi already lives via `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y`). Defer until contention is observed — neither OTA nor Improv runs during normal operation.

---

## Architecture

### Multi-layer composition (backlog)

`Layers` holds N layers; `Drivers` reads from a single active layer today. Composition is the missing piece — additional layers render their buffers but only the first enabled layer reaches output.

When picked up:
- `Drivers::loop()` blends each enabled Layer's buffer into the shared output using per-Layer blend mode + opacity (controls to add on Layer).
- `Layer::startX/Y/Z` / `endX/Y/Z` (already persisted, currently no-op) become active in `rebuildLUT` — each Layer carves a percentage region of the physical extent.
- Memory-aware allocator at `onBuildState` time decides how many Layers fit and degrades gracefully.
- Persistence already encodes Layers children positionally — adding siblings just works on the file-format side.

### Improv as a child of NetworkModule (deferred — needs scheduler work first)

Architecturally the right shape; attempted in plan-21, reverted. Blocker: `Scheduler::tick()` only walks top-level modules for `loop20ms`/`loop1s` — children silently miss those callbacks. See [decisions.md](history/decisions.md) "Trying to add a child module to NetworkModule".

Minimum-scope fix before the move:
1. `MoonModule::loop20ms`/`loop1s` propagate to children (or Scheduler walks them) — pick whichever costs less at runtime.
2. Audit every existing override of `setup`/`loop1s`/`loop20ms`/`onBuildControls`/`teardown` in NetworkModule, SystemModule, FilesystemModule, HttpServerModule to confirm base-chaining.
3. Then the actual move is a one-line `networkModule->addChild(improvModule)` swap. Estimate ~2 h total.

### Platform API: `std::span` migration (backlog)

Several `platform.h` APIs still use `(buf, len)` pairs where `std::span` would catch length/pointer mismatches at compile time. Concrete sites: `http_fetch_to_ota`, `improvProvisioningInit`, and friends. ~2 h including ripple updates to callers. Do alongside the next platform-API expansion (Windows socket port or POST /api/firmware streaming).

---

## HTTP and OTA

### POST /api/firmware — direct binary upload OTA (backlog)

Today `POST /api/firmware/url` covers end-user OTA (device fetches a GitHub release asset). Missing: developer workflow — build HEAD locally, flash over LAN without USB or a public URL.

What to build (~4 h):
- **Platform layer** — implement `ota_begin`/`ota_write`/`ota_end` in [platform_esp32.cpp](../src/platform/esp32/platform_esp32_ota.cpp) wrapping `esp_ota_*` directly (already declared in [platform.h](../src/platform/platform.h), currently bypassed by `esp_https_ota_perform`).
- **HTTP route** — `POST /api/firmware` in [HttpServerModule.cpp](../src/core/HttpServerModule.cpp); streams `conn.read(chunkBuf, 4096)` → `ota_write`; shares `g_otaStatus`/`g_otaPct` with the URL path.
- **`device_ip` control on SystemModule** — lets a script discover the device without mDNS.
- **`scripts/build/flash_over_network.py`** — finds the latest `.bin` in `build/esp32-<board>/`, POSTs it, polls for `rebooting`.
- **UI file-upload affordance** — `<input type="file" accept=".bin">` in a `<details>` expander on the Firmware card.

### HTTP file serving blocks the render tick (backlog)

`HttpServerModule::handleConnection()` serves large embedded files (`app.js`, `style.css`) with the blocking `TcpConnection::write` — a page load can briefly stall `loop20ms`. One-shot per load (lower priority than the per-tick preview issue, which is fixed). Fix: serve large HTTP responses with `writeChunks` (the same non-blocking path used for preview frames).

---

## Effects and preview

### Add real z-axis variation to 2D effects (pending)

Only **NoiseEffect** and **PlasmaEffect** have z-aware math. The other 10 effects are honest D2 — `Layer::extrude` duplicates the z=0 plane, so every z-slice is identical on 3D layers. Candidates for genuine D3 promotion: Metaballs/GlowParticles (add z to blob coordinates), Plasma palette/Spiral (add z-driven phase term), Fire (z-drift heat grid), Ripples/LavaLamp/Checkerboard/Particles (add z to each element). Prioritise after seeing real 3D installations; each promoted effect also needs its `dynamicBytes` budget for the full 3D buffer.

### Preview coordinate message — true-shape 3D preview (backlog)

The 3D preview derives `(x, y, z)` from a dense grid index — correct only for grid layouts. For rings, spheres, or arbitrary point clouds the preview shows a wrong dense bounding box.

Design (already noted in [PreviewDriver.md](moonmodules/light/drivers/PreviewDriver.md)):
- Engine sends a one-time coordinate table per layout change and per new WS client: `[type][count16][x16 y16 z16]×count` (~6 bytes/light). Data source: `Layouts::forEachCoord`.
- Browser positions preview points from the table instead of the grid formula; per-frame frames stream RGB-only indexed by light.
- PreviewDriver downsample switches to index-based striding (simpler, correct for any shape).

---

## Testing

### Additional test coverage (pending)

- **UI page load time** — scenario step measuring HTTP response time for `/`, `/api/state`, `/api/system` via the live runner. Verifies acceptable load time on ESP32.
- **Module teardown memory** — scenario that tears down all modules and verifies heap returns to pre-setup baseline. Confirms no lifecycle leaks.
- **JavaScript test harness** — `vitest` or `node --test` with `jsdom` for pure helpers in `release-picker.js` (`isCompatible`, `parseBoardsFromAssets`, `relativeTime`). Deferred until a second non-trivial JS module lands — one file doesn't justify the toolchain weight.

---

## Housekeeping

### ESP-IDF version pinning (pending)

Check whether `setup_esp_idf.py` pins to a specific commit/tag or always pulls latest. If latest, running "Setup ESP-IDF" in MoonDeck silently changes the IDF version and may break the build. Pin to the tested version (`v6.1-dev-399-gd1b91b79b`) or document that updates require re-testing.

### WiFi runtime disable (backlog)

Compile-time answer already ships: `--board esp32-eth` excludes the WiFi stack. This item is the runtime variant — a single `esp32-eth-wifi` binary that skips WiFi init when Ethernet hardware is present. Prerequisite: `platform::ethPresent()` / `platform::wifiPresent()` (listed under Release 2.0 above). Defer until that API lands.
