# What to build next

Completed items are removed. This file is deleted when empty.

## 13. README + quick-start

Update README with: what it does now, how to build/flash, how to connect and open the UI. Include screenshots.

---

## Release 1.0 — "connect, open browser, see lights"

Milestone after items 11-13. An end user with an ESP32 can flash the firmware, connect via WiFi, open a browser, see the 3D preview, change effects and controls, and have settings persist across reboots.

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

Postponed. A **compile-time** answer already ships: the `eth-only` build profile (`build_esp32.py --profile eth-only`) excludes the WiFi stack entirely. This item is the *runtime* variant — a single default-profile binary that detects at boot whether WiFi is needed and skips bringing it up. The default firmware ships the WiFi stack regardless (the app partition has room for it to live unused).

Open design question to address when this is picked up: can the platform detect at runtime whether Ethernet hardware is present (PHY responds on MDIO during `esp_eth_driver_install`)? If yes, the UI can hide WiFi controls — and skip `wifiStaInit()` — when Ethernet hardware is detected. That's a behavior-driven gate rather than a user toggle. Some ESP32 variants (e.g. ESP32-C2, ESP32-H2) don't have WiFi hardware at all, so the gate also needs to handle "WiFi not present" cleanly. Both detections live in `src/platform/`.

Planned API for the presence gate: `platform::ethPresent()` and `platform::wifiPresent()` (return bool from a quick, idempotent probe). When implemented, NetworkModule's `onBuildControls()` uses them to flip the `hidden` flag on Ethernet/WiFi-specific controls so absent interfaces don't show as "no link" / "no IP" in the UI.

## Multi-layer pipeline (backlog)

Today `DriverGroup` holds one `Layer*` and `DriverBase::setLayer()` takes one layer. The architecture (`docs/architecture-light.md`) plans for multiple layers feeding one DriverGroup, with per-layer LUTs blended into a single output buffer. The number of active layers depends on available memory — a device with PSRAM can run many; a device without may be limited to one.

When picked up:
- `DriverGroup::passBufferToDrivers` composes/blends N layer buffers upstream (Buffer + Buffer with per-layer blend mode and opacity).
- `DriverBase::setLayer` stays as-is — drivers still output to one physical fixture and need that fixture's dimensions; the *active* layer is what they query. Multi-layer composition happens upstream of drivers.
- Per-layer enable/disable from the UI (already supported by `MoonModule::enabled`); ordering via existing child-array order.
- Memory-aware allocator: decide at `onAllocateMemory` time how many layers actually fit, degrade gracefully if PSRAM is unavailable.
- Persistence (plan-10) already encodes layers + their children positionally — adding more siblings to a LayoutGroup just works on the file-format side.

## HTTP file serving blocks the render tick (follow-up)

The ESP32 tick-variability swing (FPS collapse when a browser connected) was traced to the blocking 49 KB preview WebSocket broadcast and **fixed** — see `docs/performance.md` "ESP32 tick variability". A lesser, one-shot version of the same issue remains: `HttpServerModule::handleConnection()` serves the embedded UI files (`app.js`, `style.css` — tens of KB) with the plain blocking `TcpConnection::write`, so a page load can briefly stall `loop20ms`. It's one-shot per load rather than per-tick, so lower priority. Fix when convenient: serve large HTTP responses with the same non-blocking `writeChunks` path, or chunk the response across ticks.

## Preview coordinate message — true-shape 3D preview (backlog)

The 3D preview currently positions every voxel by deriving `(x, y, z)` from a dense grid index (`ix/maxDim` etc. in `app.js renderPreviewFrame`). This only works for **grid** layouts. For a **sparse / non-grid 3D layout** — rings, spheres, a dodecahedron of LED rings, arbitrary point clouds — the physical light positions are not a regular grid, so the preview cannot show the true shape. `PreviewDriver`'s downsample is now crash-safe for sparse layouts (light index bounded by the real light count) but still previews them as their dense bounding box, which is wrong: e.g. 8 rings of 24 LEDs in a 20×20×20 space (192 lights) would render as a clump in one corner of the box, not as 8 rings.

Motivating use case: a layout shaped like a Gigaminx (12-face dodecahedron), each pentagonal face tiled with rings of 24 LEDs, positioned in true 3D space.

The architecture's intended solution (already noted in `docs/moonmodules/light/drivers/PreviewDriver.md`): a **one-time coordinate message**. When picked up:
- The engine sends, once per layout change and once to each newly-connected WebSocket client, a coordinate table — the real `(x, y, z)` of every light. The data already exists: `LayoutGroup::forEachCoord(callback, ctx)` yields `(index, x, y, z)` per light (it's how `Layer::onAllocateMemory` computes the bounding box).
- A new binary WS message type (the preview frame is `[0x02]…`; allocate `[0x01]` or `[0x03]` for coordinates). Format roughly `[type][count16][x16 y16 z16]×count` — `lengthType` is int16, so 6 bytes per light.
- The browser caches the coordinate table and positions preview points from it instead of deriving from a grid index. Per-frame binary frames then stream **only RGB**, indexed by light — for the ring example that is 192×3 ≈ 576 bytes/frame, tiny and fast.
- `PreviewDriver`'s downsample should switch to **index-based** striding (stride over the light index, not the x/y/z box) once coordinates drive the display — simpler and correct for any shape.
- Re-send the table when the layout changes (a hook on layout-control change / `Scheduler::rebuild`) and when a new WS client connects (a per-client "needs coordinates" flag, or just resend to all on connect).
- Keep the grid fast-path: a pure grid layout can still use the derived-position path (no coordinate table needed) to save the one-time transfer — or always send coordinates for uniformity; decide when planning.
