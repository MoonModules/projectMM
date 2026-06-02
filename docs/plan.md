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

### `esp32-eth` slow Ethernet bring-up vs `esp32-eth-wifi` (investigation)

On Olimex ESP32-Gateway flashed with `esp32-eth`, Ethernet sometimes takes **a minute or more** to acquire a DHCP lease at boot. The same hardware flashed with `esp32-eth-wifi` brings Ethernet up in seconds. The B1 Idle-recovery fix in `src/core/NetworkModule.h` masks the symptom (status correctly transitions to "Eth: <ip>" once the lease arrives), but the underlying slow bring-up is a real performance regression on the eth-only build.

What we know:
- `build/esp32-esp32-eth/sdkconfig` and `build/esp32-esp32-eth-wifi/sdkconfig` are **byte-identical** (3,617 lines each, `cmp -s` confirms). So lwIP buffer pools, DHCP timeouts, and Ethernet driver settings are the same.
- Same hardware (Olimex ESP32-Gateway Rev G), same RMII pin/clock config (`EMAC_CLK_OUT` on GPIO17), same `ethInit()` code in `src/platform/esp32/platform_esp32.cpp`.
- The only difference at link time: `esp32-eth` passes `EXCLUDE_COMPONENTS=esp_wifi;wpa_supplicant;esp_coex` to ESP-IDF (see `scripts/build/build_esp32.py:31`).
- `esp_coex` (WiFi/Bluetooth coexistence) is normally responsible for shared-radio timing arbitration. Hypothesis: even though Ethernet doesn't share the radio, something in `esp_coex` or its early init dependency chain warms a clock path (the shared 26 MHz crystal feeding both WiFi PHY and EMAC PLL) that helps Ethernet auto-negotiation. With it excluded, the EMAC takes longer to stabilise.

What we don't know:
- Which init step actually consumes the time — link-up (PHY negotiation) vs DHCP (lwIP) vs both?
- Whether the slow path is reproducible (does it always happen, or only on cold boot / after a reset / etc.)?

Diagnostic to run when picking this up: flash both variants, capture `idf.py monitor` from boot to "got IP" on each, diff the timestamps of `Ethernet link up` and `Ethernet got IP` ESP_LOGI lines. If link-up is fast but DHCP is slow → lwIP init issue (look at `esp_netif_init`'s side effects from excluded components). If link-up itself is slow → PHY/clock path (try re-adding only `esp_coex` to see if it helps).

### NoiseEffect cost on ESP32 (investigation)

At 128×128 with mirror XY, NoiseEffect renders a 64×64 logical area but still costs **~47 ms/tick** on `esp32-eth-wifi` (Olimex Gateway, 160 MHz) — ~11.5 µs per pixel for 4,096 pixels. That's 55% of the total ~85 ms tick, capping the workload at ~12 FPS. By comparison RainbowEffect on the same pipeline hits ~22 FPS — the simplex math is the dominant cost.

To reach 18 FPS at 128×128 with mirror + Noise (matching the historic Rainbow headline), total tick must drop to ~56 ms. ArtNet alone is ~28 ms, so Noise needs to drop from 47 ms to ~28 ms — a ~40% cut on the effect itself.

Worth investigating:

- **Q16 fixed-point simplex** instead of float (Xtensa LX6 has no FPU; float math is software-emulated).
- **Lower-precision hash** — current simplex uses a 256-entry permutation lookup; a smaller / SIMD-friendly hash may be faster on Xtensa.
- **Strided sampling + interpolation** — render at 32×32, bilinear up to 64×64. Visual quality cost; needs A/B comparison.
- **Inline / unroll the inner per-pixel loop** to keep the simplex state in registers.

None of these are obviously free. Reaching 18 FPS may require accepting a visual signature change. Defer until there's a real use case (today's "12 FPS at heaviest workload" is fine for the intended deployment scale).

### MoonDeck doc-asset endpoint hardening (backlog)

`scripts/moondeck.py::_serve_doc_asset` accepts any ROOT-relative path and serves the file. Path traversal *is* blocked (`asset_path.relative_to(ROOT.resolve())`), but inside the repo any file is served — including local-only artefacts like `scripts/build/wifi_credentials.json` if present. MoonDeck binds to all interfaces by design (the existing comment in `main()` explicitly enables LAN reach), so anyone on the LAN can hit the endpoint.

Two improvements when this matters:
- **Subdirectory whitelist** — only serve under `docs/` (and image asset paths the markdown renderer needs). Reject `scripts/build/wifi_credentials.json` etc. with 403.
- **Extension whitelist** — only image / CSS / JS mime types via a small allowlist.
- **Optional bind-to-localhost flag** — `--bind 127.0.0.1` for users who don't want LAN reachability. Default stays "" (all interfaces) since the LAN-reach is the documented design.

