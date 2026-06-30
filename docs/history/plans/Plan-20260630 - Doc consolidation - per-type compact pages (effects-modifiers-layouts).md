# Plan — Doc consolidation: per-type compact pages (effects / modifiers / layouts)

This is **Stage 2** of the [MoonLight migration](./Plan-20260630%20-%20MoonLight%20migration%20(multi-stage).md), narrowed by two product-owner decisions made at planning time:

1. **Docs only.** `src/` stays one-`.h`-per-module (the recorded [folder-structure decision](../../backlog/folder-structure-proposal.md): a blended-origin effect in a per-library *folder* forces a wrong, costly multi-file move). This change touches only `docs/` + `check_specs.py`.
2. **One page per *type*, not per *library* — for now.** Our effect origins are lopsided (10 MoonLight, 1 WLED, 2 FastLED, 4 projectMM-native); a strict per-library page split (the proposal's first draft) yields 1–2-effect sparse pages. Instead: one `effects.md` with **library *sections* inside** (MoonLight / WLED / FastLED / projectMM-native), exactly like the [MoonLight effects page](https://moonmodules.org/MoonLight/moonlight/effects/). The per-library *file* split stays in the backlog as **future growth** — when the WLED/FastLED sets grow, a section lifts into its own file with no row rework.

Inspiration for the compact per-module presentation: the MoonLight effects page (one table row per effect: name + tags, preview gif, one-line description, controls screenshot).

## Scope

- **Effects:** ~17 per-module `.md` → one `docs/moonmodules/light/effects.md`, compact rows, library sections.
- **Modifiers:** ~5 per-module `.md` → one `docs/moonmodules/light/modifiers.md`.
- **Layouts:** ~4 per-module `.md` → one `docs/moonmodules/light/layouts.md`.
- **Drivers:** **unchanged** — one `.md` per driver (PO decision: drivers are larger and structurally distinct — RMT/LCD/Parlio/Network/Hue avg ~215 lines with platform-specific wire contracts a compact row can't hold).
- **Core modules:** unchanged (stable count, no explosion).

## Row format (per module)

`| Name (+ tags emoji) | preview | one-line description | controls |`

- **Drops** the per-module `Tests` / `Prior art` / `Source` / `Design notes` sections: source is derivable from the name, tests are auto-discovered by `generate_test_docs.py`, prior-art/origin rides in the tags emoji + a short note in the description cell. Wire contracts (the one thing the row CAN'T hold) — none of effects/modifiers/layouts have integrator-facing wire contracts except `NetworkReceiveEffect`, which keeps a short protocol note in its description cell (or a footnote under its section).
- ~3–4 lines per module, so `effects.md` ≈ 17 rows ≈ 90 lines (vs ~1100 lines across 17 files today).

## `check_specs.py` rewrite (the contract change)

Today: `check_specs.py` rglobs each module `.h` → requires a matching per-module `.md` whose body mentions every control name. New contract:

- A module registered with `light/effects.md` (the page, not a per-module file) passes if **every one of its control names appears somewhere on that page** (in its row). Same anti-drift guarantee, page-scoped instead of file-scoped.
- The registered `.md` arg in `main.cpp` changes from `light/effects/Rainbow.md` → `light/effects.md` (+ optional `#rainbow` anchor) for every effect; same for modifiers/layouts. Drivers keep their per-driver `.md`.
- Map each module → its page by type suffix (`*Effect` → `effects.md`, `*Modifier` → `modifiers.md`, `*Layout`/`Layouts` → `layouts.md`, `*Driver`/`Drivers` → its own per-driver page), mirroring `asset_dir_for()`.

## Files

- **New:** `docs/moonmodules/light/effects.md`, `modifiers.md`, `layouts.md` (compact-row pages with library sections).
- **Delete:** the ~17 `docs/moonmodules/light/effects/*.md`, ~5 `modifiers/*.md`, ~4 `layouts/*.md` (folded into the pages).
- **Edit:** `src/main.cpp` (registered `.md` path per effect/modifier/layout → the page), `scripts/check/check_specs.py` (page-scoped control-name check + the type→page map), `scripts/docs/generate_test_docs.py` if it links per-module `.md` anchors, and any doc that links a deleted per-module page (re-point to `effects.md#name`).
- **Unchanged:** `src/light/effects/*.h` etc. (code), `docs/moonmodules/light/drivers/*.md` (per-driver), `docs/moonmodules/core/*`.

## Origin sections (from each module's `tags()` today)

- **Effects** — MoonLight 💫: DistortionWaves, LavaLamp, Lines, Metaballs, Particles, Plasma, Rainbow, Rings, Ripples, Spiral · WLED 🌊: Wave · FastLED ⚡️: Fire, Noise · projectMM-native: AudioSpectrum 📊, AudioVolume 🔊, Sine 🌀, NetworkReceive 📡🌙.
- **Modifiers** — MoonLight 💫: Checkerboard, Multiply · projectMM-native: RandomMap, Region, Rotate.
- **Layouts** — projectMM-native: Grid, Sphere, Wheel, Layouts.

## Verification

- `check_specs.py` green under the new page-scoped contract (every control name present on its page).
- Build/tests/scenarios unaffected (docs-only + a script change; no `src/` compile impact).
- No dangling links: every former per-module `.md` link re-points to `page.md#anchor`; `generate_test_docs.py` output still resolves.
- The three pages render as compact tables with library sections; drivers + core docs untouched.

## Out of scope (future growth, kept in backlog)

- **Per-library *file* split** (`effects_moonlight.md`, …) — revisit when a library's effect count earns its own page; the within-page sections make it a lift-not-rewrite.
- **Driver doc consolidation** — drivers stay per-file.
- **gif previews** — adding MoonLight preview gifs into the rows is the migration's separate asset work.
