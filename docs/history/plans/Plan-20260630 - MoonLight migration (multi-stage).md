# Plan — Migrate MoonLight effects / modifiers / layouts (multi-stage)

## Goal & shape

Bring MoonLight's full library of **effects, modifiers and layouts** into projectMM. This is large, so it is **staged**: each stage ships independently, builds on the previous, and is its own `/plan` + commit. This document is the *map* — the per-stage plans get written when we reach them. Stages 1–2 are specified enough to start; later stages are scoped, not detailed.

**Why this matters beyond features:** this migration is the execution vehicle for the **effect-breadth parity gate** in the [projectMM → MoonLight rename plan](../../backlog/rename-to-moonlight.md#must--the-rename-is-a-downgrade-without-these) — taking the MoonLight name requires the library not to feel thin next to the predecessor's 60+ effects. The rename's bar is "enough batches landed," not "every stage done"; this plan is *how* that bar is reached. (The two docs stay in their folders — the rename is the forward-looking backlog item that sets the bar; this is the approved staged plan that meets it — linked, not duplicated.)

Two cross-cutting rules govern every stage, from [CLAUDE.md](../../../CLAUDE.md):

- **Industry standards, our own code.** MoonLight effects are studied for *behaviour and algorithm*, then written **fresh** against our architecture (our `EffectBase`, our primitives, our names). We do **not** trace MoonLight/WLED/FastLED structure or copy code. For *effects specifically* the **visual behaviour is the spec** — we reproduce what the effect looks like faithfully (the product owner's clarification), but the implementation is ours. Prior art credited per-module + in `history/`.
- **A shared light primitive library.** Effects need a common set of small math/colour helpers (a beat/sine oscillator, integer noise, saturating add/subtract, scale, fade, a colour blend, a fast PRNG, draw primitives). projectMM provides these, extending the `color.h` set (`scale8`, `sin8`, `cos8`, `hsvToRgb` already there): **hot-path-tuned** (integer-only, LUT-backed, no float in the per-light path) and **dimension-agnostic where it makes sense** (the product owner's steer: our 3D-native model means a primitive like `drawLine` works 1D→3D, written once, not re-implemented per effect).
  - **Naming follows *Common patterns first* + *Industry standards, our own code*: the recognisable name AND our own implementation.** The LED-embedded world's canonical resource is FastLED, and its names (`beatsin8`, `inoise8`, `qadd8`, `nscale8`, `random8`/`random16`, `ColorFromPalette`) are exactly the ones a contributor recognises in 30 seconds — and consistent with the `scale8`/`sin8` we already ship. So **we use those names** (carrying the established convention), **write our own implementation** against our engine, and **credit FastLED as prior art** in each module's "Prior art" section. The point of the principle is independence-by-construction (own code, own architecture, behaviour pinned by tests), *not* a renamed copy — so the names stay recognisable; only the implementation is ours. Each primitive's design is justified at its introduction site, and we reorganise a borrowed concept when ours is genuinely cleaner (e.g. the dimension-agnostic draw set).

## What exists today (baseline)

- **Primitives:** `src/core/color.h` has `RGB`, `hsvToRgb`, `scale8`, `sin8`/`cos8` (LUT). `src/light/light_types.h` has `Coord3D`, `Dim`, `lengthType`. That's it — no beat/noise/blend/random helpers, no shared palette, no draw primitives.
- **Palette:** none shared. `PlasmaPaletteEffect` hard-codes a 256-entry `RGB palette_[256]` in flash — the pattern to generalise.
- **Effects:** ~21 already ported (Rainbow, Noise, Plasma, Fire, Particles, Metaballs, GameOfLife, Wave, …). GameOfLife (272 lines) is flagged by the product owner as **not faithful — re-port from the real algorithm**.
- **Modifiers:** Multiply, Rotate, Region, Checkerboard, RandomMap. **Layouts:** Grid, Sphere, Wheel.
- **Tags/emoji:** projectMM already has `tags()` + UI-derived role/dim emoji (architecture.md § Web UI). MoonLight's legend (🔥 effect, 💎 modifier, ♫ audio, 🧊 3D, …) becomes the **canonical basis** (product owner's choice).
- **Docs:** one `.md` per module (21 effect specs already), enforced by `check_specs.py` (it `rglob`s each `.h` → a matching `.md`). Moving to **per-library pages** (`effects_<library>.md`, compact table rows) — see Stage 2 and the [folder-structure decision](../../backlog/folder-structure-proposal.md). This requires changing the spec-check contract.
- **Assets:** **already reorganised** to `docs/assets/{core, light/{effects,modifiers,layouts,drivers}, ui}/` (the per-module move done ahead of the migration). Stage 2's gif work is *adding* MoonLight previews into this structure, not re-homing.

## Dependency analysis (what must come first)

1. **Palette** — hard prerequisite. Many MoonLight effects colour via `ColorFromPalette`. Nothing palette-dependent can be faithfully ported until this lands. **Stage 1.**
2. **The shared primitive library** (beat / noise / blend / scale / random / draw) — most effects need several. **Stage 1.**
3. **Tags/emoji legend** — must be settled before batch-migrating, so every migrated module is consistent from the first batch. Cheap; **Stage 1** (a doc + a sweep of existing `tags()`).
4. **Doc model change** — must land before the doc explosion, i.e. before batch migration. A page per **library** (type-first name, underscore-joined): `effects_moonlight.md`, `effects_wled.md`, … (and `modifiers_<lib>.md` etc. only where a library has them; most are effects-only). Library is a *doc* split only — NOT a `src`/`assets`/`tests` folder (those stay `domain/type` flat; library is the `tags()` emoji there). Fixed by the [folder-structure decision](../../backlog/folder-structure-proposal.md). **Stage 2**.
5. **Audio** — audio-reactive effects (♫) depend on `AudioModule::latestFrame()` (already exists). A later stage; not a blocker for non-audio effects.
6. **Moving heads / Art-Net fixtures** — `E_MovingHeads` targets DMX moving heads; depends on fixture-layout + Art-Net (partly present). Last, separate.

No other hidden hard dependencies: our `EffectBase` + extrude (now 1D-along-Y, matching MoonLight) + `Buffer` already provide the render context.

## Stages

### Stage 1 — Foundations (palette + primitives + GoL re-port)  ← start here

The proving-ground stage: build the shared tools, prove them on one hard effect.

- **Palette.** Take **MoonLight's palette set** (~80 gradient palettes, [palettes.h](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Modules/palettes.h) — study + carry the gradient *data*, written into our own format). The definition format is the textbook **gradient-stop** one: a compact `{position, R, G, B, …}` list (position 0..255, terminating at 255), expanded off-loop into a 256-entry lookup. Our `Palette` type + `colorFromPalette(palette, index, brightness)`: the per-light lookup is an array index + one `scale8` (hot-path-tuned; the 256-entry table precomputed on selection, not per frame). Generalises `PlasmaPaletteEffect`'s hard-coded table.
  - **Ownership (decided 2026-06-30):** the **active palette is global**, owned by the **Drivers** container (already the home of global render params — brightness, lightPreset, the shared Correction) via a new `palette` select control. Effects read it through a static `Palettes::active()` seam (the `AudioModule::latestFrame()` pattern), so an effect just calls `colorFromPalette(Palettes::active(), idx)`. This mirrors MoonLight's global `layerP.palette` without needing MoonLight's `ModuleLightsControl` — which, with **presets** and the **external-controller hub** concept, is **backlogged** ([backlog-mixed.md](../../backlog/backlog-mixed.md)) and will absorb the palette control from Drivers when built. Presets are *not* a palette dependency — separate feature, backlogged.
  - Palettes are light-domain → live under `src/light/` (file split decided in the stage plan).
- **The shared primitive library** (file split — one `light/Fx.h` vs focused `light/Beat.h`/`Noise.h`/`Blend.h` — decided in the stage plan; recognisable names, our implementation, FastLED credited as prior art). Hot-path-tuned, integer-only, LUT-backed:
  - *timing/beat:* `beatsin8/16`, `beat8/16`, `triwave8` (on `sin8` + `elapsed()`).
  - *noise:* `inoise8` 1D/2D/3D (promote + generalise `NoiseEffect`'s existing hash — the textbook value/Perlin noise).
  - *blend/scale:* `qadd8`/`qsub8` (saturating), `nscale8`, `fadeToBlackBy`, `blend(RGB, RGB, amt)` (`scale8` already in `color.h`).
  - *random:* `random8`/`random16` — a small fast seedable PRNG, hot-path-cheap (not `std::rand`).
  - *draw (the dimension-agnostic part the product owner called out):* `drawPixel`/`drawLine` (and later `drawCircle`/fill) operating on `Coord3D`, working **1D→3D** against the `Buffer`, so effects and modifiers share one set instead of re-rolling Bresenham per effect. This is the "core absorbs the hard part" principle — geometry primitives live once.
- **Re-port Game of Life** properly — the *real* MoonLight GoL algorithm (the cellular-automaton rules + its palette colouring + blur/mutation it actually uses), on top of the new palette + primitives, replacing the current 272-line version. This is the stage's proof: a real effect that exercises palette + random + neighbour math, done faithfully.
- **Tags/emoji legend.** Write the canonical legend (MoonLight as basis) into architecture.md § Web UI / a tags reference, and sweep existing effects' `tags()` to match. Lightweight.

Stage-1 exit: palette + primitives compile (-Werror), are unit-tested (each primitive pinned: `beatsin8` range, `inoise8` determinism, `qadd8` saturation, `drawLine` endpoints in 1D/2D/3D), GoL re-port renders correctly + has a scenario, tags legend documented. **No doc explosion yet** (GoL keeps its existing single `.md`; the doc-model change is Stage 2).

### Stage 2 — Doc model: per-library pages

Before migrating dozens of effects (which would create dozens of `.md`s), switch the doc model. The naming + structure is fixed by the [folder-structure decision](../../backlog/folder-structure-proposal.md): **`src`/`assets`/`tests` are `domain/type` folders, flat — library is NOT a folder there**, only a `tags()` emoji; **docs** are the one place library splits, as a **page name** (type-first, underscore-joined, matching how you'd read the folder path): `effects_moonlight.md`, `effects_wled.md`, `effects_projectmm.md`, … (and `modifiers_<lib>.md` etc. only where a library has that type — most libraries are effects-only).

- **New per-library pages:** each effect is a **compact table row** — `| name + tags | gif | one-line description | controls |` — dropping the per-module `Tests`/`Design notes`/`Source` boilerplate (source is derivable, tests auto-discovered), so a ~30-effect page is ~120 lines (avoids both the per-module explosion *and* the one-giant-file extreme). Migrate the ~21 existing per-module effect specs into the right `effects_<library>.md` by origin (from each effect's "Prior art"/tags — see the effect inventory reference). A short index page links the set.
- **Rewrite `check_specs.py`** to the new contract: every registered module's **control names** must appear *somewhere in its library page* (preserves the anti-drift guarantee the per-module check gave). The `registerType` second arg changes from `Foo.md` to the library page (`effects_moonlight.md`, or `…#foo`).
- **Gifs:** the per-module asset *move* is **already done** (assets are now `docs/assets/{core, light/{effects,…}}/` per the folder decision). What remains for this stage: **download MoonLight's preview gifs** (the WLED-Utils `FX_*.gif` set + the user-attachment gifs listed on MoonLight's effects/layouts/modifiers/drivers pages) into the matching `light/effects/…` folders as the new effects land, crediting source.
- **Wire-contract docs** that don't fit a table row (the genuinely technical ones — HueDriver's API, NetworkSend's protocols) keep a deeper section; the library page links to it.

Stage-2 exit: the library pages render with gifs, `check_specs.py` green on the new contract, the per-module effect `.md`s deleted (subtraction). This is the "kills the explosion permanently" stage.

### Stage 3+ — Effect migration in batches

With foundations + doc model in place, migrate MoonLight effects in **themed batches**, each a stage/commit: study behaviour → write fresh on our primitives → unit + scenario test → add to `effects.md` + gif. Batching keeps each commit reviewable.

**Scope: ALL effects across MoonLight's `Nodes/Effects/E_*.h` files**, not a cherry-picked subset — the [breadth-parity gate](../../backlog/rename-to-moonlight.md) needs the full set. The source files (each an effect library, mapped to our origin sections + future per-library doc pages):
- **`E_MoonModules.h`** (MoonModules-authored, 3): **GameOfLife** (Conway, 2D/3D, rulesets/wrap/colour-aging/infinite-mode), **GEQ3D** ♫ (perspective 3D equalizer bars), **PaintBrush** ♫ (frequency-modulated animated lines, chaos/softness). — verified 2026-06-30 from source.
- **`E_MoonLight.h`** (MoonLight-original geometric set).
- **`E_WLED.h`** (WLED ports/enhancements).
- moving-head / DMX effect files → Stage 5.

The batch order below is by dependency/complexity (refine per batch), and **cuts ACROSS the source files** (an audio-reactive batch pulls GEQ3D+PaintBrush from E_MoonModules and the GEQ/Blurz family from E_WLED together) rather than migrating one file at a time — themed batches keep each commit coherent:

- **3a — simple 2D/3D non-audio** (the `E_MoonLight` / `E_WLED` geometric ones: lines, scrolling, lissajous, distortion, starfield…).
- **3b — palette-heavy** (now that palettes exist: the gradient/noise/plasma family not yet ported).
- **3c — particle/physics** (bouncing balls, popcorn, blackhole — build on the draw primitives + PRNG).
- **3d — audio-reactive (♫)** (GEQ, Blurz, Waverly, FreqMatrix… — depend on `AudioModule::latestFrame()`; a shared audio-read helper may be its own small sub-stage).
- **3e — text/scrolling** (scrolling text needs a font + glyph blitter — its own primitive).

### Stage 4 — Modifiers + layouts migration

The MoonLight modifiers (mirror/tile/kaleidoscope/pinwheel/transpose…) and layouts (panel/cube/ring/sphere/spiral/fixture variants) not yet ported. Modifiers are pure geometry (they fit our modifier model cleanly — and per architecture.md, geometry transforms belong in modifiers, not effects). Layouts are coordinate iterators. Smaller than the effect batches; can interleave with Stage 3.

### Stage 5 — Moving heads / DMX fixtures (last)

`E_MovingHeads` + fixture layouts + Art-Net moving-head control. Most specialised, fewest dependencies on the rest; deferred to last.

## Riskiest parts

1. **Palette + primitives are the load-bearing wall** — if their API or performance is wrong, every later effect inherits it. Stage 1 must get the hot-path shape right (measure tick cost; these run per-light). Worth over-investing in.
2. **Primitive implementation is ours** — the temptation under deadline is to copy a source's implementation, not just its recognisable name. The names follow the established FastLED convention (what a contributor recognises); the *code* is written fresh against our engine, behaviour pinned by tests, FastLED credited as prior art. Guard: independence-by-construction (own implementation + own architecture), not a renamed copy and not a traced one.
3. **Dimension-agnostic draw** — making `drawLine` etc. genuinely 1D→3D (not 2D with a z-loop bolted on) needs thought; get the abstraction right in Stage 1 or effects will work around it.
4. **Doc-model migration is a one-way door** — deleting 21 per-module `.md`s and rewriting the spec-check; do it as one coherent Stage-2 change, not piecemeal, so docs are never half-migrated.
5. **GoL "done right"** — we already got it wrong once; Stage 1 must pin the real algorithm against a reference (the actual rules + colouring), tested, so it's faithful this time.
6. **Scope discipline** — "migrate all of it" is dozens of modules. The batching is what keeps it from becoming one un-reviewable mega-diff; resist merging batches.

## Verification (per stage)

Every stage: desktop build (-Werror), `ctest` (new primitives/effects pinned), scenarios, spec-check, ESP32 build, KPI (watch the per-light hot-path cost as primitives land). Bench on hardware for the visual effects. Each stage saves its own `/plan` and commits independently.

## Open questions (settle at each stage, not now)

- Exact home + file split of the primitive library (one `Fx.h` vs several focused headers) — Stage 1 plan.
- Palette storage (flash tables vs computed gradients) + how the UI selects a palette (a `palette` control type?) — Stage 1 plan.
- The `registerType` → category-page mapping mechanics — Stage 2 plan.
- Per-batch effect list + order — each Stage-3 sub-plan.
