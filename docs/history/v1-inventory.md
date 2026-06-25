# projectMM v1 Inventory (commit 54b50bc, release 1.4.0)

This is a throwaway reference document — not committed. Used to decide what to harvest for v3.

## Core

| File | Purpose | Harvest for v3? |
|------|---------|-----------------|
| Module.h | Base class, name/id/type | Pattern reference |
| StatefulModule.h | Lifecycle + controls (addControl binds to class variable by reference) | **Yes** — controls pattern is proven |
| ModuleManager.h | Registry, REST adapter, state persistence, dirty-flag debouncer | Overloaded (v1 lesson) — split concerns |
| Scheduler.h | Timing, loop dispatch | Pattern reference |
| Coord3D.h | 3D coordinate type | **Yes** — simple, keep |
| HttpServer.h | HTTP server wrapper | Pattern reference |
| WsServer.h | WebSocket server | **Yes** — v3 needs WebSocket |
| KvStore.h | Key-value persistence | Pattern reference |
| Logger.h | Logging | Pattern reference |
| TypeRegistry.h | Module type registration for dynamic creation | **Yes** — needed for UI type picker |
| ProducerModule.h | Base for effects (produces into Channel) | **Yes** — maps to EffectBase |
| ConsumerModule.h | Base for drivers (consumes from Channel) | **Yes** — maps to DriverBase |
| AppRoutes.h | REST API route definitions | Pattern reference |

## PAL

| File | Purpose | Harvest? |
|------|---------|----------|
| Pal.h | Platform abstraction (alloc, timing, GPIO) | **Yes** — same concept |
| FileSystem.h | File abstraction | Pattern reference |
| MemoryStats.h | Memory reporting | Pattern reference |

## Effects (light domain)

| Module | Controls | Description | Harvest? |
|--------|----------|-------------|----------|
| NoiseEffect2D | scale(1-32), speed(0-255) | 2D Perlin noise field | **Yes** |
| DistortionWaves2DEffect | freq_x(1-8), freq_y(1-8), speed(0-100) | Wavy distortion pattern | **Yes** |
| GameOfLifeEffect | seed(0-255), wraparound(toggle), hue(0-255) | Conway's Game of Life | **Yes** |
| LinesEffect | bpm(1-240) | Moving lines | **Yes** |
| RipplesEffect | speed(0-99), interval(1-254) | Ripple animation | **Yes** |
| SineEffect | frequency(1-20), amplitude(0-255) | Sine wave pattern | **Yes** |
| ArtNetInModule | universe_start(0-255) | ArtNet receive as effect | **Yes** |

## Drivers

| Module | Controls | Description | Harvest? |
|--------|----------|-------------|----------|
| ArtNetOutModule | universe_start(0-255), ip(text) | ArtNet UDP send | **Yes** |
| PreviewModule | logEveryN(1-1000) | WebSocket preview to UI | **Yes** |

## Modifiers

| Module | Controls | Description | Harvest? |
|--------|----------|-------------|----------|
| BrightnessModifier | frequency(1-20) | Brightness pulsing | **Yes** |

## Layouts

| Module | Controls | Description | Harvest? |
|--------|----------|-------------|----------|
| Layout.h | (abstract base) | Coordinate iterator base | Pattern reference |
| GridLayout | width(1-1024), height(1-1024), depth(1-32), serpentine(toggle) | 3D grid with serpentine support | **Yes** — serpentine is important |

## Layers (pipeline structure)

| File | Purpose | Harvest? |
|------|---------|----------|
| Channel.h | 3D RGB array with w/h/d metadata | **Yes** — maps to Buffer |
| EffectsLayer.h | Container for effects in a layer | Pattern reference |
| DriverLayer.h | Container for drivers | Pattern reference |
| RGB.h | Plain RGB struct, no FastLED | **Yes** — same as our Pixel/light value |

## System modules

| Module | Description | Harvest? |
|--------|-------------|----------|
| Network.h | WiFi mode selection | **Yes** |
| WifiSta.h | WiFi station mode | **Yes** |
| WifiAp.h | WiFi access point mode | **Yes** |
| Ethernet.h | Ethernet placeholder | Later |
| FileManagerModule.h | File browser | Later |
| SystemStatus.h | Memory, uptime, version display | **Yes** |
| TasksModule.h | FreeRTOS task monitoring | Later |
| DeviceDiscovery.h | UDP broadcast discovery | **Yes** |

## Frontend (single index.html, 1909 lines)

### UI structure
- Fixed status bar: brand, device name, WebSocket status dot, reconnect button
- Hamburger menu → side nav with module tree
- Module cards with controls, add/remove buttons
- **3D WebGL preview** with orbit camera (matrix math, vertex/fragment shaders)
- Log panel
- System status (memory, timing)
- Light/dark theme toggle
- Responsive (side nav auto-opens on wide screens)

### Key UI patterns
- **WebSocket-driven** (not polling) — real-time state updates
- **Type picker** — dropdown to add new modules by type
- **Dynamic control rendering** — `buildControl()` renders slider/toggle/text/color by type
- **Module tree** — hierarchical navigation with parent/child relationships
- **Drag reorder** — modules can be reordered within their parent
- **Reset-to-default** buttons per control
- **Preview canvas** — WebGL 3D rendering of light output in real-time

### Control types supported
- slider (float range)
- toggle (bool)
- text (string, used for IP addresses)
- (color picker exists in UI code but may not be fully wired)

