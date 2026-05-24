# UI Specification

This spec describes the projectMM web UI **as shipped** — minimal, MoonModule-driven, no frameworks.

For deferred items, the open design questions for 1.0, and the gap analysis, see [moonmodules_draft/core/ui.md](../../moonmodules_draft/core/ui.md).

## TL;DR

**The UI is three hand-maintained files** (`index.html`, `app.js`, `style.css`) that render any MoonModule tree generically — no per-module-type code. State updates flow via WebSocket; mutations go through a small REST API.

**Backend contract** — what the UI depends on:
- HttpServerModule serves `/api/state`, `/api/types`, `/api/control`, `POST /api/modules`, `DELETE /api/modules/<n>`, `POST /api/modules/<n>/move`, `POST /api/modules/<n>/replace`, `POST /api/reboot`.
- WebSocket at `/ws` pushes full-tree state JSON and an optional binary preview channel (leading type byte selects the domain renderer; the payload after is domain-defined).
- Per-control `hidden` flag is supported.
- Per-module `loopTimeUs`, `classSize`, and `dynamicBytes` are exposed in `/api/state`.

## Principles

- **Three hand-maintained files**: `index.html`, `app.js`, `style.css`, plus the `moonlight-logo.png` asset. No frameworks, no build tools, no npm. All four are embedded at compile time on ESP32 via `ui_embedded.h` (the logo as a binary byte array).
- **MoonModule-driven.** The UI has zero hard-coded knowledge of specific module types or domain categories. It queries `/api/types` and `/api/state` (or its WebSocket equivalent) and renders generically — module name, role chip, controls, children — without naming any concrete type in the codebase.
- **Adding a new MoonModule with new controls requires zero UI changes** — provided the engine uses control types the UI already knows how to render.
- **WebSocket for updates, REST for mutations.** Real-time state push without polling; user actions are explicit POST/DELETE calls.
- **Two timescales for inputs.** Local UI feedback (slider value label, toggle state) updates within ~20ms of the input event. Network sends are debounced (150ms slider, 500ms text). Slider feels instant even when the server is busy.
- **No write-back races.** A `dragTs[moduleId:key]` cooldown of 1s prevents incoming WS pushes from overwriting controls the user is actively manipulating.
- **No DOM rebuilds on state updates.** Initial render builds the tree; state updates patch values in place via `querySelector` by `data-mid`/`data-key`.

## Layout

```text
┌─────────────────────────────────────────────────────────────┐
│  Status bar: ☰ · logo · brand · device name · sys stats     │
│              · WS dot · reconnect · reboot · theme toggle   │
├──────────────┬──────────────────────────────────────────────┤
│  Side nav    │  Sticky preview canvas (shrinks on scroll)   │
│  (root list) ├──────────────────────────────────────────────┤
│              │  Cards for the selected root module          │
│              │    ↳ child cards inline                      │
│  ─────────   │      ↳ grandchild cards inline               │
│  footer:     │                                              │
│  social ©    │                                              │
└──────────────┴──────────────────────────────────────────────┘
```

- Fixed status bar at top (44px) — hamburger, MoonLight logo, then the rest
- Side nav: a left column listing root modules; the selected root's card subtree fills the main area (one root visible at a time)
- Module card column max-width 500px, centered (single-column module list — easier to read on phones). The preview canvas is **not** capped — it spans the full content area width
- Preview canvas is sticky just below the status bar and shrinks 0% → 50% as the user scrolls 0 → 300px

## Status bar

