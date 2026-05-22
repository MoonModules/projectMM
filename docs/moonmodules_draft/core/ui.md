# UI Specification — Deferred items & research

Companion to [moonmodules/core/ui.md](../../moonmodules/core/ui.md). The main spec describes the v3 UI as shipped (the implemented baseline). This file holds everything that is **not** in the live UI yet: short-term deferred items, open design questions for 1.0, the full gap analysis against projectMM v1, loose ends, and prior-art notes.

Promote sections from here into the main spec as they ship. When the entire file becomes empty, delete it.

## Deferred to 1.x

- Side nav with drag-reorder of root modules (root order is fixed in `main.cpp` today; not painful)
- Health panel (`<details>` + `GET /api/test`)
- Log panel (`<details>` + WS `{t:"log",m:"…"}`)
- Update-available badge + OTA panel (requires `/api/firmware`)
- Module replace (✎) button (additive to add/delete, but needs `POST /api/modules/replace`)
- Core affinity badge (C0/C1) — only meaningful when core pinning lands
- Module category() field — adds taxonomy beyond `role()` for the picker (decision: derive from role() for now)

## Open design questions

These don't block the implemented baseline but should be answered before 1.0 ships:

- **Multi-layer UI** (architecture-light.md plans for N layers blended into one DriverGroup). The current card layout shows one Layer. Likely needs a tab/accordion to switch between layers, or a per-layer column.
- **Modifier chain visualization** — show the modifier order visually (the order in `children[]` is the order they apply). Today they're just a flat list.
- **Presets** — save/load named bundles of control values. Persistence already supports the storage; needs a UI surface.
- **Canvas/node-graph view** — v2 attempted this. Powerful for complex setups but a doubling of UI surface. Reasonable v3 follow-up gated on user demand.

## Gap analysis — v1 features not (yet) in v3

A complete inventory of behaviors I found in v1's frontend that v3 doesn't have today, with a recommendation per item. Items already in the implemented baseline (control types, dragTs, two-timescale inputs, type picker, theme, scroll-shrink preview, status bar, reset-to-default, fps/ms toggle, drag-and-drop reorder) are not repeated here.

