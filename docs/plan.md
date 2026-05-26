# What to build next

Completed items are removed. This file is deleted when empty.

## Windows desktop port (blocker for 1.0 Windows binary)

`scripts/build/package_desktop.py` configures + builds + packages on Windows runners successfully through CMake configure, but `src/platform/desktop/platform_desktop.cpp` won't compile under MSVC. It includes POSIX socket headers (`<sys/socket.h>`, `<netinet/in.h>`, `<arpa/inet.h>`, `<unistd.h>`, `<fcntl.h>`) and calls POSIX-only APIs:

- `::socket` / `::connect` / `::bind` / `::accept` exist on both — but Windows needs them via `<winsock2.h>` + `<ws2tcpip.h>`.
- `::read` / `::write` on socket fds — Windows uses `::recv` / `::send` (file-descriptor I/O doesn't bridge to sockets).
- `::close(fd)` — Windows uses `::closesocket(SOCKET)`.
- `::sendmsg(fd, msghdr, MSG_DONTWAIT)` for non-blocking scatter-gather — no direct Windows equivalent; closest is `WSASend(SOCKET, WSABUF*, ...)` with `FIONBIO` mode set via `ioctlsocket`.
- `fcntl(F_GETFL/F_SETFL, O_NONBLOCK)` — Windows uses `ioctlsocket(FIONBIO)`.
- `errno` after socket calls — Windows uses `WSAGetLastError()`.
- WSAStartup must be called once before any socket use; WSACleanup once at shutdown.

The current `release.yml` `build-windows` job fails on the first source file (`platform_desktop.cpp` → `'sys/socket.h': No such file or directory`). The `release` job has `needs: build-windows`, so a tag push currently can't release: the matrix is half-broken.

Two ways to land the port:

1. **Conditional includes/typedefs in `platform_desktop.cpp`** — `#ifdef _WIN32 ... #else ... #endif` blocks around every socket call. Smallest diff, ugliest source. The platform-boundary rule keeps platform code inside `src/platform/`, so this is local damage.
2. **Split into `platform_desktop_posix.cpp` + `platform_desktop_windows.cpp`** — cleaner, ~2x file count, CMake picks which to compile per host. Mirrors how the ESP32 platform is separate from desktop today; honest separation between two genuinely different syscall worlds.

Either path: 2-3h of careful translation + Windows-side manual testing. The plan-17 CI scaffolding ships ready — once this port lands, `build-windows` flips green automatically and the v1.0.0 release can include the Windows zip.

---

## Release 2.0 — distribution catches up to the source tree

1.0 ships ESP32 firmware (4 variants) + macOS arm64 binaries (Windows once the platform port above lands). The source tree builds for Teensy, Raspberry Pi, ESP32-P4, and Linux too — distribution catches up here.

- **ESP32-P4** board variant. New chip target, new sdkconfig fragment, fits the existing `BOARDS` table in `scripts/build/build_esp32.py`.
- **Linux desktop binary** in `release.yml` (third desktop job). Static-linked libstdc++ where the host allows.
- **Teensy 4.1 release binary.** Toolchain-file build, packaged as `.hex` for Teensy Loader.
- **Raspberry Pi binary.** ARM64, cross-built or native depending on what the runner offers.
- **Installer UX polish** (plan-18 Phase 3). Clear "Pre-release (beta)" warning on RC/nightly picks, finer "do not install" affordance (yank-by-asset-tag instead of yank-by-release-deletion), manufacturer-friendly landing copy.
- **Runtime PHY / pin config** for Ethernet (see `WiFi runtime disable` below — same `platform::ethPresent()` hook). Replaces the build-time Olimex-pin baking in `sdkconfig.defaults.eth` with a runtime picker. Once this lands the `esp32-eth*` variants stop being Olimex-specific.
- **macOS code-signing.** Currently triggers Gatekeeper on first run; signed builds drop the "downloaded from internet" prompt.

---

## WiFi performance testing (pending)

Measure FPS over WiFi STA vs Ethernet at different LED counts. The 128×128 case is **done** — WiFi ArtNet is ~4× the Ethernet per-packet cost, ~7 FPS at 16K LEDs vs ~19 on Ethernet (see `docs/performance.md` "ArtNet over WiFi"). Remaining matrix:

- WiFi STA 64x64 (4K LEDs, 24 universes) — should be feasible
- WiFi STA 32x32 (1K LEDs, 6 universes) — baseline
- Compare each with Ethernet at the same grid size

This determines the practical LED limit for WiFi-only boards. The 128×128 result already says: recommend Ethernet (or the `eth-only` build profile) for large installations.

## Add real z-axis variation to 2D effects (pending)

Today only **NoiseEffect** and **PlasmaEffect** have z-aware math (declared D3). The other 10 effects are honest D2 — they iterate y,x and Layer::extrude duplicates the z=0 plane across z on 3D layers. Visually this means every z-slice is identical; the effect looks "flat" along z.

Some of these could be promoted to genuine D3 with a small math change:
- **Metaballs / GlowParticles**: add a z coordinate to each blob; the field summation already generalises.
- **Plasma palette / Spiral**: add a z-driven phase term (Plasma already shows the pattern with its 5th z-driven sine).
- **Fire**: heat could rise along y but also drift along z (e.g. for a chimney effect). Needs a z-aware heat grid (`w × h × d` instead of `w × h`).
- **Ripples / LavaLamp / Checkerboard / Particles**: each ripple/blob/checker/particle gets a z coordinate.

Prioritise after we see real 3D installations. On 2D layers (today's reality) these are visually correct as-is. Each effect that gets promoted to D3 also needs its `dynamicBytes` resizing back up to the full 3D buffer.

## Additional testing (pending)

- **UI page load time**: add a scenario step that measures HTTP response time for `/` (index.html), `/api/state`, `/api/system` using the live runner's HTTP client. Verifies the web UI loads within acceptable time on ESP32.
- **Module teardown memory**: add a scenario that tears down all modules (`DELETE /api/modules/*`) and verifies heap returns to pre-setup baseline. Confirms no memory leaks in the full lifecycle.
- **JavaScript test harness (2.0 roadmap)**: there is no JS unit-test runner today. Pure helpers in [src/ui/release-picker.js](../src/ui/release-picker.js) (`isCompatible`, `parseBoardsFromAssets`, `relativeTime`) and any future picker logic are exercised ad-hoc from DevTools. Adding `vitest` or `node --test` with `jsdom` would cost ~2 h and would mirror the host-side test pattern the C++ side already has at `test/test_improv_frame.cpp`. Deferred until a second non-trivial JS module lands — one file doesn't justify the toolchain weight.

## mDNS toggle (evaluate)

The mDNS checkbox in NetworkModule was added as a diagnostic tool during performance investigation. Testing showed mDNS has zero FPS impact (the issue was a leaked WiFi task, not mDNS). Evaluate whether to keep the toggle (useful for debugging on other boards) or remove it (unnecessary complexity). Decision after WiFi performance testing.

## ESP-IDF version pinning (pending)

The `setup_esp_idf.py` script currently clones or pulls the latest from the ESP-IDF repo. Need to check: does it pin to a specific commit/tag, or does it always get latest? If latest, running "Setup ESP-IDF" in MoonDeck will silently change the IDF version, potentially breaking the build. Should pin to the tested version (`v6.1-dev-399-gd1b91b79b`) in the setup script or document that updates require re-testing.

## WiFi runtime disable (backlog)

Postponed. A **compile-time** answer already ships: the `esp32-eth` board (`build_esp32.py --board esp32-eth`) excludes the WiFi stack entirely. This item is the *runtime* variant — a single `esp32-eth-wifi` binary that detects at boot whether WiFi is needed and skips bringing it up. The default firmware ships the WiFi stack regardless (the app partition has room for it to live unused).

Open design question to address when this is picked up: can the platform detect at runtime whether Ethernet hardware is present (PHY responds on MDIO during `esp_eth_driver_install`)? If yes, the UI can hide WiFi controls — and skip `wifiStaInit()` — when Ethernet hardware is detected. That's a behavior-driven gate rather than a user toggle. Some ESP32 variants (e.g. ESP32-C2, ESP32-H2) don't have WiFi hardware at all, so the gate also needs to handle "WiFi not present" cleanly. Both detections live in `src/platform/`.

Planned API for the presence gate: `platform::ethPresent()` and `platform::wifiPresent()` (return bool from a quick, idempotent probe). When implemented, NetworkModule's `onBuildControls()` uses them to flip the `hidden` flag on Ethernet/WiFi-specific controls so absent interfaces don't show as "no link" / "no IP" in the UI.

## Multi-Layer composition (backlog)

The top-level shape now reads `Layouts → Layers → Drivers`. The `Layers` container holds N layers today (one by default); the `Drivers` container reads from a single *active* layer via `setLayer()`. Per-layer composition into a single output buffer is the pending feature — without it, additional layers render their buffers but only the first enabled layer reaches the output.

When picked up:

- `Drivers::loop()` reads from the `Layers` container instead of a single `Layer*`. For each enabled Layer, blend its buffer into the shared output buffer using the Layer's blend mode + opacity (controls to be added on Layer).
- The `Layer::startX/Y/Z` / `endX/Y/Z` controls — already persisted today, no-op with one Layer — become active in `rebuildLUT`: each Layer carves a region of the shared Layouts. Values are **percentages** of the physical extent on each axis. Defaults `start = 0, end = 100` = full layout; negatives and values > 100 are reserved for modifier shift. Start rounds toward floor, end rounds toward ceiling so small panels still get a non-zero region.
- `DriverBase::setLayer` stays as-is — drivers still output to one physical fixture and need that fixture's dimensions; the *active* Layer is what they query. Composition happens upstream in the Drivers container.
- Per-Layer enable/disable from the UI (already supported by `MoonModule::enabled`); ordering via the child-array order of Layers (already supported by drag-reorder).
- Memory-aware allocator: at `onAllocateMemory` time, decide how many Layers actually fit and degrade gracefully when PSRAM is absent.
- Persistence already encodes the Layers container's children positionally — adding more siblings to Layers just works on the file-format side.

## Memory ceiling: default grid + Ethernet + WiFi cascade fails on non-PSRAM (backlog)

On the Olimex ESP32-Gateway running `--board esp32-eth-wifi` with the default 128×128 grid:

- Free heap at boot: **~28 KB**, largest contiguous block ~14 KB
- Layer footprint: pixel buffer `16384 × 3 = 49 KB` + MappingLUT `(16385 + 16384) × sizeof(uint16_t) ≈ 64 KB` = **~113 KB**
- Drivers buffer: ~48 KB
- Preview buffer: ~5 KB
- IDF tasks/stacks/TCP-IP/Ethernet driver: ~120 KB of the ESP32's ~290 KB DRAM
- **Remaining for WiFi init**: not enough; `esp_wifi_init` needs to claim `10 × 1600 B = 16 KB` of RX buffers, can't, returns `ESP_ERR_NO_MEM`

The cascade path is: boot → Ethernet starts → "link up" briefly → "link down" (no cable / no DHCP) → cascade to WiFi → `esp_wifi_init` runs out of memory. Before today's fix, `ESP_ERROR_CHECK` panicked the device into a reboot loop. Today the failure is reported and the device stays running on whatever connection it has (Ethernet, if available; nothing, if not).

Real fix options, in increasing scope:

- **PSRAM for the Layer buffer + LUT.** ESP32-Gateway has 4 MB PSRAM (unused today on non-S3 builds). `platform::alloc` could prefer PSRAM for buffers larger than some threshold. Pixel buffer at 49 KB and LUT at 64 KB are both clear candidates — moves the bulk of light-pipeline memory out of DRAM and frees ~110 KB for radios. The render loop reads/writes these buffers every tick; PSRAM is slower than DRAM (~12 MB/s vs ~80 MB/s for sequential), so this needs to be measured. Likely ~25% FPS hit on a 16K grid (already at 13 FPS — would drop to ~10).
- **Persisted-state default cap.** Today the default grid is 128×128. On non-PSRAM ESP32 with both Ethernet *and* WiFi enabled, that's over the ceiling. Either drop the default to 64×64 on the esp32-eth-wifi build (32 KB Layer + ~16 KB LUT = 48 KB, comfortably under) or warn at first boot if the persisted grid would breach a known safe ceiling.
- **Lazy WiFi init.** The cascade currently `esp_wifi_init`s on Ethernet drop, *always*. If `ssid_` is empty (no credentials stored), the WiFi-STA attempt is pointless; we know it can't succeed. Currently the code goes straight to AP-fallback in that case, but AP-fallback also calls `ensureWifiInit`. Same memory cost. Genuine fix would be to not even attempt WiFi init when no credentials AND no AP-fallback request is pending — but the AP-fallback IS the only way to get credentials onto a fresh device, so this only helps on devices that have credentials but the network is unreachable. Niche.

Path-of-least-surprise: cap the default grid to fit, document the ceiling. The PSRAM offload is more invasive (requires touching `platform::alloc` semantics + measuring the FPS impact) and is on the same axis as plan-10's persistence work.

## POST /api/firmware — direct binary upload OTA (backlog)

Today the only HTTP-driven flash path is `POST /api/firmware/url` — device fetches a binary from a public URL (currently always a GitHub release asset). That covers the end-user "update to a released version" use case (the picker in the Firmware card) but not the developer workflow: build HEAD locally → flash to a board on the LAN → iterate, without round-tripping through `esptool` over USB or publishing to GitHub.

This was deliberately scoped out of [plan-18](history/plan-18.md) ("File-upload OTA route is skipped. Picker drives /api/firmware/url only") because the picker UI didn't need it. Two later use cases pulled it back into scope:

- **Dev-flash from a local build** — `curl -X POST --data-binary @firmware.bin http://<device-ip>/api/firmware` triggers the OTA without esptool / USB / a public URL. Faster iteration loop. Also unblocks any agent / CI driver: a script on the LAN can flash the device the same way it would call `/api/control`.
- **Browser file-upload affordance** — drag a `.bin` onto the Firmware card and flash it. v1 had this; the picker today doesn't.

Same endpoint serves both: the browser sends `multipart/form-data`, a script sends `application/octet-stream`. Both stream into the same `pal::ota_write_chunk` path. The picker stays as the user-facing default; the file-upload affordance is a `<details>` expander on the Firmware card for "I have a local .bin I want to flash".

What needs to be built (≈4 h):

- **Platform layer** — `pal::ota_begin_streaming()` / `pal::ota_write_chunk()` / `pal::ota_finish_streaming()` in [src/platform/esp32/platform_esp32.cpp](../src/platform/esp32/platform_esp32.cpp). Wrap `esp_ota_*` directly (not `esp_https_ota_*`, which assumes the library does the fetch). These are already **declared** in [src/platform/platform.h](../src/platform/platform.h) as `ota_begin/ota_write/ota_end` per the original plan-18 design but never implemented; today the URL path uses `esp_https_ota_perform` end-to-end and bypasses them.
- **HTTP route** — `POST /api/firmware` in [src/core/HttpServerModule.cpp](../src/core/HttpServerModule.cpp). Reads `Content-Length`, loops `conn.read(chunkBuf, 4096)` → `pal::ota_write_chunk`, advances `g_otaPct`. On finish, `pal::reboot()`. **Cannot use the existing 2 KB stack request buffer** — must stream from the connection directly. The existing `/api/firmware/url` route shares `g_otaStatus` + `g_otaPct` so the UI shows the same progress for both paths.
- **`device_ip` in `/api/state`** — read-only control on [src/core/SystemModule.h](../src/core/SystemModule.h), populated in `loop1s()` from `platform::ethGetIP` / `wifiStaGetIP` (the same source `updateStatusIP()` uses for the status chip). Lets a script discover the device without needing mDNS to be reachable from the dev machine.
- **`scripts/build/flash_over_network.py`** — pyserial-style CLI mirror: `--host <ip>` or `--mdns <hostname>`, finds the most recent `.bin` in `esp32/build/`, POSTs it, polls `/api/state` for `update_status` until `rebooting`. Plus a MoonDeck button alongside the existing Flash.
- **UI file-upload affordance** — small change to `src/ui/release-picker.js` (or a new sibling) to add a `<input type="file" accept=".bin">` that POSTs to `/api/firmware`. Hidden behind a `<details>` since the picker is the everyday path.

Deferred to keep this iteration narrow: the picker + URL path is working now, and the dev-flash workflow has the `flash_esp32.py` (USB) fallback. Reopens when a second use case lands or when the iteration time on `esptool reset → flash → reboot` becomes annoying enough to fix. Reference: [plan-18 Notes](history/plan-18.md) — "File-upload OTA route … is skipped. Picker drives `/api/firmware/url` only. v1 had both."

## Post-merge cleanup: remove temporary `plan-18` carve-outs (follow-up)

Two places carry a temporary `plan-18`-named allowance that should be removed once PR #7 merges and the branch is deleted. Both were added so PR #7's CI + Pages deploys could run before merge; neither belongs in steady-state.

1. **`.github/workflows/release.yml`** — the `push: branches:` allowlist contains `plan-18` (added in commit `3e2acb5`, with a comment noting "remove when the PR merges"). Drop that line so the allowlist returns to `main` + `next-iteration`.

2. **GitHub Pages environment branch-policy** — the `github-pages` environment was extended to allow deploys from `plan-18` (was previously `main`-only). The deployment-branch-policy id is **`50239266`**. Remove via:

   ```bash
   gh api -X DELETE \
     repos/ewowi/projectMM/environments/github-pages/deployment-branch-policies/50239266
   ```

   Or via UI: Repo Settings → Environments → `github-pages` → Deployment branches → remove the `plan-18` rule.

Verify after cleanup: a `gh api repos/ewowi/projectMM/environments/github-pages/deployment-branch-policies` call should show only `{ name: main, type: branch }`. The `branches:` allowlist in `release.yml` should match the old plan-17 pattern.

If a future feature branch needs the same allowances, add them under that branch's name and follow this same cleanup pattern on its merge — or set up a `next-iteration`-style convention where every feature branch uses one of the standing names (so neither allowlist needs editing per-PR).

## Build-variant + board WiFi performance matrix (follow-up)

Measured 2026-05-25, default 128×128 grid, `MoonModules` SSID, post-Improv-provisioning. Same source tree, same MCU (ESP32 classic, 160 MHz):

| Board | Build | Tick / FPS | `Drivers/ArtNetSend` | `/api/state` | `/app.js` (77 KB) |
|---|---|---|---|---|---|
| Olimex Gateway | `esp32` (WiFi-only) | **220 ms / 4 FPS** | **155 ms** | 0.22 s | 0.29 s |
| Olimex Gateway | `esp32-eth-wifi` | 85-95 ms / 10-12 FPS | 38 ms | 4.34 s | 1.47 s |
| Generic ESP32 board | `esp32` | 100 ms / 10 FPS | 45 ms | timeout | 32 s stall |
| Generic ESP32 board | `esp32-eth-wifi` | 82-97 ms / 10-12 FPS | 28-45 ms | timeout | partial (28 KB in 10 s) |

Two distinct effects visible:

**1. Olimex `esp32` build is 4× slower at Art-Net than the same board's `esp32-eth-wifi`.** Both builds share `sdkconfig.defaults`; the eth variant adds `sdkconfig.defaults.eth` which only touches `CONFIG_ETH_*`. Yet Art-Net send goes from 38 ms to 155 ms. Hypothesis: `CONFIG_ETH_DMA_*` indirectly enlarges a shared LWIP/WiFi buffer pool, so the WiFi-only build runs out of TX buffers and stalls. Lowest-risk fix: bump WiFi TX buffer count in `sdkconfig.defaults` so both variants get the same headroom. Investigation needs `vTaskList` + `heap_caps_print_heap_info` snapshots in both builds.

**2. Generic ESP32 board has much weaker WiFi throughput than the Olimex.** Same `esp32-eth-wifi` build serves the 77 KB `/app.js` in 1.47 s on Olimex vs only 28 KB of it in 10 s on the generic board — **>7× difference in raw WiFi TX**. Independent of the build, this is PCB-antenna and power-supply quality. Practical implication: classic-ESP32 devboards vary wildly. The Olimex Gateway (proper PCB-trace antenna, regulated 3V3) is the reference; generic boards may need Ethernet or be limited to small grids.

Cross-link: line 100 ("Memory ceiling: default grid + Ethernet + WiFi cascade fails on non-PSRAM"). The Olimex `esp32` Art-Net slowdown is likely the same root cause — both point to network-stack memory being marginal on classic ESP32 when both stacks are unused but compiled in.

Practical guidance until fixed: **use `esp32-eth-wifi` for any Art-Net workload on classic ESP32**, even if Ethernet isn't physically connected. The plain `esp32` build is best reserved for UI-only / smaller-grid WiFi use cases.

## Platform API: migrate pointer+size pairs to `std::span` (backlog)

CodeRabbit flagged the post-plan-18 platform.h surface for using `(buf, len)` pairs in several places where `std::span<char>` / `std::span<uint8_t>` would let the compiler catch length/pointer mismatches. Concrete sites:

- [src/platform/platform.h](../src/platform/platform.h) — `http_fetch_to_ota(url, statusBuf, statusBufLen, bytesReadOut, bytesTotalOut)` and friends.
- [src/platform/platform.h](../src/platform/platform.h) — `improvProvisioningInit(info, ready, statusBuf, statusBufLen, ssidOut, ssidOutLen, passwordOut, passwordOutLen)`.
- `TcpConnection::read/write` already take `(uint8_t*, size_t)`; keeping those raw is fine (matches the lwip / Berkeley sockets shape) but if we do a span pass, doing it consistently everywhere is the right scope.

Not RC2 / not plan-18 work — touches every caller (modules, scenarios, tests). Worth doing alongside the next platform-API expansion (e.g. when adding Windows desktop sockets per the Windows port, or POST /api/firmware streaming). Estimate ~2 h including ripple updates.

## Improv as a child of NetworkModule (deferred — needs scheduler work first)

Improv depends on WiFi and has no meaning without it; expressing that
dependency as a parent-child relationship in the module tree is the
architecturally right shape (the Network card on the UI shows Improv
as a sub-row, similar to how Layers sits under Layouts). The bespoke
`setNetworkModule(networkModule)` pointer-passing becomes a `parent()`
read.

Attempted in plan-21 (a 30-min spike, 2026-05-26), reverted because
the change crosses load-bearing scheduler/MoonModule infrastructure
that isn't ready for it yet:

- `Scheduler::tick()` only walks **top-level** modules for `loop20ms` /
  `loop1s`. Children don't get those ticks. Today's container modules
  (Layouts / Layers / Drivers) only need `setup` / `onBuildControls` to
  propagate (the base class default does that); they don't have any
  per-tick work that runs on children.
- `Scheduler::setup` calls each top-level module's `setup` which the
  base default propagates to children — BUT every NetworkModule
  override of `setup` / `loop1s` / `onBuildControls` / `teardown`
  needs to chain to the base, otherwise children silently miss every
  one of those callbacks.

The minimum-scope refactor to make this safe:

1. **Scheduler walks children too** for `loop20ms` and `loop1s` — or
   the base `MoonModule::loop20ms` / `loop1s` propagate to children
   (today they're no-ops). Pick the place that costs the least at runtime.
2. **Every existing override** of `setup` / `loop1s` / `loop20ms` /
   `onBuildControls` / `teardown` audited to confirm it chains to the
   base. NetworkModule, SystemModule, FilesystemModule, HttpServerModule
   all need this check.
3. **Then** move Improv under Network — at that point it's a one-line
   `networkModule->addChild(improvModule)` swap for the top-level
   `scheduler.addModule(improvModule)`, and the lifecycle just works.

Estimate ~2 h for the whole sequence. Not RC2 work; defer until after
the plan-18/19/20 PR-merge into main.

## HTTP file serving blocks the render tick (follow-up)

The ESP32 tick-variability swing (FPS collapse when a browser connected) was traced to the blocking 49 KB preview WebSocket broadcast and **fixed** — see `docs/performance.md` "ESP32 tick variability". A lesser, one-shot version of the same issue remains: `HttpServerModule::handleConnection()` serves the embedded UI files (`app.js`, `style.css` — tens of KB) with the plain blocking `TcpConnection::write`, so a page load can briefly stall `loop20ms`. It's one-shot per load rather than per-tick, so lower priority. Fix when convenient: serve large HTTP responses with the same non-blocking `writeChunks` path, or chunk the response across ticks.

## Task core-pinning (backlog)

ESP32 has two cores; projectMM currently doesn't pin any of its FreeRTOS tasks. The render task runs wherever `app_main` ends up (core 0 by default + FreeRTOS load-balancing); the OTA task (`urlOta`) and Improv task (`improv`) added in plan-18 are created with `xTaskCreate` (unpinned). WiFi internals are already pinned to core 0 via `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y`.

At 16K LEDs (the current performance ceiling) the render task already takes ~52 ms / tick. If we hit cross-core contention symptoms — tick variance climbs, FPS jitter rises when a long-running task (OTA download, Improv scan) is active — the fix is explicit pinning:

- Render task → core 1 (away from WiFi's core 0).
- OTA task + Improv task → core 0 (network-adjacent; WiFi is there anyway).

Tools: `xTaskCreatePinnedToCore` instead of `xTaskCreate`. The platform-layer task-spawn sites are in `src/platform/esp32/platform_esp32.cpp` (search for `xTaskCreate`); the render task is created by Scheduler — would need plumbing to set affinity at construction.

Defer until performance data shows actual contention. Today neither OTA nor Improv runs during normal device operation (OTA = explicit user action; Improv = idle UART-blocked task), so the steady-state contention picture is zero.

## Preview coordinate message — true-shape 3D preview (backlog)

The 3D preview currently positions every voxel by deriving `(x, y, z)` from a dense grid index (`ix/maxDim` etc. in `app.js renderPreviewFrame`). This only works for **grid** layouts. For a **sparse / non-grid 3D layout** — rings, spheres, a dodecahedron of LED rings, arbitrary point clouds — the physical light positions are not a regular grid, so the preview cannot show the true shape. `PreviewDriver`'s downsample is now crash-safe for sparse layouts (light index bounded by the real light count) but still previews them as their dense bounding box, which is wrong: e.g. 8 rings of 24 LEDs in a 20×20×20 space (192 lights) would render as a clump in one corner of the box, not as 8 rings.

Motivating use case: a layout shaped like a Gigaminx (12-face dodecahedron), each pentagonal face tiled with rings of 24 LEDs, positioned in true 3D space.

The architecture's intended solution (already noted in `docs/moonmodules/light/drivers/PreviewDriver.md`): a **one-time coordinate message**. When picked up:
- The engine sends, once per layout change and once to each newly-connected WebSocket client, a coordinate table — the real `(x, y, z)` of every light. The data already exists: `Layouts::forEachCoord(callback, ctx)` yields `(index, x, y, z)` per light (it's how `Layer::onAllocateMemory` computes the bounding box).
- A new binary WS message type (the preview frame is `[0x02]…`; allocate `[0x01]` or `[0x03]` for coordinates). Format roughly `[type][count16][x16 y16 z16]×count` — `lengthType` is int16, so 6 bytes per light.
- The browser caches the coordinate table and positions preview points from it instead of deriving from a grid index. Per-frame binary frames then stream **only RGB**, indexed by light — for the ring example that is 192×3 ≈ 576 bytes/frame, tiny and fast.
- `PreviewDriver`'s downsample should switch to **index-based** striding (stride over the light index, not the x/y/z box) once coordinates drive the display — simpler and correct for any shape.
- Re-send the table when the layout changes (a hook on layout-control change / `Scheduler::rebuild`) and when a new WS client connects (a per-client "needs coordinates" flag, or just resend to all on connect).
- Keep the grid fast-path: a pure grid layout can still use the derived-position path (no coordinate table needed) to save the one-time transfer — or always send coordinates for uniformity; decide when planning.
