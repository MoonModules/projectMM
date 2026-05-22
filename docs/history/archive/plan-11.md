# Plan-11 — UI rewrite to ui-spec.md baseline (item 12)

## Context

The v3 web UI today is a thin first cut: `src/ui/index.html` (24 lines), `app.js` (576 lines), `style.css` (156 lines). It works but doesn't reflect what `docs/moonmodules_draft/core/ui-spec.md` lays out — the spec catalogues the v1 patterns proven at scale and the gap analysis between v1 and current v3.

Plan-11 rewrites the UI to that spec baseline. Once status bar + card layout + 9 control types + type picker + no-rebuild contract are in, any new MoonModule renders generically with zero UI-code cost — the spec's core promise. This is the prerequisite for an "effect/module switching from UI" user feature: the switching mechanism *is* the type picker plus reorder/delete buttons on top of a spec-compliant card layout.

**Scope:** The 8 items in ui-spec.md § Plan-12 scope (at plan time `docs/moonmodules_draft/core/ui-spec.md`; the spec has since shipped to [docs/moonmodules/core/ui.md](../moonmodules/core/ui.md)) — status bar, card layout, 9 control types, type picker, reset-to-default, light/dark theme, WS lifecycle, 3D preview polish — plus four items promoted from § Deferred to 1.x for this iteration: fps/ms toggle per card, reboot button with crashed-state styling, system stats in header (uptime · heap), drag handles for child reorder.

**Intended outcome:** Plan-11 owns its own engine additions where the UI needs them. After plan-11 lands, the v3 UI matches the spec, the spec promotes from `_draft/` to `moonmodules/`, and this plan archives as `docs/history/plan-11.md` (next sequential; plan-10 untouched).

## Decisions already locked

- **Engine additions are owned by plan-11.** Three small endpoints + supporting code: `GET /api/types`, `POST /api/modules/<n>/move`, `POST /api/reboot`, plus `MoonModule::moveChild`, `ModuleFactory` role capture at registration, `SystemModule::bootReason` control. ~150 LOC total backend.
- **Up/down icon buttons AND drag handles ship together.** Up/down for touch users, drag for desktop. Both call the same `POST /api/modules/<n>/move` endpoint with `{to: N}` (absolute target index). Up = `to: currentIndex-1`, down = `to: currentIndex+1`, drag = `to: dropTargetIndex`. One endpoint, one round-trip per move regardless of distance.
- **`POST /api/modules/<n>/move` triggers `scheduler_->rebuild()`** after a successful move. This is unnecessary for effect-only moves but required for modifier and layout moves (LUT depends on modifier order; physical→logical mapping depends on layout). Same pattern as the existing add/delete handlers — simple, correct, no need to special-case by `role()`.
- **No-rebuild contract preserved.** Existing `dragTs` 1s cooldown (current `app.js` L319) and `if (ctrl.hidden) continue` (current `app.js` L154 — plan-10 feature) stay. The rewrite extends what's there, doesn't restart from zero.
- **localStorage key migration.** Current `mm.selectedModule` → spec's `mm_selectedRoot`. Read both on init, prefer new; one-release fallback. New keys `mm_theme`, `mm_timing_mode` per spec.
- **System stats in header** uses existing `SystemModule.uptime` control from `/api/state` WS push. No new endpoint needed for that — the data is already there.
- **Reboot button needs a backend endpoint.** Adds `platform::reboot()` (ESP32: `esp_restart()`, desktop: `exit(0)`) + `POST /api/reboot` handler. Crashed-state badge driven by a new `SystemModule.bootReason` ReadOnly control populated from `esp_reset_reason()`.
- **Plan archives as `docs/history/plan-11.md`** (plan-10 untouched in history).

## Engine additions

Required so the UI scope items have endpoints to call against. All additive — no existing behavior changes.

### `src/core/MoonModule.h` — `moveChildTo(child, newIndex)`

Move child to an absolute position 0..childCount-1. Shifts intervening siblings (memmove-style). Returns `false` if child not found or newIndex out of range. Sits alongside existing `addChild`/`removeChild`/`replaceChildAt`. ~18 LOC.

