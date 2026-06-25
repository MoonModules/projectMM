# Plan — RegionModifier (start/end region carving)

**Date:** 2026-06-25
**Branch:** next-iteration

## Goal

Make per-Layer start/end region carving functional: a Layer can render its effect into only a sub-region of the physical bounding box (e.g. start `0,0,0` → end `50,50,0` writes only the top-left quarter; the rest stays dark). Coordinates are **percentages** of the physical width/height/depth, not absolute pixels.

## Decision: implement as a modifier, not as Layer::rebuildLUT logic

The six `startX/Y/Z`, `endX/Y/Z` controls currently live on `Layer` as **dead state** (persisted, surfaced in the UI, but `rebuildLUT()` ignores them). The original spec planned to wire them into `rebuildLUT`. The better solution, chosen with the product owner: a **`RegionModifier`** — a static modifier carrying the six percentage controls.

Why the modifier path wins (per § Principles):

- **Zero core change.** `Layer::rebuildLUT()` already runs the first enabled modifier through `logicalDimensions()` + `mapToPhysical()`. Carving is exactly those two operations, so it rides the existing path — no new branch in the most complex core file.
- **Fast path is free.** The product owner's hard constraint: full region (0,0,0→100,100,100) must cost nothing. With a modifier, *full region = no modifier present* → the existing identity-memcpy / dense fast path runs byte-identical. There is no carving code in the hot path or the no-modifier build path at all.
- **Minimalism / subtraction.** Net: **delete** six dead controls + their wiring from `Layer.h`; **add** one ~60-line modifier mirroring `CheckerboardModifier`. Reuses a recognisable shape (a crop/region node in any compositor).
- **Composes with modifier chaining** (backlog item): carving-as-a-modifier *is* the composition story — once chaining lands, Region + Mirror + Rotate stack.

Behaviour (product owner confirmed): **drop outside.** Lights outside the region get no physical destination (`outCount = 0`, the same mask path `CheckerboardModifier` uses). The logical box shrinks to the region size, so the effect only renders the carved region; the rest of the layer is dark. `maxMultiplier() == 1` (1:1 inside, 1:0 outside — never fans out).

## Rounding rule (from the existing spec, Layer.md § start/end)

> **Shipped differently:** during implementation the product owner chose a **half-open `[start, end)`** interval (end exclusive) so abutting regions tile exactly, instead of the inclusive rule sketched below. The authoritative rule is in `RegionModifier.h` / `RegionModifier.md`; the inclusive version here is the original intent, kept as the design record.

Per axis, percentage → pixel:
- `startPixel = floor(start% / 100 · W)`
- `endPixel   = ceil (end%  / 100 · W)`, treated as an **inclusive** last pixel
- region width on that axis = `endPixel − startPixel + 1`

Clamp `startPixel` to `[0, W−1]` and `endPixel` to `[startPixel, W−1]` so a region is always ≥1 pixel and never runs off the box. (Negative / >100 percentages are legal on the wire — they clamp to the box here; a future drag-off-screen use reads them raw, but carving clamps.)

Spec example: start=33, end=66 on a 4-wide axis → start `floor(1.32)=1`, end `ceil(2.64)=3` → pixels 1..3 inclusive (width 3). Default start=0,end=100 → 0..(W−1) = full width (identity).

## Files

### New: `src/light/modifiers/RegionModifier.h` (~60 lines)

Mirror `CheckerboardModifier.h`:
- Controls: `startX, startY, startZ` (default 0), `endX, endY, endZ` (default 100), all `addInt16` (negative/>100 legal on the wire, clamped in the math).
- `maxMultiplier() == 1`.
- `dimensions()` — advisory chip; D3 (it can carve any axis).
- A private `axisRange(pct_start, pct_end, physExtent) -> {startPixel, count}` helper applying the rounding rule + clamp, used by both `logicalDimensions` and `mapToPhysical` so the two can't drift.
- `logicalDimensions()`: `logW/H/D = count` per axis (the region size).
- `mapToPhysical(lx,ly,lz, physW,physH,physD, ...)`: translate region-local `(lx,ly,lz)` to box coordinate `(lx+startPixelX, ly+startPixelY, lz+startPixelZ)`, emit the single box index, `outCount = 1`. (Always in-bounds because `logicalDimensions` already sized the logical box to the clamped region — no per-cell drop needed; the "drop outside" is achieved by the logical box being smaller, exactly like a Mirror shrinks it.)

