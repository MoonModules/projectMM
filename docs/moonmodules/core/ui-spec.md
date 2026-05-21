# UI Specification

This spec describes the projectMM v3 web UI **as shipped**. It distils what v1 proved at scale, adapted to v3's principles (minimalism, MoonModule-driven, no frameworks).

For deferred items, the open design questions for 1.0, the full gap analysis against projectMM v1, loose ends, and prior-art notes, see [moonmodules_draft/core/ui-spec-deferred.md](../../moonmodules_draft/core/ui-spec-deferred.md).

## TL;DR

**The UI is three hand-maintained files** (`index.html`, `app.js`, `style.css`) that render any MoonModule tree generically — no per-effect or per-driver code. State updates flow via WebSocket; mutations go through a small REST API.

**Backend contract** — what the UI depends on:
- HttpServerModule serves `/api/state`, `/api/types`, `/api/control`, `POST /api/modules`, `DELETE /api/modules/<n>`, `POST /api/modules/<n>/move`, `POST /api/reboot`.
- WebSocket at `/ws` pushes full-tree state JSON and binary preview frames (`[0x02][w16][h16][d16][RGB…]`).
- Per-control `hidden` flag is supported.
- Per-module `loopTimeUs` and `dynamicBytes()` are exposed in `/api/state`.

## Principles

- **Three hand-maintained files**: `index.html`, `app.js`, `style.css`. No frameworks, no build tools, no npm. Embedded at compile time on ESP32 via `ui_embedded.h`.
- **MoonModule-driven.** The UI has zero hard-coded knowledge of specific effects, layouts, modifiers, or drivers. It queries `/api/types` and `/api/state` (or its WebSocket equivalent) and renders generically.
- **Adding a new MoonModule with new controls requires zero UI changes** — provided the engine uses control types the UI already knows how to render.
- **WebSocket for updates, REST for mutations.** Real-time state push without polling; user actions are explicit POST/DELETE calls.
- **Two timescales for inputs.** Local UI feedback (slider value label, toggle state) updates within ~20ms of the input event. Network sends are debounced (150ms slider, 500ms text). Slider feels instant even when the server is busy.
- **No write-back races.** A `dragTs[moduleId:key]` cooldown of 1s prevents incoming WS pushes from overwriting controls the user is actively manipulating.
- **No DOM rebuilds on state updates.** Initial render builds the tree; state updates patch values in place via `querySelector` by `data-mid`/`data-key`.

## Layout

```text
┌─────────────────────────────────────────────────────────────┐
│  Status bar: brand · device name · sys stats                │
│              · WS dot · reconnect · reboot · theme toggle   │
├─────────────────────────────────────────────────────────────┤
│  Sticky 3D preview canvas (shrinks 50% on scroll)           │
├─────────────────────────────────────────────────────────────┤
│  Module cards in a single column (max-width 500px)          │
│    ↳ child cards inline                                     │
│      ↳ grandchild cards inline                              │
└─────────────────────────────────────────────────────────────┘
```

- Fixed status bar at top (44px)
- Main column max-width 500px, centered (single-column module list — easier to read on phones)
- 3D preview is sticky just below the status bar and shrinks 0% → 50% as the user scrolls 0 → 300px

## Status bar

Fixed top, 44px. Left-to-right:
1. Brand wordmark — "projectMM"
2. Device name (from `System.deviceName` control)
3. System stats — `uptime · NN K free` (uptime as `Xd Yh Zm Ws`, free heap as KB). Read from the SystemModule controls in the WebSocket state push; no separate polling endpoint.
4. WebSocket connection dot — green=connected, gray=disconnected
5. Reconnect button (⟳)
6. Reboot button (⏻; red border via `data-crashed="true"` when `SystemModule.bootReason` is PANIC / INT_WDT / TASK_WDT / BROWNOUT). Confirms via `confirm()` then `POST /api/reboot`.
7. Theme toggle (☀/🌙) — flips `[data-theme]` on `<body>`; preference in `localStorage['mm_theme']`.

