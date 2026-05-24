# Plan-14 — Replace a module with another type

## Context

The web UI could add and delete child modules and reorder them, but not **replace** one — swap a child's type at the same position while keeping its siblings, order, and the parent's selection. The draft `docs/moonmodules_draft/core/ui.md` listed this three times as **Defer-1.x** ("Replace-type button (✎) … needs an atomic backend operation") — the last UI gap-analysis item needing a backend endpoint.

Research found the engine primitive already existed: `MoonModule::replaceChildAt(i, fresh)` swaps a child in place and returns the old one, and `FilesystemModule::applyNode()` already used it during persistence load on a type mismatch. This feature exposes that primitive as an explicit HTTP operation plus a UI button — no new tree-mutation logic.

## Decisions locked

- **HTTP route: `POST /api/modules/<name>/replace`**, body `{"type":"<TypeName>"}`. Mirrors the existing `POST /api/modules/<name>/move` sub-route — same strict-suffix parsing. Not PUT: the body is a swap instruction (`{type}` only), not a full resource representation, so POST-as-action is the honest verb and it keeps the route family uniform (only GET/POST/DELETE exist).
- **Clean swap, fresh defaults** — the replacement is created via `ModuleFactory::create()` and gets its own factory-default control values. No carry-over of matching controls. Matches how `add` works; predictable.
- **Same-role swap (UI)** — the replace picker filters to types whose `role` equals the replaced module's role (effect ↔ effect). The backend does not enforce role, consistent with `add` (the UI owns role filtering).
- **Position, name, selection preserved** — `replaceChildAt` swaps in place, so sibling order and index are kept. Replace only applies to children (roots rejected, like move), so the selected root is unaffected.

## Implementation

### Backend — `src/core/HttpServerModule.h`

- Added an `isReplaceRoute` check beside `isMoveRoute` — strict suffix `"/replace"` (8 chars), POST, body present; extracts the module name and calls `handleReplaceModule`.
- New `handleReplaceModule(conn, name, body)`, modeled on `handleMoveModule` + `handleAddModule`:
  - 404 if module not found; 400 if it is a root (no parent); 400 if `type` missing.
  - Find the child's index in the parent.
  - `ModuleFactory::create(typeName)` — 400 "unknown type" if it fails, **before** touching the tree (never leave a hole).
  - `parent->replaceChildAt(index, fresh)` → old module.
  - Lifecycle on the fresh module: `onBuildControls()` → `setup()` → `onAllocateMemory()` — same phase order as the add path.
  - `old->teardown()` + `Scheduler::deleteTree(old)` — the same teardown+recursive-delete pair `FilesystemModule::applyNode` uses.
  - `scheduler_->rebuild()` so Layer LUT / DriverGroup buffer wiring re-forms.
  - `parent->markDirty()` + `FilesystemModule::noteDirty()` — positional encoding rewrites `<index>.type` automatically.

### UI — `src/ui/app.js`

- `replaceModule(name, newType)` — POSTs to `/api/modules/<name>/replace`, then `refetchState()`.
- The type picker was parameterized: `openTypePicker` and the new `openReplacePicker` both delegate to a shared `openPicker(anchorEl, opts)` where `opts` carries the role filter, the confirm-button label (`create` / `replace`), and the commit action. No copy-paste of the picker.
- A **✎ button** added to `createActionButtons`, between ↓ and ×, on the same reorderable cards. Its click anchors the picker to `replaceBtn.closest(".card")` so the picker drops below the card content rather than inside the 26px button row.

### CSS — `src/ui/style.css`

- No change. The ✎ button reuses `.card-btn`; four 26px buttons + gaps (~116px) fit the title row comfortably.

### Specs

- `docs/moonmodules/core/ui.md` — documented the ✎ action, the `POST /api/modules/<n>/replace` endpoint, the dual-mode type picker, and updated the card diagram + feature summary.
- `docs/moonmodules_draft/core/ui.md` — removed the three now-implemented "Module replace" / "Replace-type button" rows and the cost-table entry.

### Tests — `test/test_replacechild.cpp` (new)

- `replaceChildAt`: swap at the same position with siblings intact; old child detached + replacement parented; out-of-range and null replacement rejected.
- Replace lifecycle: replacement built → set up → allocated, then old torn down — the order `handleReplaceModule` runs.
- Added to `test/CMakeLists.txt`; `docs/testing.md` gained a "Module tree mutation" section (also covering the previously-undocumented `test_movechild.cpp`).

## Verification

- Desktop build clean, zero warnings; `ui_embed` regenerated `ui_embedded.h`.
- `ctest` 1/1 passed (5 new replace cases, 17 assertions); `mm_scenarios` 8/8 passed.
- Live HTTP: replaced an effect under Layer → `{"ok":true}`, new type at the same index, siblings untouched. Error paths confirmed: unknown type → 400, root → 400, missing type → 400, `/replacex` → 404 — tree intact after each.
- Persistence: after a replace, `Layer.json` holds `"1.type":"FireEffect"` at the same positional index with the new type's default control values.
- Headless-Chrome DOM check: action row renders `↑ ↓ ✎ ×`; clicking ✎ opens the picker with confirm label "replace" and the role filter restricted to the target's role (`["effect"]`).

## Notes

- The engine already did this swap internally (`FilesystemModule::applyNode`); this feature is the explicit user-driven version, reusing `replaceChildAt` + `Scheduler::deleteTree` — no new tree logic.
- Per CLAUDE.md: changes confined to `src/core/HttpServerModule.h`, `src/ui/`, one new test file, and specs. `MoonModule.h` reused, not modified.
- Scenario-runner coverage deferred: the runner supports `add_module`/`set_control` only; a `replace_module` step is a follow-up. Replace is covered by the module test + live HTTP verification for now.
- Implemented on branch `next-iteration`. Pre-commit gates (ESP32 build, platform boundary, KPI) not run — the product owner's gate. Git untouched.
