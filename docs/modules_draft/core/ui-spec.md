# UI Specification

## Principles

- Three hand-maintained files: `index.html`, `app.js`, `style.css`.
  No frameworks, no build tools, no npm.
- **MoonModule-driven.** The UI has zero hard-coded knowledge of
  specific effects, layouts, modifiers, or drivers. It queries the
  system for the current MoonModule tree and renders it generically.
- Adding a new MoonModule with controls requires **zero changes** to
  the UI files.
- Controls are auto-rendered based on type (uint16 → slider, bool →
  checkbox, text → text input).
- Served by the embedded HTTP server (system MoonModule).
- Cache-Control headers prevent stale assets.

## Layout

```
┌─────────────────────────────────────────────────────┐
│  Header: projectMM v3 + logo                       │
├──────────────┬──────────────────────────────────────┤
│  Sidebar     │  Main pane                          │
│  (module     │  (controls for selected module)     │
│   tree)      │                                     │
│              │                                     │
│              │                                     │
│              │                                     │
│              │                                     │
│              │                                     │
└──────────────┴──────────────────────────────────────┘
```

Left sidebar with module tree, main pane with controls for the
selected module.

## Sidebar Content

The sidebar shows the full MoonModule tree, organized by function:

### Layer Section

Per layer (initially just "Layer 0"):

- **Effect dropdown** — select which effect is active on this layer.
  Lists all available effects by name. Switching immediately takes
  effect (no save button).
- **Active modifiers display** — shows the modifier chain in order:
  "Mirror → Rotate". Order matters (mirror-then-rotate produces
  different output than rotate-then-mirror).
- **Add modifier buttons** — one button per available modifier
  ("+Mirror", "+Rotate"). Clicking adds the modifier to the end
  of the chain.
- **Clear modifiers button** — removes all modifiers from the layer.

### Module Sections

Grouped by type, each with a colored left-border indicator:

- **Effects** (teal border) — list all available effects by name
- **Modifiers** (purple border) — list all available modifiers
- **Layouts** (yellow border) — list all available layouts
- **Drivers** (red border) — list all available drivers

Clicking a module item selects it and shows its controls in the
main pane. Selected item is highlighted.

## Main Pane (Controls)

Shows the controls of the selected module:

- **Title** — module name
- **Controls** — auto-rendered by type:
  - `Uint16` → range slider with numeric value display. The slider
    respects min/max from the control definition. Value is sent on
    mouse/touch release (not during drag) to avoid flooding the API.
    Polling pauses while dragging to prevent DOM rebuild.
  - `Bool` → checkbox with "on"/"off" label. Value sent immediately
    on change.
  - `Text` → text input. Value sent on change (blur/enter).
- If no module is selected: "Select a module to view its controls."
- If module has no controls: "No controls."

## Interaction Rules

### Polling

- The UI fetches `/api/state` every 1 second to stay in sync.
- During slider drag (`interacting` flag), polling is paused to
  prevent DOM rebuild that would reset the slider.
- After actions (effect switch, modifier add/remove), an immediate
  fetch is triggered after 100ms to show updated state.

### DOM Stability

- The sidebar is built once and only rebuilt when structural changes
  occur (effect switch, modifier add/remove, module selection).
- During normal polling, only dynamic text (active modifiers display)
  is updated without rebuilding the DOM.
- This prevents the effect dropdown from closing mid-interaction
  due to a poll-triggered DOM rebuild.

### Cache Prevention

- All HTTP responses include `Cache-Control: no-cache, no-store,
  must-revalidate`.
- Developers should hard-refresh (Cmd+Shift+R) after rebuilding
  the app binary during development.

## REST API

The UI communicates with the engine via these endpoints:

### GET /api/state

Returns the full MoonModule tree as JSON:

```json
{
  "layers": [
    {
      "index": 0,
      "bufferSize": 4096,
      "effects": ["Rainbow 2D"],
      "modifiers": ["Mirror"]
    }
  ],
  "effects": [
    {
      "index": 0,
      "name": "Rainbow 2D",
      "controls": [
        {"name": "speed", "type": 0, "value": 1, "min": 1, "max": 255}
      ]
    }
  ],
  "modifiers": [...],
  "layouts": [...],
  "drivers": [...]
}
```

Control types: 0 = Uint16, 1 = Bool, 2 = Text.

### POST /api/control/{type}/{moduleIndex}/{controlIndex}

Set a control value. Body: `{"value": ...}`

- `type`: "effect", "modifier", "layout", "driver"
- `moduleIndex`: index in the type's array
- `controlIndex`: index within the module's controls

Value format depends on type:
- Uint16: `{"value": 42}`
- Bool: `{"value": true}`
- Text: `{"value": "192.168.1.70"}`

### POST /api/effect/{layerIndex}/{effectIndex}

Switch the active effect on a layer.

### POST /api/modifier/add/{layerIndex}/{modifierIndex}

Add a modifier to a layer's chain.

### POST /api/modifier/remove/{layerIndex}

Clear all modifiers from a layer.

## Styling

- Dark theme: background #1a1a2e, text #e0e0e0, accent #e94560.
- Monospace font stack.
- Module items have colored left borders by type.
- Sliders, checkboxes, and text inputs styled to match dark theme.
- Compact sidebar (~220px width).

## What needs to be designed

- **Multiple layers.** Currently hard-coded to "Layer 0". Need UI to
  add/remove layers, reorder effects and modifiers within a layer.
- **Drag-and-drop.** Reorder modifiers within a layer, drag effects
  between layers.
- **Live preview.** Canvas-based light preview in the browser
  (alternative to requiring an ArtNet receiver for visual feedback).
- **Responsive.** Current layout doesn't adapt to mobile screens.
- **WebSocket.** Replace polling with real-time push updates.
- **Config save/load.** UI for persisting the current module tree
  configuration to disk/flash.
- **Additional control types.** Color picker (RGB), dropdown/enum,
  grouping/sections within a module's controls.