## Module card

Each MoonModule renders as a card. Children render as nested cards in the same column, visually indented with a left border and progressively lighter backgrounds.

Card structure:

```text
[enabled toggle]  [name]  [fps/ms]  [↑ ↓ × ☰]
[control rows — one per control]
[+ add child]
```

- **Enabled toggle** at the start of the title row mirrors `MoonModule::enabled()`. Toggling fires `onOnOff()`; the Scheduler skips disabled modules whose `respectsEnabled()` returns true (default).
- **Stats line is a clickable toggle.** Clicking cycles fps ↔ ms display format. Mode persists in `localStorage['mm_timing_mode']`. Same toggle affects all cards globally.
- **Up / Down (↑ ↓)** buttons reorder this child within its parent. Drag-and-drop on the whole card (HTML5 DnD) provides the same effect on desktop; mobile uses the buttons.
- **Delete (×)** removes the child. Confirms via `confirm()`.
- **Drag handle (☰)** is a visual cue; the whole card is the draggable element.
- **`+ add child`** in the card footer opens the [type picker](#type-picker) filtered to legal child roles for this parent.

## Control types

Auto-rendered by `controls[].type`. Adding a new MoonModule with these control types requires no UI changes.

| Type | Element | Interaction | Debounce |
|---|---|---|---|
| `slider` (uint8/uint16) | range + numeric display | drag → label updates instantly; value sent debounced; reset-to-default ↺ if a default is known | 150ms |
| `toggle` (bool) | checkbox | sends immediately on change | none |
| `select` | dropdown | sends immediately; server may rebuild controls (dynamic onBuildControls) | none |
| `text` | text input | sends debounced | 500ms |
| `password` | password input | hold-to-peek button; placeholder shows `•` for existing length | 500ms |
| `display` (read-only) | static text | WS push updates value in place | n/a |
| `time` (read-only seconds) | formatted text (`1d 4h 27m 13s`) | WS push updates | n/a |
| `progress` | bar + numeric `X / max` | WS push updates value | n/a |
| `button` | clickable button | sends value=1 on click | none |

**Reset-to-default button (↺).** Appears next to controls whose default is known. Defaults are captured from a fresh probe instance per type (factory's probe — no per-control boilerplate) and emitted in `/api/types`. The button is dim/inactive when value equals default, bright/clickable otherwise. Clicking sends the default value.

**Hidden controls.** When the engine returns `hidden: true` on a control descriptor, the UI skips rendering it. Used for conditional controls like NetworkModule's static-IP fields under DHCP.

## Type picker

Triggered by `+ add child` (on a parent that accepts children) or `+ add module` (root). Renders inline inside the card (not a modal).

- **Context-aware filter**: as a child, filters to roles legal for that parent (e.g. Layer accepts Effect+Modifier; DriverGroup accepts Driver). The role→child mapping is derived in the UI from each parent's `role()`.
- **Search box** with substring match on type name.
- **Keyboard nav**: type to filter, ↓ to enter list, ↑↓ to move, Enter to create, Esc to cancel.
- **Create / Cancel** action buttons at the bottom. Double-click a row to create immediately.

## Module hierarchy

v3 has a fixed top-level shape on first boot:

```text
Filesystem      (persistence)
System          (read-only diagnostics)
Network         (WiFi/Ethernet/AP/mDNS)
LayoutGroup
  └─ GridLayout (or other layouts)
Layer
  └─ effects (NoiseEffect, RainbowEffect, …)
  └─ modifiers (MirrorModifier, …)
DriverGroup
  └─ ArtNetSendDriver
  └─ PreviewDriver
HttpServer      (UI lives here)
```

Reorder of root modules is **not** supported — the order is fixed in `main.cpp`. Child reorder within a parent uses up/down buttons and HTML5 drag-and-drop; both call `POST /api/modules/<n>/move {to:N}`.

## Communication

### WebSocket (primary, for state updates)

- URL: `ws://<host>/ws` (same port as HTTP)
- Server pushes full state snapshot as JSON ~1/sec (same shape as `GET /api/state`)
- Server pushes binary preview frames: `[0x02][w16][h16][d16][R G B …]` — 3D voxel grid, little-endian widths
- Client sends `"ping"` every 25s as keepalive (Safari kills idle sockets otherwise)
- Auto-reconnect on close with exponential backoff (500ms → 5s ceiling)
- Pause on `document.visibilityState === 'hidden'`; resume on `pageshow` (Safari bfcache survival)

### REST API (for mutations and initial state)

```
GET    /api/state             full module tree state — initial load + post-mutation refresh
                              each module entry includes name, type, role, enabled, loopTimeUs, controls[]
GET    /api/types             {types:[{name, role, defaults}]} — for the type picker
                              defaults map is captured from a fresh probe instance per type
GET    /api/system            fps, tickTimeUs, freeHeap, freeInternal, maxBlock, uptime
POST   /api/control           {module, control, value} — set a control value
POST   /api/modules           {type, parent_id?} — create
POST   /api/modules/<n>/move  {to: N} — reorder to absolute index N within parent
                              strict-suffix match: /movex returns 404
                              triggers Scheduler::rebuild() so LUT-affecting reorders rebuild
DELETE /api/modules/<name>
POST   /api/reboot            calls platform::reboot() — esp_restart() on ESP32, std::exit(0) on desktop
```

The `/api/control` shape is `{module: "name", control: "key", value: …}`.

### Static assets

- `GET /` → `index.html`
- `GET /app.js`, `GET /style.css`
- `Cache-Control: no-cache, no-store, must-revalidate` on all UI responses (live development)
- ESP32: served from C arrays in `ui_embedded.h` regenerated at build time
- Desktop: served from disk (`uiPath_`)

## Styling

- **Dark theme (default)**: background `#1a1a2e`, foreground `#e0e0e0`, accent `#a78bfa` (purple)
- **Light theme**: `[data-theme="light"]` on `<body>` swaps a curated set of overrides
- **System UI font stack** — `system-ui, sans-serif` — not monospace. Numbers use `font-variant-numeric: tabular-nums` so digits don't dance.
- **Module nesting** by progressively lighter card backgrounds (depth 0 / 1 / 2 each one step lighter)
- **Responsive breakpoint** at 820px (padding adjusts)
- **Color semantics** consistent across the app:
  - Green (`#22c55e`/`#6ee7b7`) — connected, ok, pass, success
  - Red (`#f87171`) — error, fail, crashed, delete
  - Purple (`#a78bfa`/`#c4b5fd`) — accent, brand, active, value
  - Gray (`#6b7280`/`#9ca3af`/`#4b5563`) — secondary text, muted

## 3D preview

WebGL point cloud rendered to a `<canvas>`.

- Server pushes binary frames over the same WebSocket as state updates
- Frame format: `[0x02][w16][h16][d16][R G B …]` — width/height/depth in 16-bit little-endian, then RGB triples in `(z, y, x)` order
- UI builds an interleaved float vertex buffer `[x, y, z, r, g, b]`, **skipping black voxels** so payload-to-GPU is sparse and fast
- Camera: orbit on mouse-drag, wheel-zoom, touch-orbit on mobile
- Point size scales with canvas size and grid maxDim (target ~10px per LED at rest)
- GLSL: `gl_PointSize = uPtSize / gl_Position.w` (depth-corrected), `discard` for fragments outside a 0.25-radius disc, brightness falloff via `smoothstep(0.10, 0.25, r)`
- Canvas auto-shrinks 50% as the user scrolls (smooth via `requestAnimationFrame`)
- Last frame is cached so mouse-drag redraws are instant without a new server frame

## State updates — the no-rebuild contract

When a WS state-snapshot arrives:

1. For each module in the payload:
   - If `controls.deviceName` exists, update the status-bar device name and `document.title`
   - For each `controls[key]`:
     - Look up the matching `<input>`/`<span>`/`<progress>`/`<select>` by `[data-mid][data-key]`
     - **Skip** the update if `Date.now() - dragTs[mid:key] < 1000` (user is actively interacting)
     - Otherwise patch the value in place. Sliders update both the input and the adjacent `value-display` span. Toggles set `checked`. Selects set `value`.
   - Update the reset-to-default button's "at default?" state.
   - Update timing display (`fps` or `ms` per the global toggle).

The DOM is **never rebuilt** during a state update. Full re-render only happens on (a) initial load and (b) after a mutation that changes the tree shape (`/api/control` for a Select that triggers `rebuildControls`, `/api/modules` add/delete, `/api/modules/<n>/move`).

**Object identity across WS pushes.** Every WS state push replaces `state` with a fresh JSON tree. Card-render closures that hold a `mod` reference become stale within ~1s. Any lookup of "which index am I now?" must use `findIndex(c => c.name === mod.name)` rather than `indexOf(mod)`.

## localStorage keys

```
mm_selectedRoot     id of currently-selected root module       (string)
mm_theme            "dark" | "light"                            (default: "dark")
mm_timing_mode      "fps" | "ms"                                (default: "fps")
```

No other client state persists. Reorder, control values, etc. all live on the device.

## Implemented baseline

Everything in this spec is in the live codebase. The 10 features below are the explicit baseline established by plan-11 — each links to the section that describes the contract.

1. **Status bar** with brand, device name, system stats (uptime · free heap), WS dot, reconnect button, reboot button (with crashed-state styling), and theme toggle — see [Status bar](#status-bar).
2. **Single-column module card layout** with hierarchy (children inline-indented, depth-based card backgrounds) — see [Module card](#module-card).
3. **All 9 control types** (uint8 slider, uint16, bool, text, password with hold-to-peek, select, display, time, progress, button) with the `dragTs` + 20ms-feedback + 150/500ms-debounce pattern — see [Control types](#control-types).
4. **Type picker** (role-filtered, search box, keyboard navigation) on parents that accept children — see [Type picker](#type-picker).
5. **Reset-to-default ↺** buttons per control with a known default. Defaults are captured from a fresh probe instance per type (factory's probe — no per-control boilerplate) and emitted in `/api/types`.
6. **Light/dark theme toggle** via `[data-theme]` on `<body>` + CSS variables; preference persists in `localStorage['mm_theme']`.
7. **WS keepalive ping (25s)** + `visibilitychange` pause + `pageshow` bfcache resume — see [Communication § WebSocket](#websocket-primary-for-state-updates).
8. **3D preview** with orbit camera (mouse + touch), sticky position, scroll-shrink 0→50% over 300px, sparse vertex buffer (skipping black voxels), and depth-corrected GLSL point size — see [3D preview](#3d-preview).
9. **Per-card fps/ms toggle** — clickable stats line cycles fps↔ms; global mode persists in `localStorage['mm_timing_mode']`.
10. **Up/↑/↓ icon buttons** for reorder + delete (×) on reorderable children (Effect/Modifier role). Drag-and-drop (HTML5 DnD) works on desktop; mobile naturally falls through to the icon buttons. Both call `POST /api/modules/<n>/move`.

## Heritage

v3's UI is a reduction of projectMM v1's UI ([source](https://github.com/ewowi/projectMM/tree/main/src/frontend)), which shipped at release 1.4.0 as ~84/1647/744 lines (HTML/JS/CSS). The v1 patterns proven in production and carried forward are documented inline above. v1's larger surface (OTA, GitHub-release update badge, health/log panels, side nav) is held back until v3 has the supporting engine infrastructure — see [moonmodules_draft/core/ui-spec-deferred.md](../../moonmodules_draft/core/ui-spec-deferred.md) for the full catalogue and adopt/defer/drop verdicts per item.