### Edit: `src/light/layers/Layer.h`

- **Remove** `startX/Y/Z`, `endX/Y/Z` fields, their `addInt16` calls in `onBuildControls`, and the long start/end comment block (lines ~21-48). Keep `blendMode`/`opacity` (unrelated, live).
- No other change — `rebuildLUT` already handles modifiers.

### Edit: module factory / registration

- Register `RegionModifier` in the factory next to the other modifiers (`CheckerboardModifier`, `MultiplyModifier`, …) so it's addable in the UI and round-trips through persistence/types.

### Docs

- `docs/moonmodules/light/RegionModifier.md` — new spec page (controls, percent semantics, rounding rule, drop-outside, prior art: a crop/region node; MoonLight has no direct equivalent but its modifier model is the lineage). Mention every control name (spec-check requirement).
- `docs/moonmodules/light/Layer.md` — delete the `## start/end controls` section; replace with a one-line pointer: region carving is a modifier ([RegionModifier](RegionModifier.md)), not a Layer control. Update the § Status paragraph (it currently says "start/end region carving" reshapes the logical box — still true, but now via the modifier).
- `docs/architecture.md` § Effects (line ~362) — the Layer determines buffer dims from "the Layouts, its own start/end percentages, and its modifiers"; change to "the Layouts and its modifiers (region carving among them)".
- `docs/architecture.md` § Layers and Layer — note RegionModifier as a built-in carve modifier if the modifier list is enumerated.
- Remove the now-shipped backlog reference to start/end region carving (Layer.md pointed at backlog/README; if backlog has an item, delete it per *Mandatory subtraction*).

### Tests

- `test/unit/light/unit_RegionModifier.cpp`:
  - `logicalDimensions`: 0/100 → full box (identity); 0/50 on 128 → 64; the spec's 33/66-on-4 → start 1, count 3; clamp of end=100 to W−1 (no off-box overflow); clamp of a >100 / negative percentage.
  - `mapToPhysical`: a region-local (0,0,0) maps to the box index at the start offset; the last region cell maps to the correct box index; never emits an out-of-box index.
  - `axisRange` edge cases: W=1 (degenerate axis stays 1), W=0 (no crash).
- `test/unit/light/unit_Layers_container.cpp` or a Layer carving case: add a Layer with a RegionModifier, build, assert the logical box (`width_×height_×depth_`) equals the region and the driver buffer only carries the region's lights (rest dark). Reuse the existing CaptureDriver fixture.
- Scenario: extend an existing pipeline scenario (or a small new `scenario_RegionModifier`) — add a RegionModifier at 0,0→50,50, assert the tick passes its gate and the buffer is non-zero only in the carved region. (Construct-mode can't set_control post-scheduler, so use the modifier's default region or a fixture that builds it with the region preset — mirror how `scenario_MultiplyModifier_pipeline` handles defaults.)

## Fast-path guarantee (the product owner's constraint)

No RegionModifier on a Layer → `rebuildLUT` takes the no-modifier branch → identity memcpy (dense natural-order) or the sparse box→driver LUT, exactly as today. The carving code is never reached. Even an *added* full-region (0/100) RegionModifier produces an identity logical box and a 1:1 offset-0 map — correct, just not the absolute cheapest; the cheap path is "don't add it," which is the default.

## Out of scope

- Modifier **chaining** (Region + another modifier composed) — separate backlog item. Today only the first enabled modifier applies, so a Layer uses *either* Region *or* another modifier until chaining lands.
- Negative / >100 "drag off-screen" semantics beyond clamping — the wire type allows them; carving clamps to the box. Revisit if a real drag-off-screen feature needs the raw values.