Not blocking — MoonDeck is a developer tool, not a production server. Pick this up when MoonDeck is in scope for hardening.

### mDNS toggle (evaluate)

Added as a diagnostic tool during performance investigation; testing showed mDNS has zero FPS impact. Evaluate whether to keep (useful for debugging on other boards) or remove (unnecessary complexity). Decide after WiFi performance testing above.

### Memory ceiling on non-PSRAM ESP32 with eth-wifi (backlog)

On `esp32-eth-wifi`, default 128×128 grid, free heap at boot is ~28 KB — not enough for `esp_wifi_init` (needs ~16 KB RX buffers) after the light pipeline allocates ~210 KB. The device stays running but WiFi init fails silently.

Fix options in increasing scope:
- **Cap the default grid** — drop to 64×64 on `esp32-eth-wifi` (Layer ~32 KB + LUT ~16 KB = 48 KB, comfortably under). Simplest.
- **PSRAM for Layer buffer + LUT** — ESP32-Gateway has 4 MB PSRAM unused on non-S3 builds. Moving the 49 KB pixel buffer + 64 KB LUT out of DRAM frees ~110 KB for radios. Cost: ~25% FPS hit (PSRAM bandwidth ~12 MB/s vs DRAM ~80 MB/s); needs measurement. See [decisions.md](history/decisions.md) "Adaptive memory allocation design" for the allocation rules.
- **Lazy WiFi init** — skip `esp_wifi_init` when `ssid_` is empty and no AP-fallback is pending. Helps only when credentials exist but the network is unreachable — niche.

### Mirror LUT silently degrades at 128×128 on no-PSRAM ESP32 (regression — open)

The Layer mirror LUT at 128×128 needs ~72 KB contiguous (4097-entry offsets table + 32768-entry destinations, each `uint16_t`). On Olimex Gateway Rev G the largest contiguous block at runtime is **~52 KB on `esp32-eth-wifi`** and **~53 KB on `esp32-eth`** — see the `observed.<target>.max_alloc_block` field in `test/scenarios/light/scenario_GridLayout_grid_sizes.json` `size-128x128`. Removing the WiFi stack reclaims ~36 KB of *free* heap but only ~4 KB of *contiguous* heap, because the dominant fragmenters are our own allocations (49 KB Layer logical buffer + 49 KB Driver output buffer), not WiFi.

Result: `Layer::rebuildLUT` hits the `canAllocate(72 KB)` failure path, sets status `"modifier LUT skipped — not enough memory"` (severity warning), and degrades to 1:1 mapping. **Mirror has no visible effect at 128×128** on either Olimex build. At 64×64 the LUT only needs ~18 KB, easily fits. At 128×64 the LUT needs ~36 KB, also fits.

**This is a regression.** Commit `7763bf8` ("Add eth-only build, fix FPS swing, optimize ArtNet") documented `128×128 grid, mirror XY, noise effect, Ethernet` with `Noise effect | 11,200 µs | 16% | 4096 logical pixels` and total tick 69,000 µs (14 FPS, free heap 124 KB). That `4096 logical pixels` line proves the mirror LUT was applied — Noise rendered the quadrant, not the full grid. Today the same workload measures `47,000 µs` for NoiseEffect (rendering all 16,384 pixels, because the LUT degraded), and free heap is down to ~97 KB. Net regression: ~27 KB of free heap lost since `7763bf8`, mirror no longer applied, Noise pays 4× the per-pixel cost.

