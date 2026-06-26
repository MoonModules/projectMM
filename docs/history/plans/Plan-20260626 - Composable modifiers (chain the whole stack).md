# Plan — Composable modifiers (chain the whole modifier stack)

## Context

Today a Layer applies **only its first enabled modifier**: `Layer::rebuildLUT()` finds the first enabled `Modifier` child and `break`s, and `Layer::loop()` ticks only that one. A second modifier on a Layer is dead weight. The product owner has always intended **modifier order = apply order** — a stack where each modifier reshapes the result of the one below (Region *then* Multiply-mirror *then* Rotate), the way it works in MoonLight (the product owner's prior engine, 3 years proven).

The current `ModifierBase` interface — `logicalDimensions()` + `mapToPhysical(coord) → [flat physical indices]` — is a **virtual→physical fan-out** model that does not compose: each stage emits flat indices, not coordinates, so stage N+1 can't consume stage N's output, and chaining would need a product-of-`maxMultiplier` fan-out ceiling (the exact 64-bit-overflow bug class that caused the multiplyZ black-screen).

**The fix is to adopt MoonLight's proven model, written fresh in projectMM style:** invert the map build to **physical→logical**, where each modifier is an **in-place coordinate fold**. Composition becomes a plain loop over enabled modifiers mutating one coordinate. Fan-out stops being a build-time concern (N physical lights folding onto one logical cell *is* the fan-out). The product-ceiling math, the per-light scratch buffer, `buildBoxToDriver`, `buildSparseIdentityLUT`, and `isNaturalOrder`'s shuffle role all disappear.

Three tiers of hook (MoonLight's structure, projectMM names):
- `modifyLogicalSize(Coord3D& size)` — static, build-time, run once in child order; folds the logical box (Multiply shrinks, Region crops, Mirror halves).
- `bool modifyLogical(Coord3D& pos, …)` — static, build-time, run per physical light in child order; folds a physical coord into logical space, returns `false` to reject (mask/out-of-region).
- `modifyLive(Coord3D& pos, …)` — **dynamic, per-frame**; the per-frame coordinate transform for smooth rotation/scroll, applied without a LUT rebuild.

**Pay-for-what-you-use is the load-bearing guarantee (product owner's explicit requirement):** a modifier with no `modifyLive` override imposes **zero per-frame cost** — the hot path runs at exactly today's speed. Per-frame cost exists only when a dynamic modifier is actually present, and that cost is inherent to dynamic motion (moving pixels every frame). MoonLight proves it's viable.

## Decisions locked with the product owner

- **Full three-tier model** (static size + static fold + dynamic per-frame), not a static-only first cut.
- **Reject signal = `bool modifyLogical()` return** (not a sentinel coord — avoids a later modifier's `% size` aliasing a sentinel back into range).
- **No `modifyXYZ` override ⇒ max hot-path speed.** The dynamic pass is gated behind a build-time "any live modifier?" flag; absent it, the render path is byte-identical to today.
- **Box may grow** (MoonLight's Rotate `expand` grows it). Do NOT hard-forbid growth; size the LUT from the *post-fold* logical box and clamp/guard defensively (robust-to-any-input), rather than asserting shrink-only.

## Architecture mapping (MoonLight idea → projectMM)

| MoonLight | projectMM |
|---|---|
| `Coord3D` | new `Coord3D` in `light_types.h`, our naming + rationale comment |
| "virtual" space | **logical** box (our existing word) |
| `modifySize` | `modifyLogicalSize(Coord3D& size)` |
| `modifyPosition` (mutate, reject via UINT16_MAX) | `bool modifyLogical(Coord3D& pos, const Coord3D& phys, const Coord3D& logical)` (mutate, reject via `false`) |
| `modifyXYZ` (per-frame at `setRGB(pos)`) | `modifyLive(Coord3D& pos, const Coord3D& logical)` — per-frame coordinate remap; seam described below |
| `PhysMap` table | existing `MappingLUT` CSR (**unchanged** — see build note) |
| `addLight(physicalPos)` gather | the physical→logical counting-sort build in `rebuildLUT()` |

## Key build-mechanics finding (verified)

The hot-path **read** (`BlendMap::blendMap` → `for li in logicalCount: forEachDestination(li) → dst[physIdx]=src[li]`) stays **byte-identical**. Only the cold-path **build** inverts.

But `MappingLUT::setMapping` requires **sequential, in-order, one-shot** writes (`offsets_[logicalIdx] = destinationCount_`, monotonic append). A physical→logical loop scatters onto *arbitrary, repeated* logical indices — a scatter, not an in-order gather. So `rebuildLUT()` builds the CSR with a **counting sort** (the textbook way to build CSR from scattered keys), entirely in `Layer` on the cold path, then replays it through `setMapping` in logical order. **No `MappingLUT` structural change.**

```
Pass A (count):  forEachCoord → fold each driver light to logIdx (or reject) → counts[logIdx]++
Prefix-sum:      counts → offsets
Pass B (scatter):forEachCoord (same fold) → scratchDests[cursor[logIdx]++] = driverIdx
Replay:          for logIdx 0..N-1: setMapping(logIdx, &scratchDests[offsets[logIdx]], counts[logIdx]); finalize()
```

`maxDest` passed to `MappingLUT::build` is now exactly `driverCount` (each physical light folds to ≤1 logical cell → contributes ≤1 destination total). The product-ceiling/overflow math is **deleted** — `destinationCount_ ≤ driverCount` is a hard invariant. `overwrites_` stays `true` for the fold build (each physical cell appears in exactly one logical entry's run; no within-layer additive accumulation can arise), simplifying `BlendMap`'s `overwrites()` handling for this path.

Two `forEachCoord` passes + a `logicalCount`-sized counts array is the build cost — cold path, bounded, comparable to today's `boxToDriver` + `physicals` scratch.

## The dynamic (per-frame) seam — projectMM placement

MoonLight applies `modifyLive` per pixel at write time because effects call `setRGB(pos)`. projectMM effects write a **flat logical buffer**, then `blendMap` scatters. So our seam is a **per-frame logical→logical remap** applied between the effect write and the scatter, only when a live modifier exists:

- At build time, compute `hasLive_` = any enabled modifier overrides `modifyLive`. Store on the Layer.
- `Layer::loop()`: after effects fill `buffer_` and static modifiers are already baked into `lut_`, if `hasLive_`, run one pass that, for each logical cell, folds its coordinate through the enabled `modifyLive` chain to a *source* logical coordinate and gathers (a coordinate remap over the logical buffer into a scratch buffer, then swap). If `!hasLive_`, skip entirely — **the buffer goes straight to the scatter, zero added cost** (the guarantee).
- Rotation is naturally an **inverse-sample gather** here (for each destination logical cell, sample its rotated source) — which is exactly how our current `RotateModifier` already reasons (`// map a destination light to its rotated SOURCE`), so no visual regression. The live pass is the right home for that inverse-sample logic that did NOT fit the forward static fold.

This resolves the Plan-agent's concern: dynamic modifiers do **not** force the forward-fold visual change, because they live in the per-frame gather seam, not the static build. Static modifiers fold forward (build); dynamic modifiers gather inverse (per-frame). Each in its natural direction.

## Files

### New / core types
- **`src/light/light_types.h`** — add `struct Coord3D { lengthType x,y,z; }` with `+ - % / ==` operators, a one-line rationale comment matching that file's house style. Used by the fold hooks.

### `src/light/modifiers/ModifierBase.h` — interface inversion
- Replace `logicalDimensions()` + `mapToPhysical()` + `maxMultiplier()` with:
  - `virtual void modifyLogicalSize(Coord3D& size) const {}` (default: no resize)
  - `virtual bool modifyLogical(Coord3D& pos, const Coord3D& phys, const Coord3D& logical) const { return true; }` (default: pass-through)
  - `virtual void modifyLive(Coord3D& pos, const Coord3D& logical) const {}` (default: no per-frame work; presence detected so absence = zero cost)
- Keep `dimensions()` (the 📏/🟦/🧊 chip) and `controlChangeTriggersBuildState` (already `true`).
- `phys`/`logical` passed in (not stashed on the modifier) so modifiers stay stateless and the two-pass build can't desync a cached size.

### `src/light/layers/Layer.h` — the heart
- `rebuildLUT()`: scan **all** enabled modifiers (not first-only). Run `modifyLogicalSize` chain → `width_/height_/depth_`. Then the counting-sort fold build above. Keep the dense-natural `setIdentity` memcpy shortcut (cheap `isNaturalOrder`-style check, retained only for that gate). n==0 and n==1 take the same paths with zero per-frame overhead.
- **Delete**: `buildBoxToDriver`, `buildSparseIdentityLUT`, the `maxMultiplier` scratch + `physicals[]`, the `maxDestWide`/`ceiling` product-clamp. (`isNaturalOrder` demoted to gating only the memcpy shortcut.)
- `loop()`: drop the `break;` after the first modifier — tick **all** enabled modifiers in child order. Coalesce dynamic rebuilds with a single dirty flag (avoid N rebuilds/frame when several modifiers tick). Add the `hasLive_`-gated per-frame remap pass.
- Defensive bounds-guard on the folded coord before flatten (a buggy modifier must not write past `counts[]`).

### `src/light/modifiers/*.h` — rewrite all 5 to the fold interface
- **MultiplyModifier** — `modifyLogicalSize`: divide per axis; `modifyLogical`: fold `pos` into the tile, mirror odd tiles (mirror is already a Multiply control — no separate Mirror class). ~10 lines, simpler than today.
- **RegionModifier** — `modifyLogicalSize`: `axisCount`; `modifyLogical`: subtract `axisStart`, return `inBox(pos, logical)` (reject outside region). Reuses the existing `axisStart`/`axisCount` helpers.
- **CheckerboardModifier** — `modifyLogicalSize`: identity; `modifyLogical`: parity test, return `false` to drop. ~5 lines.
- **RotateModifier** — `modifyLive` (inverse-sample gather, its current math moves here, unchanged visuals); `expand` grows the box via `modifyLogicalSize`. No static fold.
- **RandomMapModifier** — bijective permutation; can express as static `modifyLogical` (1:1) since it's a permutation; keep its `loop()` beat-reshuffle.

### Tests
- `unit_Coord3D` — operator coverage.
- Rewrite `unit_MultiplyModifier`, `unit_RegionModifier`, `unit_CheckerboardModifier`, `unit_RotateModifier`, `unit_RandomMapModifier` to the fold interface (they call the old virtuals directly today, so they change in lockstep with their modifier).
- `unit_Layer_sparse_mapping` — the green gate for the build (asserts CSR contents, not the interface). Update corner-fan-out + region cases to the fold values.
- New `unit_Layer_modifier_chain` — the payoff: Region∘Multiply-mirror composed CSR; A∘B ≠ B∘A; a disabled middle modifier is skipped.
- New scenario `scenario_modifier_chain` — reorder a 2-modifier stack live, assert the composite changes; perf capture at depth 2–3.

### Docs
- `ModifierBase.md` (or the modifier specs) — the three-hook contract, the reject convention, the pay-for-what-you-use guarantee.
- `architecture.md` § Layers and Layer / § Modifiers — update "first enabled modifier" → "modifier chain", document physical→logical build + the live seam.
- `docs/backlog/backlog-mixed.md` — delete the "Composed modifiers" item (shipped).
- `decisions.md` — the inversion lesson (forward-fold static, inverse-gather dynamic; CSR-via-counting-sort keeps the hot path untouched).
- `performance.md` — chain-depth + dynamic-modifier per-frame cost.

## Commit breakdown (~6)

1. `Coord3D` + new `ModifierBase` hooks **alongside** the old ones (no behavior change); `unit_Coord3D`. All green.
2. Implement new hooks on all 5 modifiers (old hooks still present, Layer still uses old); rewrite each modifier's unit test to the new hooks. Green.
3. New `rebuildLUT()` counting-sort fold build using the new hooks; delete `buildBoxToDriver`/`buildSparseIdentityLUT`/scratch/ceiling; keep `setIdentity` shortcut. `unit_Layer_sparse_mapping` is the gate. The big commit.
4. Delete old `mapToPhysical`/`logicalDimensions`/`maxMultiplier` from base + modifiers; remove old-hook test cases. Green.
5. Dynamic tier: `loop()` ticks all modifiers + dirty-flag coalesced rebuild + the `hasLive_`-gated per-frame remap seam; Rotate/RandomMap to the new model; rewrite their tests. Green.
6. `unit_Layer_modifier_chain` + `scenario_modifier_chain` + docs + backlog delete + decisions/perf. Green.

**Test-green honesty:** commits 1, 4, 6 are cleanly additive/green. Commits 2, 3, 5 rewrite tests in the same commit as the contract they pin (normal for an interface inversion) — "green" means "the new contract's tests pass in that commit," not "old tests still pass."

## Risks / watch-items
- **Build cost rises** (two `forEachCoord` passes + counts array, every rebuild even for n==1). Cold path, bounded — don't sell it as free.
- **Coalesced dynamic rebuild**: today a modifier's `loop()` re-enters `Layer::onBuildState()`; with N modifiers, gate to one rebuild/frame via a dirty flag (cleaner than today's re-entrancy).
- **`expand`-style growth**: size the LUT from the post-fold box; guard the flatten against `nrOfLightsType` overflow on a grown box.
- **Per-frame seam scratch buffer**: the live remap needs a logical-sized scratch (allocated when `hasLive_`, off the hot path). Confirm it degrades gracefully on OOM (fall back to static LUT, no live motion, status warning).

## Verification
- `ctest` + `uv run scripts/scenario/run_scenario.py` green at each commit boundary.
- New `unit_Layer_modifier_chain` proves Region∘Multiply composes (A∘B ≠ B∘A, disabled-middle skipped).
- Live on the bench: build a Region + Multiply(mirror) stack on the S3, confirm the carved-then-mirrored result in the preview; add a Rotate on top, confirm smooth (not stepped) dynamic rotation with the static chain underneath.
- Perf: capture single-layer (no modifier) tick on S3/classic/P4 — must match today (max-speed guarantee); capture a static 2-chain and a dynamic (Rotate) chain to quantify the live-seam cost.
