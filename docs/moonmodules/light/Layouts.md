# Layouts

![Layouts controls](../../assets/screenshots/Layouts.png)

Top-level container for one or more layouts. Shared by every layer in the Layers container — defines the physical light topology of the installation.

> **Naming convention.** Capital `Layouts` is the container class; lowercase "layout"/"layouts" is the English singular/plural for individual `LayoutBase` children. Capitalisation disambiguates "the Layouts container" from "two layouts stacked". Same rule for `Layers`/layer and `Drivers`/driver.

Coordinate iteration is owned by the container, not the layer: `Layouts::forEachCoord` walks every child layout's coordinates, offsetting physical indices so multiple layouts (e.g. 16 strips making one panel) don't overlap. A layer *uses* those coordinates to build its LUT. `totalLightCount()` — the sum across enabled children — sizes both the layer buffer and the driver output buffer. Children inherit `LayoutBase` and implement its virtual interface directly (no wrapper).

## Disabling a layout

Disabling a layout child (the `enabled` toggle in the UI) removes its lights from the LUT entirely. Indices of any layouts after it shift down to close the gap: with two grids of 4 and 2 lights, disabling the first leaves the second at indices 0–1, and `totalLightCount` drops from 6 to 2. A `Scheduler::buildState()` fires from the HTTP handler so the LUT, layer buffer, and driver output buffer reallocate.

Side effect: ArtNet universe assignments shift with the indices. To keep driver-to-fixture mapping stable across enable changes, disable the driver instead of the layout.

## Status

The container's status line (the `MoonModule` status slot, rendered by the UI) shows the **physical** setup it describes: `"<N> lights · <W>×<H>×<D>"` — the total light count summed across all enabled layout children (this is the size of the driver output buffer) and the physical bounding box (the extent of all light coordinates, the size of the dense render buffer). For a dense grid the count equals the box volume; for a sparse layout (e.g. a sphere shell) the count is smaller than the box — the gap is the at-a-glance signal that the layout is sparse. An empty setup (no lights) reports `Warning` severity. Recomputed on every rebuild (`onBuildState`), not per tick.

## Reordering

Layout children are reordered by drag-and-drop in the UI (`POST /api/modules/<name>/move` with `{"to": <index>}`). **Insert semantics, not swap:** the dragged layout takes the drop target's slot and the others shift to fill — the standard reorderable-list behaviour (Finder, Trello, SortableJS). Dropping onto a row (rather than a between-rows gap) means the landing is the target's absolute index, so dragging down lands after the target and dragging up lands before it. Order matters: it sets the physical index range each layout occupies, which drives ArtNet universe assignment. Same `move` op and semantics apply to every container (effects/modifiers under a Layer, drivers under Drivers).

## Rebuild propagation

A layout control change propagates to every layer (LUT rebuild) and to the Drivers container (output-buffer reallocation) via the mechanism in [architecture.md § Rebuild propagation](../../architecture.md#rebuild-propagation).

## Source

[Layouts.h](../../../src/light/layouts/Layouts.h)
