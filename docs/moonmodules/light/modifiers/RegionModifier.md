# Region Modifier

Static modifier. Carves the layer down to a sub-rectangle of the physical bounding box: the effect renders only inside the region, everything outside is dark. The region is given as **percentages of the physical extent on each axis**, so it survives a physical resize — a `0..50` region stays the left half whether the panel is 64 or 128 wide. Default `0..100` on every axis is the full box (an identity carve).

A Layer applies only its **first enabled modifier**, so today a Layer uses *either* Region *or* another modifier (Multiply, …) at a time. Region and Multiply are independent, so stacking them (occupy a region *and* tile/mirror within it — Region then Multiply) is planned via [modifier chaining](../../../backlog/README.md).

## Controls

- `startX` / `startY` / `startZ` (Int16, default 0) — region start, as a percentage of physical width / height / depth.
- `endX` / `endY` / `endZ` (Int16, default 100) — region end, as a percentage of physical width / height / depth.

`Int16` (not a 0–100 slider) so negative and >100 values round-trip through `/api/state`, `/api/types`, and persistence; the carve math clamps them into the box.

## Region math

Per axis, **half-open** `[startPixel, endPixel)`:

- `startPixel = floor(start% / 100 · extent)`, clamped to `[0, extent-1]`.
- `endPixel = ceil(end% / 100 · extent)`, **exclusive**, clamped to `[startPixel+1, extent]`.
- region size = `endPixel − startPixel` (always ≥ 1).

Half-open is what makes abutting regions **tile exactly**: a `0..50` and a `50..100` layer split a 128-wide axis into pixels `0..63` and `64..127` — no overlap, no gap. `start` floors and `end` ceils so a small panel never rounds to an empty region (`start 33 / end 66` on a 4-wide axis → `floor(1.32)=1` .. `ceil(2.64)=3` → pixels 1, 2). Default `end 100` on a `W`-wide axis → `ceil(W)=W` → the full width.

## Effect on the pipeline

- **Logical dimensions = the region size** — `logicalDimensions()` reports the carved rectangle, so the Layer's render buffer (and the Layer status line `w×h×d`) shrinks to the region. The effect only ever renders the region; the rest of the layer has no logical source and stays dark. This is the same "the box is smaller than the physical box" mechanism a Mirror modifier uses.
- **1:1 mapping with a start offset** — `mapToPhysical()` translates a region-local cell `(lx,ly,lz)` to the box cell `(lx+startPixelX, ly+startPixelY, lz+startPixelZ)`, a single destination. Because the logical box is already the region size, every region cell is in-bounds; no per-cell drop is needed. `maxMultiplier()` is 1 — it never fans out.
- **Fast path**: the cheapest carve is *no modifier at all* — then `Layer::rebuildLUT` keeps its identity-memcpy / sparse fast path with zero carving cost. The default is to not add a RegionModifier; a full-region `0..100` one is correct but not the absolute cheapest, so full-coverage layers simply omit it.

## Cross-domain wiring

A Layer applies its first enabled modifier during `rebuildLUT`. Region is a normal `ModifierBase` (no contract change) — it expresses carving entirely through the existing `logicalDimensions()` + `mapToPhysical()` virtuals, the same two the LUT builder already calls. See [architecture.md § Modifiers](../../../architecture.md#layers-and-layer).

## Tests

[Unit tests: RegionModifier](../../../tests/unit-tests.md#regionmodifier) — the region math (full box, exact half, abutting-tile, small-panel rounding, ≥1-pixel floor, out-of-range clamp, degenerate axes) and the coordinate offset mapping. [Unit tests: Layer](../../../tests/unit-tests.md#layer) adds the integration case: a RegionModifier shrinks the Layer's logical box to the region and the LUT maps only region cells.

## Prior art

The crop / region node of any compositor (After Effects' crop, a shader scissor rect): restrict rendering to a rectangle, the rest is transparent. MoonLight has no single "region" modifier — its layers map through the same coordinate-transform mechanism, which is the lineage for expressing this as a modifier rather than a Layer control.

## Source

[RegionModifier.h](../../../../src/light/modifiers/RegionModifier.h)