### API shape (REST + WebSocket)
- `GET /api/types` — list available module types
- `GET /api/modules` — list all module instances with state
- `POST /api/modules` — create new module
- `DELETE /api/modules/:id` — remove module
- `PATCH /api/modules/:id/props/:key` — set control value
- WebSocket: pushes full state on change, receives light preview frames

### Detailed reverse-engineering (the v1 frontend at 1.4.0)

The v1 `index.html` was fully reverse-engineered. The notes below capture mechanisms worth not rediscovering and, where v3 chose differently, why. The **forward-looking** UI gap analysis (what to still adopt) lives in the [backlog](../backlog/README.md) (UI chapter of backlog-core.md).

**Engine-side data fields v1 exposed that v3 doesn't (yet):**
- `setup_ok` (bool) + `health` (string) per module — drove a setup-dot colour and tooltip. v3 would add `bool setupOk()` + `const char* health()` to MoonModule when a real failure mode exists.
- `parent_id` per module — v1 shipped a *flat* array and the UI rebuilt the tree by walking `parent_id` (`buildTree()`). v3's `/api/state` returns a nested tree, so this is unnecessary.
- `core` per module (FreeRTOS affinity 0/1) with inheritance (`propagateCore` — children take the parent's core). Irrelevant until core pinning is a real v3 engine feature.
- Distinct `id` (stable, machine-friendly) vs `name` (human-friendly, editable). v3 uses `name` for both; split only when disambiguating two instances of one type, or when external configs reference ids.
- `timing.ms_per_tick` — inclusive time (module *plus* children). v3 surfaces self-time (`loopTimeUs`) per card; add inclusive only if profiling demands it.

**Rendering quirks worth keeping (or rejecting):**
- v1 used `innerHTML` heavily with an `esc()` helper to neutralise HTML in user strings. v3 prefers `textContent` for all dynamic strings — no escaping, no XSS surface; reserve `innerHTML` for static templates in the JS source.
- Log stick-to-bottom: tracks `logAtBottom` via `scrollTop + clientHeight >= scrollHeight - 5`; auto-scrolls newest only if the user hasn't scrolled up. Adopt with the log panel.
- Log severity colouring: `appendLogLine` lowercases and substring-matches `error`/`fail` → red, `warn` → yellow. No structured levels — cheap and matches how embedded logs read. Adopt with the log panel.
- v1 auto-generated module ids: `type.toLowerCase().replace(/[^a-z0-9]/g,'') + '_' + (Date.now()%100000)`. v3 uses the typeName as the name at factory-create; a similar generator is needed if v3 ever splits id-from-name.
- v1's `POST /api/modules/reorder {parent_id, ids:[…]}` reordered a whole sibling group at once; v3's `POST /api/modules/<n>/move {to:N}` moves one at a time. v3's form is simpler (up/down + drag call the same endpoint).

**WebSocket nuances v3 kept:**
- `wsPaused` suppresses *processing* of incoming WS messages (state updates and binary frames dropped) while the socket stays open; set on `visibilitychange` hidden, cleared on visible; a heartbeat keeps the socket alive in the background.
- `pageshow` with `event.persisted === true` = Safari bfcache restore — `DOMContentLoaded` does NOT fire, so re-init must also hook `pageshow`.
- `wsRetryDelay` doubles each reconnect (500ms → 1s → 2s → 4s → 5s ceiling), resets on a successful open.

**Preview-canvas nuances v3 kept:**
- `naturalMaxH` captured on first scroll (unscrolled height); later shrinks compute `maxHeight = naturalMaxH * (1 - ratio*0.5)`; a resize clears the cache to re-capture.
- The WebGL drawing buffer (`canvas.width/height`) resyncs to the CSS-rendered size only when it changes — avoids a per-scroll-frame redraw cost.
- `gl_PointSize = uPtSize / gl_Position.w` (depth-corrected — closer LEDs bigger); `discard` outside a 0.25-radius disc → soft circular dots; brightness falloff via `smoothstep(0.10, 0.25, r)`.

**OTA quirks (deferred in v3, recorded for whoever implements it):**
- XHR `onerror` during firmware upload is treated as *success* — the device reboots before it can respond; the real success signal is "WS reconnects and `otaInProgress` is true."
- Byte-level upload progress needs XHR (`upload.onprogress`); `fetch()` doesn't expose it.
- `POST /api/firmware/url {url}` lets the device pull from GitHub directly instead of through the browser — useful when the browser is on cellular and the device on WiFi.
- GitHub releases cached 1h in `sessionStorage` (`pmm_gh_releases`, a JSON blob + timestamp).
- `isNewerVersion` strips a leading `v`, splits on `.`, drops anything after `-` (prerelease), compares left-to-right.

**Patterns consciously NOT carried into v3:**
- Two-port WS (HTTP on 80, WS on 81) — a v1 ESPAsyncWebServer constraint. v3 serves both on one port (`/ws`).
- `PATCH /api/modules/:id/props/:key` — v1's own UI doesn't even use it. v3's path is `POST /api/control {module, control, value}`.
- In-card OTA injection (`if (mod.type === 'FirmwareUpdateModule') card.appendChild(buildOtaPanel())`) — special-casing a module type in the UI breaks the "UI is generic" principle. v3's OTA should expose itself via standard controls (file-upload + progress-bar control types), not a hand-built panel.
- `TYPE_TO_DOC` (14 hardcoded type→docs-path entries in JS) — the UI shouldn't know per-type docs paths. v3's engine exposes `docPath` per type via `/api/types` (registered in `ModuleFactory`); the card's help link is built from that.
