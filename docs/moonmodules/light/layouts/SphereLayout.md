# Sphere Layout

Arranges lights on the **surface of a hollow sphere** — a one-light-thick shell, no interior lights. Lattice layout: every light sits at an integer `(x, y, z)` inside a `(2·radius+1)³` bounding box centred at `(radius, radius, radius)`.

## Controls

- `radius` (default 4, range 1–64) — surface radius in light-units. A lattice point is on the shell when its distance from the centre rounds to `radius` (it falls in the half-open band `[radius−0.5, radius+0.5)`). `radius = 1` is the smallest hollow sphere — 18 lights: the 6 axis-neighbours (d²=1) plus the 12 edge-neighbours (d²=2) of the centre, all of which round to distance 1.

## Light count and mapping

Light count is derived from `radius` (not set directly) — the lattice points landing in the shell band, growing roughly with surface area (`~4π·radius²`). The iterator and `lightCount()` share one shell predicate, so the count always matches the emitted points. Distances compare in squared integer space (no `sqrt`, no per-light float), so the shell is exact and deterministic across platforms.

A sphere is **not** 1:1 unshuffled — the shell points are sparse within the bounding box, so it supplies explicit coordinates via `forEachCoord` like every non-grid layout; Layer/Drivers wiring treats it identically to any other `LayoutBase`.

## Tests

[Unit tests: SphereLayout](../../../tests/unit-tests.md#spherelayout) — shell-only (no interior/centre point), symmetry, count matches the iterator, radius-1 base case. Add / replace / remove / multiple layouts are covered in [Layouts](../../../tests/unit-tests.md#layouts) and the layout-mutation scenario.

## Prior art

### MoonLight / projectMM v1–v2 — layout nodes ([MoonLight L_MoonLight.h](https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h))

Prior projects expose layouts that call an `addLight(x, y, z)` per position; SphereLayout follows the same "a layout enumerates its light coordinates" shape, computing the shell analytically rather than from a stored list.

## Source

[SphereLayout.h](../../../../src/light/layouts/SphereLayout.h)
