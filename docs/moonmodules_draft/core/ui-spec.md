# UI Specification

## Principles

- Three hand-maintained files: `index.html`, `app.js`, `style.css`. No frameworks, no build tools, no npm.
- **MoonModule-driven.** The UI has zero hard-coded knowledge of specific effects, layouts, modifiers, or drivers. It queries the system for the current MoonModule tree and renders it generically.
- Adding a new MoonModule with controls requires **zero changes** to the UI files.
- Controls are auto-rendered based on type.
- Served by the embedded HTTP server (system MoonModule).

## Proven patterns from v1

The v1 UI (release 1.4.0) was a single 1909-line index.html that worked well. The following proven patterns should be carried forward:

### WebSocket-driven updates (not polling)
v1 used WebSocket for real-time state push. The v3 prototype used 1-second polling which caused DOM rebuild issues (dropdowns closing, sliders resetting). WebSocket eliminates these problems. The WebSocket connection shows a status dot (connected/disconnected) with a reconnect button.

### Selective DOM updates
v1's `handleStateUpdate()` updates individual control values in-place using `querySelector` by module-id and control-key. It does NOT rebuild the DOM on state updates. A `dragTs` timestamp per control prevents overwriting values the user is actively dragging (1-second cooldown).

### Debounced control sending
v1 debounces slider input with a 150ms timer and text input with a 500ms timer. Values are sent via `PATCH /api/modules/:id/props/:key`. This prevents flooding the API during rapid slider movement while keeping the UI responsive.

### Module cards with metadata
Each module card shows: name, id, category, core affinity badge, timing stats (fps/avg/min/max — clickable to cycle), class size, heap size. This gives immediate visibility into system health without a separate diagnostics page.

### Type picker for dynamic module creation
A context-aware picker with category chips and search. When adding a child module, only valid child types are shown (e.g. effects can only be added to layers). Types are loaded from `/api/types`.

### 3D WebGL light preview
A point-cloud renderer receives binary frames via WebSocket: `[0x02][w_lo][w_hi][h_lo][h_hi][d_lo][d_hi][R G B ...]`. Mouse/touch orbit camera. This provides visual feedback without external hardware.

### Hierarchical module tree
Modules have parent/child relationships. The side nav shows root modules; clicking one shows its card with children inline. Children can be reordered via drag handles.

### Reset-to-default buttons
Each control with a known default shows a reset button. It highlights when the value differs from default.

## Layout

```
┌─────────────────────────────────────────────────────────┐
│  Status bar: logo, device name, system stats,          │
│              WebSocket dot, reconnect, theme toggle     │
├──────────────┬──────────────────────────────────────────┤
│  Side nav    │  Module cards                           │
│  (module     │  (selected module + children,           │
│   tree)      │   each with controls)                   │
│              │                                         │
│  + add       ├──────────────────────────────────────────┤
│   module     │  3D Preview canvas                      │
│              ├──────────────────────────────────────────┤
│  Community   │  System health (collapsible)            │
│  links       │  Log panel (collapsible)                │
└──────────────┴──────────────────────────────────────────┘
```

- Fixed status bar at top
- Side nav with hamburger menu on mobile, always visible on desktop (≥820px)
- Responsive: side nav auto-hides on narrow screens

## Control Types

Auto-rendered by type. No UI code changes needed when adding new control types to the engine — as long as the UI knows how to render the type.

| Type | UI element | Interaction |
|------|-----------|-------------|
| slider | Range input + numeric display | Debounced (150ms), reset-to-default button |
| toggle | Checkbox | Immediate send on change |
| text | Text input | Debounced (500ms) |
| password | Password input | Debounced, shows dots for existing value |
| display | Read-only text | Updated via WebSocket push |
| progress | Progress bar + percentage text | Updated via WebSocket push |
| button | Action button | Sends value=1 on click |
| color | Color picker | (planned, not yet in v1) |
| dropdown | Select menu | (planned, for enum/mode controls) |

## Communication

### WebSocket (primary)

- Connect to `ws://<host>/ws`
- Server pushes state updates as JSON: `[{id, controls: {key: value, ...}, timing: {...}}, ...]`
- Server pushes binary light preview frames: `[0x02][w16][h16][d16][RGB...]`
- Client sends: nothing (all mutations via REST)
- Status dot: green=connected, gray=disconnected
- Auto-reconnect on connection loss

### REST API

- `GET /api/types` — list available module types with category and metadata
- `GET /api/modules` — list all module instances with full state (controls, timing, hierarchy)
- `POST /api/modules` — create new module (body: `{type, id, parentId}`)
- `DELETE /api/modules/:id` — remove module (must remove children first)
- `PATCH /api/modules/:id/props/:key` — set a control value

### Static assets

- `GET /` → index.html
- `GET /app.js` → app.js
- `GET /style.css` → style.css
- All responses include `Cache-Control: no-cache, no-store, must-revalidate`
- On ESP32: assets embedded as C arrays. On desktop: served from disk.

## Styling

- Dark theme (default): background #1a1a2e, text #e0e0e0, accent #a78bfa (purple)
- Light theme: togglable via button, uses CSS `[data-theme="light"]` overrides
- System-ui font stack (not monospace — v1 uses system-ui for readability)
- Module cards with rounded corners, subtle borders
- Color-coded core affinity badges (C0, C1)
- Responsive breakpoint at 820px

## Module hierarchy

The UI supports a tree of modules:
- **Root modules**: EffectsLayer, DriverLayer, system modules. Shown in side nav.
- **Child modules**: effects, modifiers, layouts, drivers. Shown as cards within their parent.
- Adding: via "+ add child" button on any module card, filtered by valid child types
- Removing: via "✕ delete" button (disabled if module has children — remove children first)
- Reordering: drag handle on child cards within a parent

## What still needs design for v3

- **Layer management** — v1 had EffectsLayer/DriverLayer as root modules. v3 has LayoutGroup/Layer/DriverGroup. The UI needs to reflect this hierarchy.
- **Modifier chain visualization** — show the modifier pipeline visually (not just a list), with order indicators.
- **Light preview protocol** — the binary WebSocket frame format needs to match v3's buffer structure.
- **Config persistence UI** — save/load current module tree configuration.

## Prior art — Canvas View (v2)

### projectMM v2 — Canvas/Node-graph view ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/frontend/index.html), [source](https://github.com/ewowi/projectMM-v2/blob/main/src/frontend/app.js))
v2 introduced a canvas/node-graph view alongside the tree view. Toggle between:
- Tree view (⎇) — hierarchical list (proven in v1 and MoonLight)
- Canvas view (⬡) — modules as draggable nodes with SVG connection lines on a pannable viewport

The canvas view gives a visual representation of the pipeline topology, showing data flow between modules. Powerful for complex setups but adds significant UI complexity (89KB app.js in v2 vs 1909 lines total in v1).

**v3 approach:** Start with tree view only. Introduce canvas view carefully later — it should not be required for basic operation. The tree view must be fully functional standalone.
