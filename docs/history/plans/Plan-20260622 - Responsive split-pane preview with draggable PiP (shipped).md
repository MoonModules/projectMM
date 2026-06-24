# Plan — Responsive preview: docked split-pane (wide) ↔ draggable PiP (narrow)

## Problem

The 3D preview and the module cards stack **vertically** inside `.main-area`: a sticky `.preview-wrap` (aspect 1/1, `max-height: 50vh`, scroll-shrinks to ~25vh) sits above `#main` (cards, capped 500px, centered). On short or small screens the preview eats most of the viewport height even when configuring a module unrelated to the 3D view (e.g. Network/SSID), leaving the cards crammed into a narrow column far down the page; on wide screens there's large empty space *beside* the 500px card column while the preview hogs vertical space *above* it. The vertical stack is the worst fit for short/wide screens.

## Model (product-owner decisions)

One canvas, two modes, switched by width with a manual override. "Always visible, sometimes as a small popup."

- **Mode A — docked split-pane** (wide, ≥ ~960px): `.content` is a 3-column row — nav (200px) · preview (flex:1, sticky, fills its pane height) · cards (**fixed ~480px**, own `overflow-y:auto`, full height). The scroll-shrink hack is removed: the preview is stable, only the cards column scrolls. Industry standard: editor+canvas (Blender / Figma / VS Code).
- **Mode B — floating PiP** (narrow < ~960px, OR docked-preview manually dismissed): the **same** canvas moves into a fixed-position, **draggable, corner-snapping** card (~160px), with a drag handle + expand + close (×). Cards take the full content width. Industry standard: YouTube-mobile PiP.
- **Switching**: a `ResizeObserver` / matchMedia listener toggles a class on `.content` (`mode-docked` ↔ `mode-pip`); CSS does the layout, and the preview's existing `resize` handler (preview3d.js:180, renders at `clientWidth/clientHeight`) re-fits the canvas — so a dynamic window resize pops in/out smoothly with no reload or state loss.
- **PiP trigger**: auto on narrow + a manual toggle on wide (pop the preview out to reclaim card space).
- **PiP dismiss**: × fully hides it; a small "show preview" affordance (status-bar icon or floating pill) brings it back.

## Files

- **`src/ui/index.html`** — restructure `.content`: keep `#nav`; wrap preview + cards so they're siblings in a row (`.workspace` flex: `.preview-pane` + `#main`). Add the PiP chrome (drag handle, expand/close buttons, the re-show pill). The `<canvas id="preview">` stays one element — it's *reparented* (or its wrapper is restyled) between modes, never duplicated (one WebGL context).
- **`src/ui/style.css`** — `.content` row layout; `.preview-pane` (sticky, flex:1) + `#main` (fixed 480px, `overflow-y:auto`, `height: calc(100vh - 44px)`) for docked. `.mode-pip` rules: preview becomes `position:fixed`, small, draggable; `#main` goes full-width. The `<820px` block + a new `~960px` breakpoint drive the auto-switch. Remove `.preview-wrap` sticky-scroll styling + the `max-height:50vh`.
- **`src/ui/preview3d.js`** — replace `setupShrink` (scroll-shrink) with `setupLayout`: the mode toggle (matchMedia/ResizeObserver) + PiP drag/snap + dismiss/re-show. Keep the existing `resize` re-fit. Drag = pointer events, clamp to viewport, snap to nearest corner on release; persist PiP corner + dismissed state in localStorage (hostile-storage guarded, like the other UI prefs).
- **`src/ui/app.js`** — `setupShrink()` call site → `setupLayout()`.

## Verification

- `node --check` the JS; manual responsive sweep: wide (docked split), drag-narrow (auto-pops to PiP, canvas re-fits), drag-back-wide (re-docks), PiP drag + corner-snap, ×-dismiss + re-show, mobile (<820px nav drawer still works with PiP). On a real device (S3 UI) at phone width.
- No backend change → no ctest/scenario/ESP32 impact; the commit gates that fire are spec (none — no control names change) + the build only if `src/ui` compiles into the binary (it's embedded via `embed_ui.cmake`, so a desktop build confirms the embed).
- Confirm the preview still renders binary frames in both modes (one canvas, one WebGL context throughout).

## Risks / notes

- **One WebGL context**: the canvas must never be duplicated — reparent or restyle in place, or the context is lost. Test the dock↔PiP transition keeps rendering.
- **Drag vs. orbit**: the PiP's drag handle must be a separate element from the canvas, or dragging the window fights the camera-orbit pointer handler (preview3d.js owns `touch-action:none` on the canvas).
- **Cards column height**: `height: calc(100vh - 44px)` with its own scroll means the page itself no longer scrolls in docked mode — verify the status bar + nav still behave.
- Pure front-end, UI-only; no protocol/control/spec change.
