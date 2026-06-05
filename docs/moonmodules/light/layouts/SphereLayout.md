# Sphere Layout

Arranges lights on the **surface of a hollow sphere** — a one-light-thick shell, no interior lights. Lattice layout: every light sits at an integer `(x, y, z)` inside a `(2·radius+1)³` bounding box centred at `(radius, radius, radius)`.

## Controls

- `radius` (default 4, range 1–64) — surface radius in light-units. A lattice point is on the shell when its distance from the centre rounds to `radius` (it falls in the half-open band `[radius−0.5, radius+0.5)`). `radius = 1` is the smallest hollow sphere — the 6 axis-neighbours of the centre.

## Coordinate Iterator

Yields `(physicalIndex, x, y, z)` for each shell light, scanning the bounding box in z-then-y-then-x order. The physical index is sequential over emitted shell points only (gaps in the lattice aren't indexed). `lightCount()` and the iterator share one shell predicate, so the count always matches the emitted points — no off-by-one between allocation and fill.

## Light count

Derived from `radius`, not set directly: it's the number of lattice points landing in the shell band, counted with the same predicate the iterator uses. Grows roughly with the sphere's surface area (`~4π·radius²`) — e.g. `radius = 4` yields a few dozen lights. There is no minimum-light control; pick the `radius` that gives the surface density you need.

## Mapping

A sphere is **not** a 1:1 unshuffled layout — the shell points are sparse within the bounding box, so the physical indices don't map linearly to box coordinates. It supplies explicit coordinates per light via `forEachCoord`, the same contract every non-grid layout uses; the Layer/Drivers wiring treats it identically to any other `LayoutBase`.

## Edge cases

- `radius = 0`: prevented by min = 1 on the control.
- Distances are compared in squared integer space (no `sqrt`, no float per light), so the shell is exact and deterministic across platforms.

## Tests

[Unit tests: SphereLayout](../../../tests/unit-tests.md#spherelayout) — shell-only (no interior/centre point), symmetry, count matches the iterator, radius-1 base case. Add / replace / remove / multiple layouts are covered in [Layouts](../../../tests/unit-tests.md#layouts) and the layout-mutation scenario.

## Prior art

### MoonLight / projectMM v1–v2 — layout nodes ([MoonLight L_MoonLight.h](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h))

Prior projects expose layouts that call an `addLight(x, y, z)` per position; SphereLayout follows the same "a layout enumerates its light coordinates" shape, computing the shell analytically rather than from a stored list.