Legend:
- **Adopt-1.0** — bring in shortly; small and the value/cost ratio is high
- **Defer-1.x** — postpone with a clear gate (needs engine work, or value depends on a feature we don't have yet)
- **Drop** — v1 had it but v3 doesn't need it

### Layout & navigation

| v1 feature | v3 today | Recommendation |
|---|---|---|
| Sidebar nav with one root visible at a time (selection persisted in localStorage) | flat module grid, all roots visible at once | **Defer-1.x** — the current grid works for v3's ~6 root modules. Side nav matters when the tree grows. Start with grid; add side nav when root count exceeds ~8 or the user reports clutter. |
| Drag-to-reorder root modules (saves to `/api/modules/reorder`) | not supported | **Drop** — root order is fixed in `main.cpp` and that's correct: System/Network/Layer/DriverGroup are mandatory and have a logical setup order. Children get up/down + drag (already shipped). |
| Hamburger menu + slide-in drawer + overlay on mobile (<820px) | none | **Adopt-1.0 only if side nav is adopted.** Skip otherwise. |
| `<details>` collapsible panels at bottom (health, log) | none | See dedicated rows below. |
| Footer in side nav with copyright + Discord/Reddit/YouTube/GitHub links | none | **Defer-1.x** until/unless we adopt the side nav. The links can sit elsewhere (or in a README). |

### Per-card features

| v1 feature | v3 today | Recommendation |
|---|---|---|
| Header: setup-dot before name | name only | **Defer-1.x** — needs `setupOk()` + `health()` on MoonModule with a real failure mode to report. Today both would always be `true` / `""`. |
| Module ID shown separately from name in meta line | name only | **Defer-1.x** — v3's typeName is shown in `/api/state` but the UI doesn't display it. Add when module instances need disambiguating (e.g. multiple effects with the same typeName under one Layer). |
| Category emoji badge (⚙️ 🌐 ✨ 💡 📐) | none | **Adopt-1.0 if `role()` mapping is added in the UI.** Five emojis, zero backend work — the mapping `Effect→✨`, `Driver→💡`, `Modifier→…`, `Layout→📐`, `Generic→none` lives in `app.js` directly. Adds visual scannability for free. |
| Core affinity badge (C0/C1 colored badge for FreeRTOS core pinning) | core pinning not implemented in v3 engine | **Drop** for now. If core pinning is added to v3, this badge re-enters scope. |
| Memory display: classSize · heap-bytes · psram-bytes (separated) | none | **Adopt-1.0** — `classSize()` + `dynamicBytes()` already on MoonModule. Splitting heap vs PSRAM needs `platform::isPsramPointer(p)` or per-allocation tracking, neither of which exists yet. Start with `dynamicBytes` only, total. |
| Help link per module (`?` link to `https://ewowi.github.io/projectMM/modules/<type>/`) with TYPE_TO_DOC mapping in JS | none | **Defer-1.x** — needs documentation site to exist for v3 module pages. Mapping is 14 lines of JS — add when docs are published. Cleaner approach: engine exposes `docPath` per type via `/api/types`, defaulting to nothing. |
| Children indented with left border, depth-based card backgrounds (3 nesting levels visible) | none | **Adopt-1.0** — pure CSS, ~5 rules. Makes the parent/child relationship obvious at a glance. |
| Replace-type button (✎) | none | **Defer-1.x** — needs `POST /api/modules/replace` endpoint. The UX value is "stays selected, position preserved" which needs an atomic backend operation. |

### Type picker — research notes

| v1 feature | v3 today | Recommendation |
|---|---|---|
| Emoji "tag" chips for additional filtering | none | **Adopt-1.0** if `tags` is added to `/api/types`. Otherwise **defer**. Tags are nice-to-have; the search box alone is enough for the current type count. |
| Module replace (`POST /api/modules/replace`) — swap a child's type at the same position without losing siblings | none | **Defer-1.x** — needs backend endpoint. |

### WebSocket protocol

| v1 feature | v3 today | Recommendation |
|---|---|---|
| Log channel: `{t:"log", m:"..."}` pushed by server | server doesn't push logs | **Defer-1.x** — needs engine-side log producer. Useful for live debugging on hardware. Gate: when boot/network/persistence logs start being interesting to non-developers. |
| Schema channel: `{t:"schema", modules:[...]}` for tree-shape changes | full /api/state push on every update | **Drop** — keep v3's full-tree push. Re-evaluate only if WS bandwidth becomes a problem with large trees. |

### Other panels

| v1 feature | v3 today | Recommendation |
|---|---|---|
| System health panel (`<details>` collapsible at bottom, polls `GET /api/test` every 30s, shows pass/fail table) | none | **Defer-1.x** — needs backend `/api/test` endpoint that runs the doctest suite at runtime. Lower priority — `ctest` covers this for now. |
| Log panel (`<details>`, ring buffer of 100 lines, error/warn coloring, auto-scroll with stick-to-bottom detection, backfill via `GET /api/log` on reconnect) | none | **Defer-1.x** — pairs with the log WS channel above. Both arrive together. |
| Update-available badge in status bar (polls GitHub `/releases`, hour cache, version compare) | none | **Defer-1.x** — needs to be paired with an OTA story. Gate: only meaningful when v3 ships GitHub releases. |
| OTA panel inside `FirmwareUpdateModule` card (file upload + GitHub releases tab, XHR with progress, device-side download via `POST /api/firmware/url`, handle WS-drop-as-success-during-reboot) | none | **Defer-1.x** — non-trivial backend work (HTTP upload handler, secondary OTA partition handling, signed update support). Whole feature is its own plan. |
| Firmware download to device (`POST /api/firmware/url` with body `{url}`) — device downloads from GitHub, frees UI from being the bottleneck | none | **Defer-1.x** — same plan as OTA. |

### Helpers and polish

| v1 feature | v3 today | Recommendation |
|---|---|---|
| Byte formatting (`X B` / `X KB`) | inconsistent | **Adopt-1.0** — ~5 lines of `fmtBytes()`. Use everywhere memory is shown. |
| Numeric formatting with `Number.isInteger()` check (integer→`String(n)`, float→`n.toFixed(1)`) | inconsistent | **Adopt-1.0** — small utility. Already done by `displayControlValue` in v3 partially. |
| Favicon = base64 brand logo (declared once in HTML, copied to favicon link at init) | no favicon | **Adopt-1.0** — visual identity in browser tabs. ~5 lines. |
| Document title kept in sync with deviceName | none | **Adopt-1.0** — one line in the WS handler. Helps when juggling multiple devices in browser tabs. |

### Patterns to consciously NOT carry over

- **Single root visible at a time.** v1's side nav forces the user to pick which root to look at. v3 has fewer roots — show them all by default and revisit if it gets noisy.
- **Two-port WS (HTTP on 80, WS on 81).** v1 uses port 81 for WS because of the ESPAsyncWebServer constraints. v3's HTTP stack handles both on the same port — keep `/ws`.
- **`PATCH /api/modules/:id/props/:key`.** The earlier draft mentioned this; v1 itself doesn't use it. Drop entirely. `POST /api/control {module, control, value}` is the path.
- **In-card OTA injection** (`if (mod.type === 'FirmwareUpdateModule') card.appendChild(buildOtaPanel())`). Special-casing module types in the UI breaks the "UI is generic" principle. If/when OTA arrives in v3, the OTA module should expose its UI via standard controls (file-upload control type, progress-bar control type) — not a hand-built panel.

### Quick decision table

| Cost class | Items |
|---|---|
| Tiny (< 30 lines each, no backend work) | category emoji badge, child indent + bg-depth, document.title sync, favicon, byte/number formatters |
| Small (30–100 lines, no backend) | — (all small items shipped in the baseline) |
| Medium (needs minor backend change) | help-link mapping (needs docs site), Module replace (needs `/api/modules/replace`), category() field if we ever want it richer than role()-derived |
| Large (separate plan) | health panel + `/api/test`, log panel + WS log channel, side nav, OTA + GitHub-update badge, full multi-layer UI, presets UI |

## Loose ends — details from v1 that don't belong in any cluster

These are smaller mechanisms recorded so we don't have to rediscover them. Each is a one-line note plus disposition.

**Engine-side data fields that v3 doesn't expose yet:**
- `setup_ok` (bool) and `health` (string) per module — drive the setup-dot color and its tooltip. **Defer-1.x**: add to MoonModule as `bool setupOk()` + `const char* health()` when we have a real failure mode to report. Today both would always be `true` / `""`.
- `parent_id` per module — v1 ships a flat array and the UI rebuilds the tree via `buildTree()` walking `parent_id`. v3's `/api/state` already returns a nested tree, so **Drop** — the UI never needs to assemble parents from a flat list.
- `core` per module (FreeRTOS core affinity 0/1) with **inheritance**: children take their parent's core via `propagateCore`. **Drop until core pinning is a real engine feature.**
- Distinct `id` (stable, machine-friendly) vs `name` (human-friendly, editable) per module. v3 today uses `name` for both. **Defer-1.x**: split when the UI needs to disambiguate two instances of the same type or when ids are referenced from external configs.
- `timing.self_ms_per_tick` (this module excluding children) and `timing.ms_per_tick` (this module including children). v3's `loopTimeUs` is the "self" form. **Adopt-1.0 in reduced form**: surface the existing self-time; add an "inclusive" time later only if profiling demands it.

**Rendering quirks worth keeping (or rejecting):**
- v1 uses `innerHTML` heavily and an `esc()` helper to neutralize HTML in user strings. **v3 should prefer `textContent`** for all dynamic strings (no escape needed, no XSS surface). Reserve `innerHTML` for static templates inside the JS source. Already partially the case in v3.
- Stick-to-bottom log auto-scroll: v1 tracks `logAtBottom` based on whether `scrollTop + clientHeight >= scrollHeight - 5`. Scrolls newest line into view only if the user hasn't scrolled up to read older messages. **Adopt-1.0** when the log panel arrives.
- Log severity coloring: `appendLogLine` lowercases the text and looks for `'error'`/`'fail'` → red, `'warn'` → yellow. Substring-based — no structured log levels. **Adopt-1.0 with the log panel.** Cheap and matches how most embedded firmware logs read.
- Auto-generated module ids in v1's `createModule`: `type.toLowerCase().replace(/[^a-z0-9]/g, '') + '_' + (Date.now() % 100000)`. **Adopt or replace.** v3 currently uses the typeName as the name when the factory creates a module; if/when v3 splits id-from-name (deferred row above), we need a similar generator.
- v1's `POST /api/modules/reorder` accepts `{parent_id, ids: [...]}` — full reordering of a sibling group at once. v3's `POST /api/modules/<n>/move {to:N}` moves one module at a time. **Keep v3's form** — simpler API, simpler UI (up/down + drag both call the same endpoint).

**WebSocket nuances (already in the baseline but worth recording):**
- `wsPaused` flag suppresses *processing* of incoming WS messages (state updates and binary frames are dropped) but the socket itself stays open. Set on `visibilitychange` hidden, cleared on visible. Heartbeat keeps the socket alive in the background.
- `pageshow` event with `event.persisted === true` indicates Safari restored the page from bfcache. DOMContentLoaded does NOT fire in this case, so any re-init logic must hook `pageshow` too.
- `wsRetryDelay` doubles each reconnect attempt (500ms → 1s → 2s → 4s → 5s ceiling) and resets on successful open.

**Preview canvas nuances (already in the baseline):**
- `naturalMaxH` is captured on first scroll (the initial unscrolled canvas height). Subsequent shrinks compute `maxHeight = naturalMaxH * (1 - ratio * 0.5)`. Resize clears the cache so the next scroll re-captures.
- WebGL drawing buffer (`canvas.width`/`canvas.height`) is resynced to the actual CSS-rendered size only when it changes — avoids a redraw cost on every scroll frame.
- `gl_PointSize = uPtSize / gl_Position.w` (depth-corrected point size) — closer LEDs render bigger. Plus `discard` for fragments outside a 0.25-radius disc → soft circular dots. Plus brightness falloff via `smoothstep(0.10, 0.25, r)`.

**OTA quirks (all deferred but worth recording for whoever picks up OTA):**
- XHR `onerror` during firmware upload is treated as success — devices reboot before they can send the response. The success path is "WS reconnect succeeds and `otaInProgress` is true."
- File upload via XHR (`upload.onprogress`) for byte-level progress; `fetch()` doesn't expose upload progress.
- `POST /api/firmware/url {url}` lets the device download from GitHub directly instead of through the browser — useful when the browser is on cellular and the device on wifi.
- GitHub releases cached for 1h in `sessionStorage` (`pmm_gh_releases`) — keyed nothing fancy, just a JSON blob with a timestamp.
- `isNewerVersion` strips a leading `v`, splits on `.`, drops anything after `-` (prerelease tags), compares left-to-right. Good enough for the existing tag scheme.

**Specifics consciously rejected:**
- v1's `if (mod.type === 'FirmwareUpdateModule') card.appendChild(buildOtaPanel())` — already called out in "Patterns to NOT carry over." OTA should expose itself via standard controls, not a hand-built panel.
- v1's `TYPE_TO_DOC` mapping (14 hardcoded type→docs-path entries in JS) — the UI shouldn't know the docs path per type. When help links return, the engine should expose `docPath` per type via `/api/types`, defaulting to nothing.

## Prior art

- **projectMM v1** ([source](https://github.com/ewowi/projectMM/tree/main/src/frontend)) — the direct ancestor. The full v1 frontend at release 1.4.0 has been thoroughly reverse-engineered into this catalogue.
- **projectMM v2** ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/frontend/index.html)) — added a canvas/node-graph view alongside the tree view. The tree view stayed as the proven default; canvas was an "alongside" complexity tax. v3 inherits only the tree view.
- **MoonLight** — the WLED-MM web UI. Card layout, type picker patterns, and the dragTs cooldown originated there.
