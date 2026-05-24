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
- **OTA / FirmwareUpdateModule.** Re-flashing via the web installer works in 1.0 but requires a USB cable. Port the passive-observer pattern from projectMM-v1 — pulls release JSON from GitHub, surfaces availability in the UI, applies on user confirm.
- **Linux desktop binary** in `release.yml` (third desktop job). Static-linked libstdc++ where the host allows.
- **Teensy 4.1 release binary.** Toolchain-file build, packaged as `.hex` for Teensy Loader.
- **Raspberry Pi binary.** ARM64, cross-built or native depending on what the runner offers.
- **Nightly CI / pre-release channel.** A second workflow on a schedule that produces unstable binaries, separate from the tag-driven `release.yml`.
- **Improv WiFi.** One-step flash + WiFi credentials from the browser, eliminating the SoftAP detour. ESP Web Tools supports the Improv handshake natively.
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

## HTTP file serving blocks the render tick (follow-up)

The ESP32 tick-variability swing (FPS collapse when a browser connected) was traced to the blocking 49 KB preview WebSocket broadcast and **fixed** — see `docs/performance.md` "ESP32 tick variability". A lesser, one-shot version of the same issue remains: `HttpServerModule::handleConnection()` serves the embedded UI files (`app.js`, `style.css` — tens of KB) with the plain blocking `TcpConnection::write`, so a page load can briefly stall `loop20ms`. It's one-shot per load rather than per-tick, so lower priority. Fix when convenient: serve large HTTP responses with the same non-blocking `writeChunks` path, or chunk the response across ticks.

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
