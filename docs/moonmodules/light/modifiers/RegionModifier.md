# Region Modifier

Static modifier. Carves the layer down to a sub-rectangle of the physical bounding box: the effect renders only inside the region, everything outside is dark. The region is given as **percentages of the physical extent on each axis**, so it survives a physical resize — a `0..50` region stays the left half whether the panel is 64 or 128 wide. Default `0..100` on every axis is the full box (an identity carve).

Region and Multiply are independent and **compose**: a layer can occupy a region *and* be tiled/mirrored within it (Region then Multiply), since a Layer folds its whole enabled modifier chain in order (see [ModifierBase](../ModifierBase.md)).

## Controls

- `startX` / `startY` / `startZ` (Int16, default 0) — region start, as a percentage of physical width / height / depth.
- `endX` / `endY` / `endZ` (Int16, default 100) — region end, as a percentage of physical width / height / depth.

`Int16`, not a 0–100 slider: the UI renders an unbounded int16 as a **−100..200 percentage slider** so the window can slide **off-screen** (negative start / >100 end). Values round-trip through `/api/state`, `/api/types`, and persistence.

## Region math

Per axis, **half-open** `[startPixel, endPixel)`, **un-clamped** to the box:

- `startPixel = floor(start% / 100 · extent)` — may be negative.
- `endPixel = ceil(end% / 100 · extent)`, **exclusive** — may exceed `extent`.
- window size = `endPixel − startPixel` (floored to ≥ 1 on a non-empty axis).

Half-open makes abutting windows **tile exactly**: a `0..50` and a `50..100` layer split a 128-wide axis into pixels `0..63` and `64..127` — no overlap, no gap. `start` floors and `end` ceils so a small panel never rounds to an empty window. Default `0..100` on a `W`-wide axis → the full width.

### Off-screen windows (move, don't rescale)

The window's **logical size is the full `start..end` span**, so the effect always renders at a fixed scale — moving `start` and `end` together slides the window without resizing it (like dragging an OS window). A physical light outside the window is dropped; window cells with no physical light under them (the off-screen part) stay dark. A window slid **entirely** off the box (e.g. `start=-100, end=0`) maps no lights — the layer goes dark, the way you move an effect completely out of view. Because the span is fixed, sweeping `start`/`end` translates an effect across and off the panel without distorting it.

## Effect on the pipeline

- **Logical box = the region size** — `modifyLogicalSize` shrinks the box to the carved rectangle, so the Layer's render buffer (and the status line `w×h×d`) shrinks to the region. The effect only ever renders the region.
- **Fold + reject** — `modifyLogical` folds a physical light into region-local space (subtract the start offset) and returns `false` for any physical light outside the region, so everything beyond the region stays dark. A 1:1 fold, never fans out.
- **Fast path**: the cheapest carve is *no modifier at all* — then `Layer::rebuildLUT` keeps its identity-memcpy / sparse fast path with zero carving cost. The default is to not add a RegionModifier; a full-region `0..100` one is correct but not the absolute cheapest, so full-coverage layers simply omit it.

## Cross-domain wiring

Region is a normal `ModifierBase` — carving is its `modifyLogicalSize` + `modifyLogical` fold, composed into the chain like any modifier. See [ModifierBase](../ModifierBase.md).

## Tests

[Unit tests: RegionModifier](../../../tests/unit-tests.md#regionmodifier) — the region math (full box, exact half, abutting-tile, small-panel rounding, ≥1-pixel floor, off-screen / fully-off / wider-than-box windows, degenerate axes) and the coordinate offset mapping. [Unit tests: Layer](../../../tests/unit-tests.md#layer) adds the integration case: a RegionModifier shrinks the Layer's logical box to the region and the LUT maps only region cells.

## Prior art

The crop / region node of any compositor (After Effects' crop, a shader scissor rect): restrict rendering to a rectangle, the rest is transparent. MoonLight has no single "region" modifier — its layers map through the same coordinate-transform mechanism, which is the lineage for expressing this as a modifier rather than a Layer control.

## Source

[RegionModifier.h](../../../../src/light/modifiers/RegionModifier.h)
