# UI — deferred items & open questions

Forward-looking companion to the shipped UI spec, [moonmodules/core/ui.md](../moonmodules/core/ui.md). The live spec describes the UI as shipped; this file holds what is **not** in it yet: deferred items, open design questions for 1.0, and the gap analysis against projectMM v1. The backward-looking half (how v1/v2 actually worked, patterns consciously rejected, recorded quirks) lives in [history/v1-inventory.md](../history/v1-inventory.md).

Promote items from here into the live spec as they ship. Delete the file when empty.

## Deferred to 1.x

- Side nav with drag-reorder of root modules (root order is fixed in `main.cpp` today; not painful — and arguably correct, see the gap-analysis note below)
- Health panel (`<details>` + `GET /api/test`)
- Log panel (`<details>` + WS `{t:"log",m:"…"}`)
- Core affinity badge (C0/C1) — only meaningful when core pinning lands
- Module `category()` field — taxonomy beyond `role()` for the picker (decision: derive from `role()` for now)

## Open design questions

These don't block the shipped baseline but should be answered before 1.0:

- **Multi-layer UI** — [architecture.md](../architecture.md) plans for N layers blended into one Drivers. The current card layout shows one Layer. Likely needs a tab/accordion to switch layers, or a per-layer column.
- **Modifier chain visualization** — show the modifier order visually (the `children[]` order is the apply order). Today they're a flat list.
- **Presets** — save/load named bundles of control values. Persistence already stores them; needs a UI surface.
- **Canvas/node-graph view** — v2 attempted this. Powerful for complex setups but doubles the UI surface. A reasonable v3 follow-up gated on user demand.

## Gap analysis — v1 features not yet in v3

Inventory of v1 frontend behaviours v3 lacks, with a recommendation each. Items already shipped (control types, dragTs, two-timescale inputs, type picker, theme, scroll-shrink preview, status bar, reset-to-default, fps/ms toggle, drag reorder, side nav + drawer + footer) are not repeated.

Legend: **Adopt-1.0** (small, high value) · **Defer-1.x** (needs engine work or a feature we lack) · **Drop** (not needed).

### Per-card features

| v1 feature | v3 today | Recommendation |
|---|---|---|
| Header: setup-dot before name | name only | **Defer-1.x** — needs `setupOk()` + `health()` on MoonModule with a real failure mode. Today both would always be `true` / `""`. |
| Module ID shown separately from name | name only | **Defer-1.x** — add when instances need disambiguating (e.g. two effects of the same type under one Layer). |
| Category emoji badge on the card header | role emoji in the picker, not on the card | **Defer-1.x** — `ROLE_EMOJI` already exists in `app.js`; showing it per-card is a small step if card scannability needs it. |
| Core affinity badge (C0/C1) | core pinning not implemented | **Drop** until core pinning is a real engine feature. |
| Memory split heap vs PSRAM | `static+dynamic` shown on the card | **Defer-1.x** — splitting `dynamicBytes` further needs `platform::isPsramPointer(p)` or per-alloc tracking, neither exists yet. |

### WebSocket / panels

| v1 feature | v3 today | Recommendation |
|---|---|---|
| Drag-to-reorder *root* modules (`POST /api/modules/reorder`) | not supported | **Drop** — root order is fixed in `main.cpp` and that's correct: Layouts/Layers/Drivers + system modules are mandatory and ordered. Children reorder via drag already. |
| Log channel `{t:"log",m:"…"}` pushed by server | no server log push | **Defer-1.x** — needs an engine-side log producer. Gate: when boot/network/persistence logs become interesting to non-developers. |
| Schema channel `{t:"schema",modules:[…]}` for tree-shape changes | full `/api/state` push every update | **Drop** — keep the full-tree push; re-evaluate only if WS bandwidth becomes a problem with large trees. |
| System health panel (polls `GET /api/test`, pass/fail table) | none | **Defer-1.x** — needs a runtime `/api/test` that runs the doctest suite; `ctest` covers this for now. |
| Log panel (ring buffer, severity colouring, stick-to-bottom, `GET /api/log` backfill) | none | **Defer-1.x** — pairs with the log WS channel; both arrive together. |

### Cost / decision table

| Cost class | Items |
|---|---|
| Tiny (< 30 lines, no backend) | category emoji badge on the card header |
| Medium (minor backend change) | help-link mapping (needs docs site); richer `category()` than role()-derived |
| Large (separate plan) | health panel + `/api/test`; log panel + WS log channel; OTA + GitHub-update badge; full multi-layer UI; presets UI |
