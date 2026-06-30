# Folder-structure consistency — decision

A *Refactor for simplicity* decision (per CLAUDE.md). Alternatives were weighed; the product owner chose. This records the chosen structure and the work to reach it. Nothing moves until each move is executed deliberately.

## The three axes — and where each one earns a place

1. **Domain** — `core` vs `light`. Already structured (src, docs, tests).
2. **Module type** — effects / modifiers / layouts / drivers. Structured in `src/light/` + `docs/moonmodules/light/`; **missing** in `test/` and `assets/` — added here.
3. **Library** — a module's *origin* (MoonLight, WLED, MoonModules, projectMM-native). **Used only as a doc split and a UI tag — NOT a folder axis** (decision below).

## Decision: `domain / type` folders; library is a tag (+ a doc split)

The structure is **`<core|light> / <type> / Module`**, flat within type. Library does **not** become a folder level.

**Why library is not a folder** (the deciding analysis): an effect's origin is frequently *blended*, not a single fact — e.g. `DistortionWavesEffect` cites MoonLight + WLED + v1 + v2; `GameOfLifeEffect` cites MoonLight + MoonModules + v1; several have no clear origin. A folder forces one answer to a multi-valued question, and a *wrong* or *shifting* answer means a multi-file move (src + assets + tests + the registered `.md` path). It also duplicates the dimension the `tags()` emoji already carries (and the emoji can carry *several* origins; a folder can't). **The end user does not care about a module's library** — they filter by the emoji chip in the UI if they want origin at all. So library stays where it's free and non-duplicative:

- **In code:** the `tags()` emoji (already there; drives the UI origin-filter; can be multi-valued).
- **In docs:** the page split (below) — the one place library earns a structural role, because docs have an explosion problem src doesn't.

This drops every drawback of library-as-folder (fuzzy-origin filing, two-places-disagree, reclassification churn, sparse subfolders, deep paths) at once.

### The target tree

```text
src/light/
  effects/    EffectBase.h, Rainbow.h, Wave.h, DistortionWaves.h, …   (flat per type)
  modifiers/  ModifierBase.h, Multiply.h, Rotate.h, …
  layouts/    GridLayout.h, SphereLayout.h, …
  drivers/    Drivers.h, Correction.h, RmtLedDriver.h, HueDriver.h, …
src/core/  …   (unchanged — no type or library axis)
```

Identical shape for `docs/assets/` and `test/` (below). `src/` itself is **unchanged** — it's already `domain/type`, flat within type; library was never there as a folder and stays out.

## Docs: per-library pages with compact rows (solves explosion *and* giant-file)

Per-module `.md` would be ~65 files post-migration (explosion); one all-effects file would be ~2100 lines (MoonLight's mistake). The middle ground: **one page per library**, each effect a **compact table row**.

```text
docs/moonmodules/light/
  effects_moonlight.md     ← ~30 effects, one row each (~120-150 lines)
  effects_wled.md          ← ~20
  effects_projectmm.md     ← ~15
  modifiers_<library>.md   layouts_<library>.md  drivers_<library>.md   (where a library has them)
docs/moonmodules/core/     ← unchanged: per-module .md (stable count, no explosion)
```

- **Row format** (MoonLight-style): `| Name + tags | gif | one-line description | controls |`. Drops the per-module `Tests` / `Design notes` / `Source` sections — source is derivable from the name, tests are auto-discovered. ~4 lines/effect, so a 30-effect page is ~120 lines.
- **Why library splits docs but not src:** docs are the only area with the explosion problem, and a doc page is *forgiving* about fuzzy origin — a blended-lineage effect goes on one page with its full origin in the row's tags/prose; mis-filing is a one-line edit, not a multi-file move. So the drawbacks that made library-as-*folder* bad are soft for library-as-*doc-page*.
- **`check_specs.py` rewrite:** every registered module's control names must appear somewhere in its library page (preserves the anti-drift guarantee). The registered `.md` arg changes from `light/effects/Rainbow.md` to `light/effects_moonlight.md` (or `#rainbow`). This is migration **Stage 2** work.

## assets + tests: add the missing type-split (mirror src)

The consistency win that's independent of library — make `test/` and `assets/` match src's existing `domain/type` shape:

```text
docs/assets/
  core/      DevicesModule.png, Drivers.png, …
  light/
    effects/    Rainbow.gif, Wave.gif, DistortionWaves.gif, …
    modifiers/  layouts/  drivers/
  boards/  gettingstarted/      ← kept as-is

test/unit/light/
  effects/    unit_Rainbow.cpp, unit_Wave.cpp, …
  modifiers/  layouts/  drivers/
test/unit/core/                 ← unchanged
test/scenarios/light/{effects,layouts,drivers,…}/   (mirror)
```

## The one rule, across all four areas

| | core/light | type | leaf | library |
|---|---|---|---|---|
| **src** | `light/` | `effects/` | `DistortionWaves.h` | tag in `tags()` |
| **assets** | `light/` | `effects/` | `DistortionWaves.gif` | — |
| **tests** | `light/` | `effects/` | `unit_DistortionWaves.cpp` | — |
| **docs** | `light/` | the type+library collapse into the page name → `effects_wled.md` | (row inside) | the page split |

`docs` is the one area where `type` is expressed as part of a **page name** (`effects_<library>.md`) rather than a folder, because the migration compacts docs to per-library pages — and library, the only axis with an explosion problem, rides along in that name. Everywhere else: plain `domain/type` folders, library as a tag.

## Impacted folders + the work

| Folder | Change | Cost | When |
|---|---|---|---|
| `docs/assets/` | flat `screenshots/` → `assets/{core, light/{effects,drivers,layouts,modifiers}}/`; keep `boards/`, `gettingstarted/`. Move 63 files, update ~34 referencing docs. | Medium, mechanical | **Now** (clearly-broken flat area; PO specified the asset structure) |
| `test/unit/`, `test/scenarios/` | add the type-split under `core/`/`light/`; re-path the 81 CMake test entries. | Medium, mechanical | **Now or fold into next test-touching change** |
| `docs/moonmodules/` | per-library pages (`effects_<library>.md`) with compact rows + the `check_specs.py` rewrite; delete the ~21 per-module effect `.md`s. | Medium | **Migration Stage 2** (already planned) |
| `src/` | **unchanged** (already `domain/type`, flat). | none | — |

**Not reshaped** (correctly orthogonal): `test/js`, `test/python` (host-side; test scripts/installer, not modules), `src/platform/{desktop,esp32}` (platform split, already consistent), `src/{core,light}/moonlive` (feature sub-tree).

## Sequencing

1. **Now:** `docs/assets/` reorg (the broken flat area).
2. **Now / next test change:** `test/` type-split.
3. **Migration Stage 2:** per-library doc pages (compact rows) + the `check_specs.py` rewrite + delete the per-module `.md`s.

`src/` needs no move at all — the leanest possible outcome.
