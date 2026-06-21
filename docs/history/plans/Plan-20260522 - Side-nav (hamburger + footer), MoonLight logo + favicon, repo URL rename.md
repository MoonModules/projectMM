# Plan-12 — Side-nav (hamburger + footer), MoonLight logo + favicon, repo URL rename

## Context

The v3 web UI ships with a flat module grid: `index.html` already has a `<nav id="nav">` placeholder inside `.content` > `.main-area`, `style.css` has `#nav { display: none; }`, and `app.js` has an empty `renderNav()`. The draft spec (`docs/moonmodules_draft/core/ui.md`) lists side nav / hamburger / footer as **Defer-1.x** and favicon as **Adopt-1.0** — the product owner chose to implement all of them now.

A MoonLight logo (`docs/assets/moonlight-logo.png`, 320×320, 23.5 KB) goes top-left in the header and as the browser favicon.

Separately: the v1 repo was renamed on GitHub (`ewowi/projectMM` → `ewowi/projectMM-v1`); ~22 files in this repo link to the old URL and need updating.

## Decisions locked

- **Logo delivery:** downscale to 64×64 (~2–4 KB) with `sips`, serve as a real asset (new `/moonlight-logo.png` route + `image/png`), not base64-inlined. 64px covers the ~28px header use at retina and a 32px favicon.
- **Side nav on wide screens:** static left column; the hamburger collapses/expands it. Narrow (<820px): slide-in over a semi-transparent overlay.
- **One root visible at a time.** The side nav selects a root module; `renderCards()` renders only the selected root's subtree, not all roots. (This supersedes the draft spec's "show all roots" note — the product owner wants the v1 single-root pattern.) Selection persists in `localStorage['mm_selectedRoot']`.
- **Footer:** copyright line `© <year> MoonLight` + four social icon links (inline SVG, no extra assets): GitHub `https://github.com/ewowi/projectMM`, Discord `https://discord.gg/TC8NSUSCdV`, Reddit `https://reddit.com/r/moonmodules`, YouTube `https://www.youtube.com/@MoonModulesLighting`. Discord/Reddit/YouTube URLs taken from the v1 frontend.
- **No root drag-reorder** — root order stays fixed in `main.cpp`.
- `sips` (built into macOS) does the downscale.

## Implementation steps

### Part A — Side nav, logo, favicon

1. `sips -z 64 64 docs/assets/moonlight-logo.png --out src/ui/moonlight-logo.png`.
2. `src/ui/embed_ui.cmake` — add a 4th hex array `logoPng[]` + `logoPngLen` (the `hex_to_c_array` helper handles arbitrary bytes).
3. `src/core/HttpServerModule.h` — route `GET /moonlight-logo.png` → `serveFile(..., "image/png")`; add the `logoPng` case to the embedded-array branch. Binary-safe (serveFile writes by `dataLen`).
4. `src/ui/index.html` — `<link rel="icon" type="image/png" href="/moonlight-logo.png">` in `<head>`; `<img id="brand-logo">` at the start of the status bar; `<button id="nav-toggle">☰</button>` first in the status bar.
5. `src/ui/app.js` — hamburger click toggles a body class; overlay-click + Esc close on narrow screens. `renderNav()` populates `#nav` with one entry per root module (calls `selectModule()`), plus a `<footer>` with copyright + social links. `selectModule()` re-renders cards; `renderCards()` renders **only the selected root's subtree**.
6. `src/ui/style.css` — `#nav` becomes a flex column (was `display:none`); footer pinned to bottom; `@media (max-width: 820px)` extended for the slide-in + overlay.

### Part A2 — Backend efficiency check (one-root rendering)

7. Investigate whether the backend can update / push only the visible root's data rather than the whole tree. Today `HttpServerModule` pushes the full module tree on every WS state push (~1 Hz) and the UI patches it all in place. With one-root-visible, the cards for non-selected roots don't exist in the DOM — so the UI already ignores their data, but the **backend still serializes and sends all of it**. Check:
   - Is there a cheap way for the client to tell the server which root it is viewing (e.g. a WS client→server message `{t:"view",root:"Layer"}` or a query param), so the server serializes only that subtree?
   - Does the JSON-state-buffer cost or the per-tick serialization cost matter enough to justify it on ESP32? (HttpServer is currently ~850 µs/tick — measure the state-push portion.)
   - If the saving is real and the change is small, scope it; if it adds protocol complexity for a sub-millisecond gain, record the finding and defer.
   This step is an **investigation** — its outcome (do it / defer with reason) is reported to the product owner before any backend change.

   **Finding (defer):** the state push runs in `loop1s()` — once per second, not per render tick — and the JSON payload is ~700 bytes today (~5 KB worst case for a 20-module system). Serializing one root instead of seven saves a few hundred µs and ~500 bytes once per second, against a ~50,000 µs tick budget — negligible. Sending only the visible root would require a client→server WS "view" message, per-connection view-state tracking (up to 4 clients), and reconnect/switch race handling — bidirectional state protocol complexity that does not pay for itself (CLAUDE.md minimalism). The UI already does the cheap half: non-visible roots have no DOM, so `updateValues()`'s `querySelector` patches no-op for them. Kept the full-tree push; revisit only if the tree outgrows the JSON buffer (the spec's documented fallback is then streaming JSON to the socket, a better fix than per-root filtering).

### Part B — Repo URL rename

8. Replace the old v1 repo URL `github.com/ewowi/projectMM` → `github.com/ewowi/projectMM-v1` across ~22 files (doc "prior art" links + README). Word-boundary aware so `projectMM-v2` / `projectMM-v3` are untouched. Verify with grep.

### Part C — Docs

9. `docs/moonmodules/core/ui.md` — update the Layout ASCII diagram, add a "Side navigation" section (one-root-visible behavior, footer, hamburger, responsive), add logo/favicon to the Status bar section and Feature summary.
10. `docs/moonmodules_draft/core/ui.md` — remove the now-shipped rows (Sidebar nav, Hamburger menu, Footer in side nav, favicon line); keep genuinely-deferred items. Reconcile the "Patterns to consciously NOT carry over" note about single-root-visible — that pattern is now adopted.

### Verify

11. `cmake --build build` (regenerates `ui_embedded.h`), run `./build/projectMM`, browser-check: logo in header, favicon in tab, hamburger toggles nav, one root visible per nav selection, footer links + copyright, <820px slide-in works.

## Critical files

- `src/ui/moonlight-logo.png` (new — generated by sips)
- `src/ui/embed_ui.cmake`, `src/core/HttpServerModule.h` — asset embed + serve
- `src/ui/index.html`, `src/ui/app.js`, `src/ui/style.css` — header, hamburger, nav, footer
- `docs/moonmodules/core/ui.md`, `docs/moonmodules_draft/core/ui.md` — spec move
- ~22 files for the URL rename

## Risks

- The embed pipeline is hex-based and binary-agnostic, so the PNG embeds fine — but `serveFile` must write the body by length, not as a C string (it does; uses `dataLen`).
- The URL rename must not catch `projectMM-v2` / `projectMM-v3` — the replacement targets the bare `projectMM` token only.
