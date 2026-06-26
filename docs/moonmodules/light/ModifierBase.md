# ModifierBase

Light-domain MoonModule subclass for modifiers. A modifier is a **coordinate transform** that reshapes how a Layer's effect output maps onto the physical lights. Multiple modifiers on one Layer **compose**: they apply in child order, each reshaping the result of the one below (Region *then* Multiply-mirror *then* Rotate).

## The fold contract

A Layer builds its mapping by walking the **physical** lights and folding each through every enabled modifier in order — the composition `M₁∘M₂∘…∘Mₙ` collapsed into one mapping, so the per-frame render stays a single lookup. Three hooks, each a no-op by default so a modifier implements only what it needs:

- **`modifyLogicalSize(Coord3D& size)`** — static, build-time, once per rebuild in child order. Folds the logical box (Multiply divides it, Region crops it, a mask leaves it). The running `size` starts at the physical box; each modifier reshapes it.
- **`bool modifyLogical(Coord3D& pos, const Coord3D& phys, const Coord3D& logical)`** — static, build-time, per physical light in child order. Folds a physical coordinate into this stage's logical space in place. Returns **`false` to reject** — the physical light has no logical source (a mask drops it, a region light falls outside the crop). A bool, not a sentinel coord, so a later modifier's `% size` can't alias a sentinel back into range.
- **`modifyLive(Coord3D& pos, const Coord3D& logical)`** — dynamic, per-frame at render time. Remaps a coordinate without rebuilding the mapping (smooth rotation/scroll). The Layer runs this pass **only** when some enabled modifier reports `hasModifyLive()`, so a static-only chain pays nothing per frame — the render path stays at full speed (pay-for-what-you-use). It is a **backward** map: for each destination cell it returns the source cell to gather, so no destination is left torn.

A beat-driven modifier (RandomMap reshuffles on a timer) sets a flag in `loop()`; the Layer polls `consumeNeedsRebuild()` across its modifiers and rebuilds the mapping **once** if any asks, coalescing several dynamic modifiers into a single rebuild.

`dimensions()` (the 📏/🟦/🧊 chip) advertises which axes the modifier can transform.

## Fan-out is free

Because the build walks physical lights, fan-out (one logical cell driving N physical lights — a Multiply kaleidoscope) emerges naturally: N physical lights fold onto the same logical cell. There is no build-time fan-out list and no product-of-multipliers ceiling — each physical light contributes at most one destination, so the mapping can never overflow.

## Affine modifiers and the matrix reference

Most modifiers are **non-affine** (a mask is a predicate, a tile is modulo) and express their fold directly. [RotateModifier](modifiers/RotateModifier.md) is the exception and the codebase's **transform-matrix reference**: rotation is the canonical affine transform, written as an explicit integer 2×2 rotation matrix in `modifyLive`. A future affine "Transform" modifier (translate+scale+rotate+shear in one) would compose its matrix the same way and apply it through the same hook — the fold interface hosts a matrix-backed modifier with no change.

## Prior art

The mapping bake is the textbook image-warping pattern (precompute a coordinate transform into a spatial LUT; build it by backward mapping so no output pixel is unfilled — [Forward and Backward Mapping for Computer Vision](https://towardsdatascience.com/forward-and-backward-mapping-for-computer-vision-833436e2472/)). Collapsing a **chain** of discrete pixel folds into one index table — instead of giving each node its own frame buffer as a PC node graph (TouchDesigner, shader graphs) would — is the MCU-memory synthesis credited to **MoonLight** ([M_MoonLight.h](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Modifiers/M_MoonLight.h), [VirtualLayer.cpp](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Layers/VirtualLayer.cpp)): `modifySize` / `modifyPosition` / `modifyXYZ` map to our `modifyLogicalSize` / `modifyLogical` / `modifyLive`, written fresh against our `MappingLUT`.

## Source

[ModifierBase.h](../../../src/light/modifiers/ModifierBase.h) — the hook contract. The Layer-side fold build (physical→logical counting-sort, the live pass) lives in [Layer.h](../../../src/light/layers/Layer.h).
