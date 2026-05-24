# Plan-13 — Nest child module cards inside their parent card's box

## Context

In the v3 web UI, every MoonModule renders as a card. Before this change, `renderModuleTree` (`src/ui/app.js`) appended **every** card — parent and child alike — as a flat sibling into the single `#main` container. Children only *looked* nested because of a `margin-left` on `.card[data-depth="1"/"2"]`. The parent card's border did **not** enclose its children.

This was surfaced while reconciling the two `ui.md` specs after the repo rename: the flat-indent part shipped in plan-12, but the *containment* part was never built. The draft `docs/moonmodules_draft/core/ui.md` had a stale gap-analysis row conflating the two. The product owner wants the parent card to visibly **contain** its children so the module tree shape is structural, not just an indentation hint.

The promoted spec `docs/moonmodules/core/ui.md` § Module card was updated first to describe the target layout: within a parent card the order is **title row → parent's own controls → `.card-children` block → `+ add child` footer**.

## Decisions locked

- **Children inside the parent box** — child cards live in a new `.card-children` wrapper that is a DOM descendant of the parent card; the parent's border encloses them. (Chosen over a bracket/spine-only treatment or keeping the flat-sibling layout.)
- **Controls above children** — the parent's own controls render above the `.card-children` block; `+ add child` renders below it, at the bottom of the parent box. No collapsible children block (rejected the `localStorage`-per-parent toggle as unneeded complexity).
- **`.card-children` gated on `acceptsChildren(mod)`** (Layer/DriverGroup/LayoutGroup), not on `mod.children.length` — so an empty parent still has a mount point and keeps `+ add child` below an (empty) children block. `.card-children:empty` collapses it visually.
- **Drag-and-drop gate** — `dragover` now accepts a drop only when source and target share the same `.card-children` container (true siblings under one parent), replacing the old `data-depth` equality check which would wrongly match effects under different Layers.

## Implementation steps

Two files changed: `src/ui/app.js` and `src/ui/style.css`. No backend change. `src/ui/ui_embedded.h` is regenerated at build time (`CMakeLists.txt:29-34`).

1. **`createCard`** — returns `{ card, childrenEl }` instead of just `card`. When `acceptsChildren(mod)`, creates a `.card-children` div (with `data-depth = depth+1`) and appends it after the controls, then the `.card-footer` after that. `createCard` has only one caller (`renderModuleTree`), so the return-type change is contained.
2. **`renderModuleTree`** — destructures `{ card, childrenEl }`, appends `card` to `parentEl`, and recurses children into `childrenEl` (guarded by `childrenEl &&`) instead of into the flat `parentEl`.
3. **`attachDragHandlers`** — `dragover` gate changed to `src.parentElement === card.parentElement && card.parentElement.classList.contains("card-children")`. The `drop` handler was untouched — it already resolves the parent via `findParent(mod.name)` and computes `targetIdx` by name, position-independent.
4. **`style.css`** — added `.card-children` (margin-top, margin-left, left accent border, padding-left) and `.card-children:empty { display: none }`. The per-depth `.card[data-depth=...]` rules became background-only (removed `margin-left` and the per-card `border-left` — the wrapper now owns the indent and border). The responsive `@media (max-width:820px)` block's two per-depth margin overrides collapsed into one `.card-children` rule (nesting compounds naturally).

## Verification

- **Build** — `python3 scripts/build/build_desktop.py` from a clean `build/` (the old build cache held stale `projectMM-v3` paths from the directory rename and was removed). Zero warnings; `ui_embed` regenerated `ui_embedded.h` from the edited assets.
- **Rendered DOM** — drove headless Chrome via CDP, selected the `Layer` root (`localStorage['mm_selectedRoot']`), asserted on the live DOM: root card is `Layer`; its child order is exactly `["card-title", "card-children", "card-footer"]`; the `.card-children` wrapper holds 2 child cards (`Noise`, `Mirror`) as direct descendants; `+ add child` sits below the children block. All assertions passed.
- **Tests** — `ctest` 1/1 passed, `./build/test/mm_scenarios` 8/8 passed. UI-only change, no C++ touched, so test results unaffected as expected.

## Notes

- The `build/` directory was deleted and regenerated because the `projectMM-v3` → `projectMM` directory rename left an absolute path in `CMakeCache.txt`. This is unrelated to the feature.
- Per CLAUDE.md minimalism: changes confined to `src/ui/`, no new files, no inheritance — one struct-shaped return value and one new CSS class.
- Pre-commit gates (ESP32 build, platform boundary, KPI, etc.) not run — this is the product owner's gate to open. Git left untouched.