Fixed top, 44px. Left-to-right:
1. Hamburger (☰) — toggles the side nav (`body.nav-open` class). See [Side navigation](#side-navigation).
2. MoonLight logo — 28px PNG, served from `/moonlight-logo.png`. The same image is the page favicon (`<link rel="icon">`).
3. Brand wordmark — "projectMM"
4. Device name (from `System.deviceName` control)
5. System stats — `uptime · NN K free` (uptime as `Xd Yh Zm Ws`, free heap as KB). Read from the SystemModule controls in the WebSocket state push; no separate polling endpoint.
6. WebSocket connection dot — green=connected, gray=disconnected. The socket auto-reconnects with exponential backoff (500ms → 5s) on close; no manual reconnect button — a page reload covers the rare half-dead-socket case.
7. Reboot button (⏻; red border via `data-crashed="true"` when `SystemModule.bootReason` is PANIC / INT_WDT / TASK_WDT / BROWNOUT). Press-twice to confirm: the first click arms it (turns solid red), a second click sends `POST /api/reboot`. Disarms after 3s or when the pointer leaves — no `confirm()` popup.
8. Theme toggle (☀/🌙) — flips `[data-theme]` on `<body>`; preference in `localStorage['mm_theme']`.

## Side navigation

A left column listing the root modules, one entry per top-level MoonModule.

- **One root visible at a time.** Clicking a nav entry sets the selected root; `renderCards()` renders only that root's card subtree. The selection persists in `localStorage['mm_selectedRoot']`; the active entry is highlighted.
- **Hamburger toggle.** The ☰ button toggles `body.nav-open`. On wide screens (≥820px) the nav is a static column the hamburger collapses/expands. On narrow screens (<820px) the nav is a slide-in drawer over a dimming overlay; clicking the overlay or pressing Esc closes it.
- **No root reorder.** Root order is fixed in `main.cpp` — the nav does not drag-reorder.
- **Footer** pinned to the bottom of the nav: social links (GitHub, Discord, Reddit, YouTube — inline SVG icons) and a `© <year> MoonLight` copyright line.

The WebSocket state push still carries the full module tree; only the selected root is rendered, and `updateValues()` patches just the visible cards (non-rendered roots have no DOM nodes, so their data is silently ignored). Per-root server-side filtering was evaluated and deferred — the 1 Hz push is cheap and the JSON is built through a streaming sink (see below), so tree size is not a buffer-limit concern; a bidirectional view-state protocol isn't justified.

## Module card

Each MoonModule renders as a card. **Child cards are nested inside their parent card's box** — the parent's border encloses its children, so the tree shape is visible structurally, not just by indentation. Nesting depth shows as progressively lighter backgrounds and a left-margin indent on the children block.

Card structure:

```text
┌─ card ──────────────────────────────────┐
│ [name] [emoji]  [timing · 🧠 mem]  [enabled toggle]  [✎ × ☰]  [? help] │
│ [control rows — one per control]        │
│ ┌─ child card ────────────────────────┐ │
│ │ …                                   │ │
│ └─────────────────────────────────────┘ │
│ ┌─ child card ────────────────────────┐ │
│ │ …                                   │ │
│ └─────────────────────────────────────┘ │
│ [+ add child]                           │
└─────────────────────────────────────────┘
```

- The parent's own controls render **above** its children; `+ add child` renders **below** them, at the bottom of the parent box.
- Child cards live in a `.card-children` wrapper appended into the parent card's DOM node — not as flat siblings in the main column. `renderModuleTree` recurses into the parent card, not into `main`.

- **Enabled toggle** in the right-hand action cluster (next to ✎ × ?) mirrors `MoonModule::enabled()`. Rendered as a styled `<button>` matching the other action buttons (transparent + muted border, 26×26); state is shown by the glyph alone — accent-coloured **✓** when on, muted **○** when off (`data-checked` carries state, `aria-pressed` reflects it). Toggling fires `onEnabled()`; the Scheduler skips disabled modules whose `respectsEnabled()` returns true (default).
- **Emoji tags** next to the name show the same set the type picker uses for that type: role emoji + dimensional emoji (both derived in the UI from `role` and `dim`) + the curated `tags()` string from `/api/types`. Identical visual identity across card and picker.
- **Help link (?)** at the far right of the title row opens the module's spec page on GitHub in a new tab. The path comes from `docPath` in `/api/types` (engine-provided, relative to `docs/moonmodules/`); the link is omitted when the type declares no doc path.
- **Stats line** shows timing and memory: `🕒 <timing>` then `🧠 <static>[ + <dynamic>]`. Timing is fps or µs/ms per the global toggle (µs under 1 ms, ms above); it is omitted entirely when the module has no measured loop time. Memory is the module's C++ object size (`classSize`); the `+ <dynamic>` part (`dynamicBytes`, heap) is shown only when the module allocated heap. Sizes are compact-formatted (`B` / `KB`). Clicking the line cycles the timing figure fps ↔ ms; the memory figure is unaffected. Mode persists in `localStorage['mm_timing_mode']` and the toggle applies to all cards globally.
- **Reorder is drag-and-drop** (HTML5 DnD) on the whole card — works on desktop and mobile. A drag starting on an interactive control (slider, button, toggle, select, text input, help link) is canceled in `dragstart` so the control's own gesture is used instead; drags from any other part of the card start a reorder. A drag is only accepted when source and target share the same `.card-children` container — i.e. they are true siblings under one parent.
- **Replace (✎)** swaps this module for another type at the same position. Opens the [type picker](#type-picker) filtered to the module's own role (same-role swap only). The replacement starts with its own default control values — a clean swap, no value carry-over. Siblings, order, and the selected root are preserved.
- **Delete (×)** removes the child via a press-twice confirm: the first click arms the button (it turns red and shows `✓`), a second click deletes. It disarms after 3s or when the pointer leaves — no browser `confirm()` popup.
- **Drag handle (☰)** is a visual cue; the whole card is the draggable element.
- **`+ add child`** in the card footer opens the [type picker](#type-picker) filtered to legal child roles for this parent. The button hides while the picker is open (the picker takes its place) and reappears when it closes.

## Control types

Auto-rendered by `controls[].type`. Adding a new MoonModule with these control types requires no UI changes.

| Type | Element | Interaction | Debounce |
|---|---|---|---|
| `slider` (uint8/uint16) | range + numeric display | drag → label updates instantly; value sent debounced; reset-to-default ↺ if a default is known | 150ms |
| `toggle` (bool) | switch (pill track + sliding thumb; visually-hidden `<input type=checkbox>` underneath stays the source of truth) | sends immediately on change | none |
| `select` | dropdown | sends immediately; server may rebuild controls (dynamic onBuildControls) | none |
| `text` | text input | sends debounced | 500ms |
| `password` | password input | masked; hold-to-peek button reveals the stored value | 500ms |
| `display` (read-only) | static text | WS push updates value in place | n/a |
| `time` (read-only seconds) | formatted text (`1d 4h 27m 13s`) | WS push updates | n/a |
| `progress` | bar + numeric `X / max` | WS push updates value | n/a |
| `button` | clickable button | sends value=1 on click | none |

**Reset-to-default button (↺).** Appears next to controls whose default is known. Defaults are captured from a fresh probe instance per type (factory's probe — no per-control boilerplate) and emitted in `/api/types`. The button is dim/inactive when value equals default, bright/clickable otherwise. Clicking sends the default value.

**Hidden controls.** When the engine returns `hidden: true` on a control descriptor, the UI skips rendering it. Used for conditional controls like NetworkModule's static-IP fields under DHCP.

**Password controls are obfuscated, not encrypted.** A `password` control (`ControlType::Password`) is serialized in `/api/state` as `{"type":"password","value":"<encoded>"}` where `<encoded>` is the password XOR'd with a fixed key then base64-encoded. The UI decodes it (`decodePassword()` in `app.js`) so the input holds the real value — masked by the password field, revealed by hold-to-peek. This is a **first line of defence only**: the XOR key is a shared constant present in both the firmware and `app.js`, so the obfuscation is trivially reversible by anyone who looks. It stops the password being plainly readable in a raw `curl /api/state`; it is not real protection against a determined reader. Writes (`/api/control`) set the value like any text control.

## Type picker

The same picker serves two purposes: **add** (triggered by `+ add child`) and **replace** (triggered by the ✎ button on a card). Renders inline inside the card (not a modal).

- **Role filter**: in add mode, filters to roles legal for the parent (the container declares which child roles it accepts). In replace mode, filters to the target module's own role. The role→child mapping is derived in the UI.
- **Emoji tag chips**: a row of toggle chips above the list, one per distinct emoji across the role-filtered types. Each type's emoji set has three sources, in this order: a **role chip** (derived in the UI from `role`), a **dimensional chip** (derived in the UI from `dim` when the type declares one — 1/2/3 means 1D/2D/3D), and the curated **`tags`** string from `/api/types` (the module's `tags()` — a flash string literal). The UI treats `tags` as opaque: it splits the string into grapheme clusters and renders each as a chip. The domain that owns this UI assigns each emoji's meaning — see the domain's own architecture page for the assignments (e.g. [architecture.md § Web UI](../../architecture.md#web-ui) for the role / dim / origin / creator / audio / moving-head assignments used by the light domain shipped today). Toggling chips narrows the list with **AND** logic: a type shows only if it carries every active chip. Each list row shows the type's emoji before its name.
- **Search box** with substring match on type name. Search and chips combine (both must match).
- **Keyboard nav**: type to filter, ↓ to enter list, ↑↓ to move, Enter to confirm, Esc to cancel.
- **Confirm / Cancel** action buttons at the bottom (the confirm button reads `create` or `replace` per mode). Double-click a row to confirm immediately.

## Module hierarchy

Each project pins a fixed top-level shape in its `main.cpp` — the side nav lists those roots in registration order, and the UI does **not** allow root reorder. System modules (Filesystem, System, Network, HttpServer) are always present; the domain modules (the actual data-flow pipeline) sit alongside them. For the light-domain shape see [architecture.md § Web UI](../../architecture.md#web-ui).

Child reorder *within* a parent (a child within a container) is supported via HTML5 drag-and-drop (desktop and mobile), which calls `POST /api/modules/<n>/move {to:N}`.

## Communication

### WebSocket (primary, for state updates)

- URL: `ws://<host>/ws` (same port as HTTP)
- Server pushes full state snapshot as JSON ~1/sec (same shape as `GET /api/state`). The JSON is built through a streaming sink with no fixed-size buffer — a module tree of any size serializes without truncation
- Server may push **binary frames** on the same socket. The first byte selects the frame type and dispatches to a domain renderer; the rest of the frame is the domain's choice. The UI ignores types it doesn't recognise. See [Domain preview channel](#domain-preview-channel) for the dispatching contract and the domain's own architecture page for the payload (e.g. [architecture.md § Web UI](../../architecture.md#web-ui))
- Client sends `"ping"` every 25s as keepalive (Safari kills idle sockets otherwise)
- Auto-reconnect on close with exponential backoff (500ms → 5s ceiling)
- Pause on `document.visibilityState === 'hidden'`; resume on `pageshow` (Safari bfcache survival)

### REST API (for mutations and initial state)

```text
GET    /api/state             full module tree state — initial load + post-mutation refresh
                              each module entry includes name, type, role, enabled,
                              loopTimeUs, classSize, dynamicBytes, controls[]
                              streamed to the socket (no Content-Length, Connection: close)
                              so a tree of any size serializes without a fixed-buffer limit
GET    /api/types             {types:[{name, displayName, role, docPath, tags, dim, defaults}]} — for the type picker
                              name is the stable factory key (e.g. "SomeTypeRole"); use this for create/replace API calls
                              displayName is the role-suffix-stripped label (e.g. "SomeType") — what the cards and picker rows show
                              docPath is the spec page relative to docs/moonmodules/ ("" if none)
                              tags is a curated emoji string for the picker's chip filter ("" if none)
                              dim is the dimensionality (1/2/3) when the type declares one; 0 otherwise
                              defaults map is captured from a fresh probe instance per type
GET    /api/system            fps, tickTimeUs, freeHeap, freeInternal, maxBlock, uptime
POST   /api/control           {module, control, value} — set a control value
POST   /api/modules           {type, parent_id?} — create
POST   /api/modules/<n>/move  {to: N} — reorder to absolute index N within parent
                              strict-suffix match: /movex returns 404
                              triggers Scheduler::rebuild() so LUT-affecting reorders rebuild
POST   /api/modules/<n>/replace {type} — swap module <n> for another type at the same
                              position. Strict-suffix match. The replacement starts with
                              factory defaults; siblings and order are preserved.
                              Rejects top-level modules and unknown types.
DELETE /api/modules/<name>
POST   /api/reboot            calls platform::reboot() — esp_restart() on ESP32, std::exit(0) on desktop
```

The `/api/control` shape is `{module: "name", control: "key", value: …}`.

### Static assets

- `GET /` → `index.html`
- `GET /app.js`, `GET /style.css`
- `GET /moonlight-logo.png` → the MoonLight logo (`image/png`), used in the header and as the favicon
- `Cache-Control: no-cache` on all UI responses (live development)
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

## Domain preview channel

The UI dedicates a binary slot on its WebSocket — separate from the JSON state updates — for a domain-specific preview frame. The engine pushes one frame per render; the UI hands it to a domain renderer.

Generic shape: `[type-byte] [domain-specific header] [payload]`. The first byte identifies the frame type and selects the renderer the UI dispatches to; everything after is the domain's choice.

- The canvas is sticky just below the status bar and scroll-shrinks 0→50% over 300px of page scroll
- `width: 100%` + `aspect-ratio: 1 / 1` derives the height from the column width; `max-height: 50vh` caps it so the canvas never dominates the viewport
- The last received frame is cached so camera gestures (orbit, pan, zoom) redraw instantly without waiting for a new frame
- `touch-action: none` so single- and multi-finger gestures don't trigger native page scroll or pinch-zoom
- WebGL clear color is `(0, 0, 0, 0)` — transparent canvas blends into either theme without per-frame color work

For the light-domain renderer (WebGL point cloud, frame format, orbit camera, downsampling) see [architecture.md § Web UI](../../architecture.md#web-ui).

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

The DOM is **never rebuilt** during a state update. Full re-render only happens on (a) initial load and (b) after a mutation that changes the tree shape (`/api/control` for a Select that triggers `rebuildControls`, `/api/modules` add/delete, `/api/modules/<n>/move`, `/api/modules/<n>/replace`).

**Object identity across WS pushes.** Every WS state push replaces `state` with a fresh JSON tree. Card-render closures that hold a `mod` reference become stale within ~1s. Any lookup of "which index am I now?" must use `findIndex(c => c.name === mod.name)` rather than `indexOf(mod)`.

## localStorage keys

```text
mm_selectedRoot     id of currently-selected root module       (string)
mm_theme            "dark" | "light"                            (default: "dark")
mm_timing_mode      "fps" | "ms"                                (default: "fps")
```

No other client state persists. Reorder, control values, etc. all live on the device.

## Feature summary

Everything in this spec is in the live codebase. The 12 features below each link to the section that describes the contract.

1. **Status bar** with hamburger, MoonLight logo, brand, device name, system stats (uptime · free heap), WS dot, reboot button (with crashed-state styling), and theme toggle — see [Status bar](#status-bar).
2. **Side navigation** — a left column listing root modules; one root visible at a time, selection persisted; footer with social links + copyright; hamburger collapses it (wide) or slides it in over an overlay (<820px) — see [Side navigation](#side-navigation).
3. **Single-column module card layout** with hierarchy (children inline-indented, depth-based card backgrounds) — see [Module card](#module-card).
4. **All 9 control types** (uint8 slider, uint16, bool, text, password with hold-to-peek, select, display, time, progress, button) with the `dragTs` + 20ms-feedback + 150/500ms-debounce pattern — see [Control types](#control-types).
5. **Type picker** (role-filtered, emoji tag chip filter, search box, keyboard navigation) on parents that accept children — see [Type picker](#type-picker).
6. **Reset-to-default ↺** buttons per control with a known default. Defaults are captured from a fresh probe instance per type (factory's probe — no per-control boilerplate) and emitted in `/api/types`.
7. **Light/dark theme toggle** via `[data-theme]` on `<body>` + CSS variables; preference persists in `localStorage['mm_theme']`.
8. **WS keepalive ping (25s)** + `visibilitychange` pause + `pageshow` bfcache resume — see [Communication § WebSocket](#websocket-primary-for-state-updates).
9. **Domain preview channel** — a sticky canvas above the cards that scroll-shrinks 0→50% over 300px, fed by a binary frame on the WebSocket and rendered by a domain plugin — see [Domain preview channel](#domain-preview-channel).
10. **Per-card stats line** — `🕒` timing (clickable to cycle fps↔ms, global mode in `localStorage['mm_timing_mode']`) plus `🧠` memory: `classSize`, and `+ dynamicBytes` only when the module allocated heap.
11. **Card action buttons** on reorderable children: ✎ replace-with-another-type, × delete (press-twice). Reorder is HTML5 drag-and-drop on desktop and mobile. Reorder calls `POST /api/modules/<n>/move`, replace calls `POST /api/modules/<n>/replace`.
12. **MoonLight logo + favicon** — the header logo and browser-tab favicon are the same `/moonlight-logo.png` asset, embedded on ESP32.