### `src/core/ModuleFactory.h` — capture role at registration

Extend `TypeEntry` with `ModuleRole role`. Template `registerType<T>()` discovers role via a probe instance: `T probe; ModuleRole r = probe.role();` then forwards to the non-template overload. Add `static ModuleRole typeRole(uint8_t i)` accessor. ~10 LOC.

### `src/core/HttpServerModule.h` — three new endpoints

- `GET /api/types` → `{"types":[{"name":"NoiseEffect","role":"effect"}, …]}`. Role string lowercased from `ModuleRole` enum. UI uses it for the picker's context filter (parent's `role()` → accepted child roles, derived in JS). ~25 LOC.
- `POST /api/modules/<n>/move {to: N}`. Route uses strict-suffix match — path must end with `/move` exactly, not `/movex`. Resolves module by name, finds its parent, calls `parent->moveChildTo(mod, to)`, marks dirty, notes filesystem dirty, calls `scheduler_->rebuild()` so any LUT depending on modifier/layout order rebuilds. ~30 LOC.
- `POST /api/reboot`. Calls `platform::reboot()` and returns `{"ok":true}` (the response races the actual restart on ESP32; that's fine — the UI sees a WS disconnect and reconnects when the device comes back up). ~10 LOC.

### `src/platform/platform.h` + impls — `reboot()`

Add `void reboot();` to the API. ESP32 impl: `esp_restart()`. Desktop impl: `std::exit(0)` (a no-op or exit; matches "smoke-tested but not load-bearing" expectations on desktop). ~6 LOC across three files.

### `src/core/SystemModule.h` — `bootReason` ReadOnly control

Add a ~32-byte `bootReasonStr_` member. In `setup()`, query `esp_reset_reason()`, map enum to "POWERON" / "SW" / "PANIC" / "WDT" / etc., snprintf into the buffer. In `onBuildControls`, bind it as a ReadOnly control. On desktop the buffer reads "OK" (no reset reason concept). The UI uses this to set the reboot button's `data-crashed` attribute when the value indicates an unclean prior boot (PANIC/WDT/BROWNOUT). ~20 LOC.

## UI rewrite

### `src/ui/index.html` — full restructure (was 24 lines, target ~50)

- Fixed top **status bar** (`<header>` becomes 44px fixed): brand logo + wordmark, device name (from `System.deviceName`), system stats span (`uptime · NN KB heap`), spacer, WS dot, reconnect button, **reboot button** (with crashed-state class hook), **theme toggle** button.
- Sticky **3D preview canvas** wrapper below status bar.
- Main column: single column `max-width: 500px; margin: 0 auto`, card list. Root modules rendered with depth=0; children indented with depth+1, etc.
- `<body data-theme="dark">` default.

### `src/ui/app.js` — extend existing 576 lines

**Preserve as-is:** `dragTs` cooldown (L319), `if (ctrl.hidden) continue` (L154), the 7 working control type renderers (uint8 slider, uint16, bool, text, display, select, progress).

**WebSocket lifecycle (spec item 7):**
- Rewrite `connectWs()` (L13-43) with exponential backoff: `wsRetryMs` 500 → 1000 → 2000 → 4000 → 5000, reset on `onopen`.
- Add `setInterval(() => ws.readyState===1 && ws.send("ping"), 25000)` keepalive on connect; clearInterval on close.
- Module-level `let wsPaused = false`; gate `onmessage` body on `!wsPaused`.
- `document.addEventListener("visibilitychange", () => wsPaused = (document.visibilityState === "hidden"))`.
- `window.addEventListener("pageshow", e => { if (e.persisted) { wsPaused = false; if (ws.readyState !== 1) connectWs(); } })` for Safari bfcache.

**Status bar wiring (spec item 1 + 4 promoted items):**
- Device name from `state.modules[].controls[]` where name === "deviceName".
- System stats from `SystemModule.uptime` + free heap (computed via `freeHeap` field on /api/system or `dynamicBytes` from /api/state). Pull from the existing /api/state WS push — no new endpoint. Format: uptime as `Xd Yh Zm Ws`, heap as KB.
- Theme toggle button (`☀/🌙`): reads/writes `localStorage['mm_theme']`, sets `body.dataset.theme`.
- Reconnect button (already wired): force `ws.close()` then `connectWs()`.
- **Reboot button**: confirm dialog `confirm('Reboot device?')`, then `POST /api/reboot`. Add red border (`data-crashed="true"`) when `SystemModule.bootReason` indicates an unclean prior boot.

**Card rendering (spec item 2 + per-card fps/ms toggle):**
- `createCard(mod, depth)` accepts depth. Sets `card.dataset.depth = depth`. `renderCards()` recurses children with `depth+1`.
- Title line `[name] [stats] [actions]`. **Actions** appear for children whose `role()` is reorderable (Effect, Modifier):
  - `↑` up button → `POST /api/modules/<name>/move {delta:-1}`
  - `↓` down button → `POST /api/modules/<name>/move {delta:+1}` (both disabled at extremes)
  - `✕` delete button → `DELETE /api/modules/<name>`
  - Drag handle `☰` (desktop) — see drag section below
- **Stats span** is clickable; cycles fps↔ms display via `localStorage['mm_timing_mode']`. Shows `loopTimeUs` from `/api/state` formatted per mode. Single global toggle affects all cards.

**Control rendering (spec item 3 + 5):**
- Extend `createControl()` (L165-293) with three new branches:
  - `button` — `<button>` calls `sendControl(name, 1)` on click, no echo.
  - `password` — `<input type="password">` + hold-to-peek button (`onmousedown` shows, `onmouseup`/`onmouseleave` hides), 500ms debounce, placeholder shows `•` repeated to value length.
  - `time` — read-only formatted via `fmtTime(seconds)` helper → `Xd Yh Zm Ws`. Updated via WS push.
- Add matching update branches in `updateModuleControls()` (L312-371).
- **Reset-to-default button (↺)**: in `createControl()`, when `ctrl.default !== undefined` (engine adds this field — see Engine additions below), append a small button. Class `dim` vs `active` based on `ctrl.value === ctrl.default`. Click → `sendControl(name, ctrl.default)`. `updateResetButtonState(mid, key, ctrl)` called from `updateModuleControls()` to refresh state.

**Type picker (spec item 4):**
- `roleAcceptsChild(parentRole, childRole)` map (~10 LOC): `Layer → [effect, modifier]`, `DriverGroup → [driver]`, `LayoutGroup → [layout]`, others → `[]`.
- `openTypePicker(parentMod, anchorEl)`:
  - Fetches `/api/types` (cache for session).
  - Filters by `roleAcceptsChild(parentMod.role, t.role)`.
  - Renders inline list (not modal) below anchor: search input, filtered list, Create/Cancel buttons.
  - Keyboard nav: ↓ enters list from search, ↑↓ moves selection, Enter → `POST /api/modules {type, parent_id: parentMod.name}` then re-fetch state, Esc closes.
  - Search filters by substring on type name.
- `+ add child` button in each card's footer (for parents that accept children) → `openTypePicker(mod, button)`.
- `+ add module` button somewhere at the top (top-level addition, parent_id null/missing).

**Drag-to-reorder (promoted from Deferred):**
- `☰` drag handle in reorderable child cards (alongside existing up/down).
- On `dragstart`: store source card id in `dataTransfer`, add `.dragging` class.
- On `dragover` on a sibling card: `preventDefault()` to allow drop, add `.drag-over` class.
- On `drop`: compute delta from indices (source index vs drop target index), call `POST /api/modules/<name>/move {delta}` enough times to reach target (or extend the endpoint to accept absolute index; **decision: keep `delta:-1|+1` and call multiple times** — simpler endpoint, drag is short-range anyway. If we move 3 down, call delta:+1 three times in sequence with awaits between).
- `dragleave` / `dragend`: clean up classes.

**3D preview polish (spec item 8):**
- Wrap canvas in `.preview-wrap { position: sticky; top: 44px; z-index: 5; }`.
- Touch handlers (`touchstart`/`touchmove`) mirroring mouse drag for mobile orbit.
- **Sparse vertex buffer**: in `renderPreviewFrame()` (L459+), pre-count non-black voxels; skip RGB=0 in upload loop. Halves GPU work for typical effects.
- **Cache `lastFrame`** (the buf) so a `redrawFromCache()` can be called from orbit handlers between server frames — orbit feels smooth even at low FPS.
- Scroll listener on main column → set `--preview-shrink` 0→1 over 0→300px scroll, recompute canvas height via `requestAnimationFrame` throttling. Preview shrinks to 50% of natural height when fully scrolled.
- GLSL vertex: `gl_PointSize = uPtSize / gl_Position.w` (depth-corrected).
- GLSL fragment: tighten disc to `d > 0.25 → discard`, soft brightness falloff via `smoothstep(0.10, 0.25, d)`.

**localStorage migration:**
- On init, read `localStorage['mm_selectedRoot']` first, fall back to `localStorage['mm.selectedModule']`. Write only to the new key. One-release fallback.
- Add `mm_theme` (default `"dark"`) and `mm_timing_mode` (default `"fps"`) keys.

### `src/ui/style.css` — restructure (was 156 lines, target ~350)

**Layer 1 — variables.** Define `:root` CSS variables for the palette:
```
--bg-0, --bg-1, --fg, --fg-muted, --accent, --accent-soft,
--card-bg-0, --card-bg-1, --card-bg-2 (depth-based backgrounds),
--border, --green (connected/ok), --red (error/crashed), --yellow (warn)
```
Existing dark colors refactor to use them.

**Layer 2 — `[data-theme="light"]` overrides.** ~10-12 variable flips. Per spec, ~30 lines total.

**Layer 3 — structural rules.**
- Fixed status bar (44px, position:fixed top, flex row, gap 8px).
- Sticky `.preview-wrap` (top:44px, z-index:5).
- Main column max-width 500px, centered, padding-top to clear sticky preview.
- Card depth backgrounds via `.card[data-depth="0/1/2"]` + left-border accent on indented children.
- 600px → 820px breakpoint per spec.

**Layer 4 — component styles.**
- `.card`, `.card-title`, `.card-stats` (cursor:pointer for fps/ms toggle), `.card-actions`, `.card-btn` (square 26×26 button), `.card-btn-del` (red variant).
- `.drag-handle` (cursor:grab).
- `.reboot-btn`, `.reboot-btn[data-crashed]` (red border).
- `.type-picker` (inline list styling).
- `.reset-btn` dim/active states.
- `.peek-btn` for password.

## Test additions

Three small additions in `test/`:

- `test_movechild.cpp` — verify `MoonModule::moveChild` swaps siblings, returns false on out-of-range, doesn't disturb non-child slots. ~40 LOC.
- `test_module_factory.cpp` — verify the role probe captures correctly via `registerType<T>("…")` and `typeRole(i)` returns expected enum for the 10+ registered types in `main.cpp`. ~30 LOC.
- `test_system_module.cpp` — already exists; extend with a bootReason-present check (desktop value should be a non-empty string). ~5 LOC added.

`CMakeLists.txt` updated to include the two new test files.

## Documentation

- **`docs/moonmodules/core/SystemModule.md`** — add `bootReason` to the controls list, note the UI's crashed-state behavior.
- **`docs/moonmodules/core/HttpServerModule.md`** — add the three new endpoints to the API table, with shapes.
- **`docs/moonmodules/core/MoonModule.md`** — add `moveChild` to the children API list (alongside `addChild`/`removeChild`/`replaceChildAt`).
- **`docs/testing.md`** — add entries for `test_movechild.cpp` and `test_module_factory.cpp`.
- **`ui-spec.md` final cleanup** — once the UI matches, the Quick guide's deferred items get updated (the 4 promoted items move out of Deferred-1.x and into "implemented"). Then `git mv docs/moonmodules_draft/core/ui-spec.md docs/moonmodules/core/ui-spec.md`.
- **`docs/plan.md`** — remove the `## 12.` section per the file's "Completed items are removed" rule.
- **`docs/history/plan-11.md`** — 1:1 copy of this plan file (per CLAUDE.md's "Save plan to history" rule).

## Critical files

**Engine:**
- [src/core/MoonModule.h](src/core/MoonModule.h) — add `moveChild`
- [src/core/ModuleFactory.h](src/core/ModuleFactory.h) — role at registration
- [src/core/Control.h](src/core/Control.h) — add `default` field + `setDefault(i, val)` helper
- [src/core/HttpServerModule.h](src/core/HttpServerModule.h) — 3 endpoints, emit `default` field
- [src/core/SystemModule.h](src/core/SystemModule.h) — `bootReason` control
- [src/platform/platform.h](src/platform/platform.h) — declare `reboot()`
- [src/platform/desktop/platform_desktop.cpp](src/platform/desktop/platform_desktop.cpp) — `reboot()` stub
- [src/platform/esp32/platform_esp32.cpp](src/platform/esp32/platform_esp32.cpp) — `reboot()` via `esp_restart()`

**UI:**
- [src/ui/index.html](src/ui/index.html) — full restructure
- [src/ui/app.js](src/ui/app.js) — extend existing 576 lines
- [src/ui/style.css](src/ui/style.css) — restructure with CSS variables + light theme

**Tests:**
- [test/test_movechild.cpp](test/test_movechild.cpp) (new)
- [test/test_module_factory.cpp](test/test_module_factory.cpp) (new)
- [test/test_system_module.cpp](test/test_system_module.cpp) — extend
- [test/CMakeLists.txt](test/CMakeLists.txt) — register

**Docs:**
- [docs/moonmodules/core/SystemModule.md](docs/moonmodules/core/SystemModule.md)
- [docs/moonmodules/core/HttpServerModule.md](docs/moonmodules/core/HttpServerModule.md)
- [docs/moonmodules/core/MoonModule.md](docs/moonmodules/core/MoonModule.md)
- [docs/testing.md](docs/testing.md)
- `git mv docs/moonmodules_draft/core/ui-spec.md docs/moonmodules/core/ui-spec.md`
- [docs/plan.md](docs/plan.md) — remove step 12
- [docs/history/plan-11.md](docs/history/plan-11.md) — new

## Existing utilities to reuse (do NOT duplicate)

- `controls_` array + `addUint8/addBool/addText/addSelect/addReadOnly/addProgress` on every MoonModule
- `MoonModule::role()` returning `ModuleRole::{Generic, Effect, Modifier, Driver, Layout}` — picker filter derives from this in JS
- `MoonModule::children_` array + `addChild`/`removeChild`/`replaceChildAt` (plan-10) — `moveChild` joins these
- `MoonModule::loopTimeUs` + `dynamicBytes()` — already in `/api/state`, drive the fps/ms toggle
- `MoonModule::enabled()` / `setEnabled()` — already wired by HttpServerModule for the per-card checkbox; no new code needed for the enabled toggle UX
- `FilesystemModule` (plan-10) — persistence "just works" for new controls (bootReason isn't persisted because it's `ReadOnly`)
- `dragTs` cooldown + `if (ctrl.hidden) continue` in `app.js` — preserve, don't rewrite
- `ControlDescriptor.hidden` flag (plan-10) — already supported end-to-end

## Risks and mitigations

1. **`ui_embedded.h` regen.** UI files served from disk on desktop, but baked into `src/ui/ui_embedded.h` at ESP32 build. After UI edits, regen via `build_esp32.py` (CMake should regen automatically on file timestamps). Verify via ESP32 smoke test before declaring done.
2. **localStorage migration.** Renaming the selected-module key silently drops old values once. Mitigation: read both old and new on init, prefer new. Acceptable one-release migration.
3. **Persistence (plan-10) interaction.** Adding `default` field to `ControlDescriptor` is append-only; doesn't change persistence binary serialization. The new ReadOnly `bootReason` is correctly excluded from persistence (ReadOnly controls are derived, not state). Verify `test_filesystem_persistence.cpp` still passes.
4. **No-rebuild contract.** Card rendering restructure must keep WS state pushes patching values in place via `[data-mid][data-key]` selectors — never call `renderCards()` from `updateValues()`. dragTs cooldown at L319 must still work after restructuring. Drag operations and add/delete DO trigger a re-fetch + re-render of the affected parent only.
5. **Reboot endpoint response race.** `POST /api/reboot` returns 200 then the device restarts; the client may not see the response. Acceptable — the UI's existing reconnect-on-WS-close logic handles the disconnect cleanly. On desktop, `exit(0)` makes the server vanish; localhost smoke test should see clean WS close.
6. **WS reconnect storm.** Exponential backoff without jitter could cause N clients to slam the device. Acceptable for now (one developer + browser).
7. **bootReason on first boot.** Fresh ESP32 with no prior state reports POWERON_RESET, which is normal — UI must NOT show crashed-state for that. Map only PANIC, INT_WDT, TASK_WDT, BROWNOUT to "crashed".
8. **Drag-to-reorder iteration cost.** Multiple `/move {delta}` calls in sequence for a multi-position drop. Acceptable for short-range drags; for long-range, the up/down buttons or repeated drags are fine. Avoid extending the endpoint to absolute-index for now.

## Verification

Per CLAUDE.md pre-commit checklist (10 steps). Specific to this plan:

1. `cmake --build build` — zero warnings (UI changes don't affect build but engine changes do)
2. `ctest --output-on-failure` — existing tests pass + 2 new (`test_movechild`, `test_module_factory`)
3. `./build/test/mm_scenarios` — exit 0
4. `python3 scripts/check/check_platform_boundary.py` — PASS (new `platform::reboot` correctly placed)
5. `python3 scripts/check/check_specs.py` — `10+ modules ok` (HttpServer/SystemModule/MoonModule specs updated)
6. `python3 scripts/build/build_esp32.py` — clean; `ui_embedded.h` regenerated
7. Reviewer agent (Opus) over the staged diff
8. KPI one-liner with PC + ESP32 tick/FPS per CLAUDE.md step 8
9. Hardware smoke test at `http://192.168.1.210/`:
   - UI loads, status bar shows device name + green WS dot + system stats (uptime · NN KB heap)
   - Theme toggle switches dark↔light, persists across reload
   - Tab away 30 seconds, return: WS dot stays green (keepalive working)
   - Click stats line on any card: cycles fps↔ms display; persists across reload
   - Scroll main column: preview shrinks 50% over 300px; mouse-orbit during low-FPS stays smooth (frame cache)
   - On Layer card, click `+ add child` → picker shows only effects + modifiers (NoiseEffect, RainbowEffect, MirrorModifier); search "noi" filters to Noise; Enter creates; new card appears
   - Click ↑ / ↓ buttons on a child → order changes visibly in preview and in `/api/state`
   - Drag a child to a new position → same effect as ↑/↓
   - Click ✕ on a child → confirm dialog → child disappears
   - Click ↺ on a control with default off-default → snaps back, dragTs cooldown applies (no fight with WS push)
   - Click reboot button → confirm dialog → device reboots, WS reconnects, UI returns
   - If a panic/WDT happens on the device, reboot button shows red border on the next boot
10. Documentation: ui-spec.md matches code, promoted out of `_draft/`; SystemModule.md / HttpServerModule.md / MoonModule.md updated; testing.md updated; plan.md step 12 removed; this plan archived as `docs/history/plan-11.md`.

## Out of scope (explicit follow-ups, deferred per ui-spec.md)

- Side nav with drag-reorder of root modules (root order is fixed in main.cpp; the four roots stay)
- Health panel (`<details>` + `GET /api/test`)
- Log panel (`<details>` + WS `{t:"log",m:"…"}`)
- Update-available badge + OTA panel (requires `/api/firmware`)
- Module replace (`✎`) button (requires `POST /api/modules/replace`)
- Core affinity badge (C0/C1) — only meaningful when core pinning lands
- Help links per type (TYPE_TO_DOC mapping)
- Category emoji badge (deferrable — role() suffices)
- Multi-layer UI (plan.md backlog)
- Presets UI
- Canvas/node-graph view