Suspect additions between `7763bf8` and today:
- `aaf8d98` Per-driver output correction (Correction stage in Drivers path; allocates the 49 KB output buffer that didn't exist before).
- `4cdc09a` Plan-18: release-channel picker + OTA + Improv WiFi + post-flash fix-pack (FirmwareUpdateModule, ImprovProvisioningModule).
- HTTP server endpoints accumulated over the same span (WebSocket buffers, route handlers, mDNS service registrations).

The contiguous-block deficit is structural now: even with mDNS off + Preview disabled + Improv deleted at runtime, max block stays at 53 KB (verified live on Olimex 2026-06-02). So toggling features off won't restore it — the per-driver Correction stage in `Drivers.h` lines 92-104 is the dominant non-removable consumer.

Fix options:
- **Allocate LUT before Driver output buffer** in `Layer::rebuildLUT()` / `Drivers::onBuildState()` — claim the larger contiguous chunk while heap is still less fragmented at startup. Requires reshuffling allocation order (Drivers currently allocates `outputBuffer_` in its own `onBuildState`, independently of Layer). Low risk; should let mirror work at 128×128 on eth-only at least.
- **Smaller LUT representation for symmetric mirrors** — a "1:1 + axis-mirror suffix" encoding could compress the 72 KB LUT into ~4 KB (just the axis flags + an offset per logical pixel). Significant work but kills the problem outright for any mirror config.
- **PSRAM** — only helps on PSRAM-equipped boards (Olimex Gateway Rev G has none).

Tracked because surfacing the regression now (with max_alloc_block telemetry in scenarios) makes the next step concrete: pick one of the three options and measure.

### Preview memory optimizations (backlog)

- **Defer Preview allocation** — allocate the preview buffer on first WebSocket client connect; free on last disconnect. Saves ~5 KB when no browser is open.
- **Cap `detail` at 2 by default** — `detail = 3` adds ~14 ms/tick (see [performance.md](performance.md) "Preview detail cost"). Straightforward guard in PreviewDriver.

### Task core-pinning (backlog)

No FreeRTOS tasks are pinned today. At 16K LEDs the render task takes ~52 ms/tick; if OTA download or Improv scan causes tick-variance spikes, pin render → core 1, OTA/Improv → core 0 (where WiFi already lives via `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y`). Defer until contention is observed — neither OTA nor Improv runs during normal operation.

---

## Architecture

### Board vs firmware separation, runtime board presets (multi-commit, started)

Today `--board <variant>` in `scripts/build/build_esp32.py` actually picks a **firmware variant** (`esp32`, `esp32-eth`, `esp32-eth-wifi`, `esp32s3-n16r8`), not a physical board. The eth variants additionally hardcode Olimex Gateway RMII pins in `src/platform/esp32/platform_esp32.cpp::ethInit()`, so they only work on that one PCB. As we add boards (LOLIN D32 tested 2026-06-02, QuinLED variants planned), the conflation gets painful: every new board with different pins would need another firmware. The fix is to **separate physical-board metadata from firmware-variant metadata** and let the device pick up board-specific values (pin assignments, default module config) at runtime.

Started — minimal scope, no catalog yet:
- `scripts/moondeck.py::_probe_device` now reads `SystemModule.board` as a **firmware** value (the existing control is misnamed; rename in the final phase) and deduces the physical board where the firmware uniquely identifies hardware (`esp32-eth*` ⇒ `olimex-esp32-gateway-rev-g`). MoonDeck device-list shows `<deviceName> · <ip> · fw:<firmware> · board:<board>`.
- For firmware variants that work on multiple boards (`esp32`), the board picker in the UI lets the user select from a short hardcoded list (LOLIN D32, generic ESP32, Olimex Gateway when running the eth-less firmware, etc.). Selection persists in moondeck.json.
- `flash_esp32.py` polls for the just-flashed device after a successful flash and writes `last_port` into the matching device record, so MoonDeck shows which serial port a device was last flashed via.
- Firmware variants stay separate — `esp32-eth` saves ~670 KB flash + ~30 KB DRAM vs `esp32-eth-wifi` (measured); merging would erase that win.

Pin config moves to runtime (next, separate commit):
- Drop hardcoded `GPIO_NUM_17` from `ethInit()`. NetworkModule reads `Network.eth_rmii_clock_gpio` (new control) and similar pin values, defaulting to current Olimex hardcodes so behaviour is unchanged.
- Same for any other hardware-pin literal in the firmware.

Board preset catalog + upload (later, when the runtime config has real consumers):
- Add structured per-board files (location TBD — not `docs/` since they're config not docs; `boards/` at repo root is the strong candidate, matches the PlatformIO convention contributors will recognise).
- Each file declares chip, flash, PSRAM, Ethernet PHY + pins, default module config.
- New `/api/board-preset` endpoint accepts the JSON; device persists to LittleFS; bootstrap applies pins + defaults on next boot.
- MoonDeck "Set board" picker reads the catalog to populate the dropdown.
- Pin reassignment requires reboot (ESP-IDF can't hot-reconfigure EMAC pins after `esp_eth_driver_install`); document the constraint.
- A first attempt at this catalog landed and was rolled back in the started-scope commit — the catalog only earns its keep once the device reads it, otherwise it's a docs-shaped file in the wrong place.

Terminology cleanup (last, coordinated rename):
- `--board` arg in `build_esp32.py` → `--firmware` (and the `BOARDS` dict to `FIRMWARES`).
- `SystemModule.board` control → `firmware`.
- Scenario `contract.<target>` keys to match.
- No behavioural change; block-scope so one commit mechanically sweeps all three layers.

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
