# Layouts

Every layout, one block each: what it does and what each control means — together. A layout maps light indices to physical `(x, y, z)` positions — it defines the *shape* an [effect](../effects/effects.md) draws onto and a [driver](../drivers/) sends out. The [Layouts](../Layouts.md) container holds one or more layout children and composes them into one coordinate space; a [Layer](../Layer.md) renders over that combined space. The per-library file split is future work — see the [folder-structure decision](../../../backlog/folder-structure-proposal.md). Preview gifs land with the [MoonLight migration](../../../history/plans/Plan-20260630%20-%20MoonLight%20migration%20(multi-stage).md) (MoonLight documents matching layout nodes — Panel ≈ Grid, Wheel — at <https://moonmodules.org/MoonLight/moonlight/layouts/>; Sphere is projectMM-native).

## projectMM-native layouts

<a id="grid"></a>

### Grid

A dense 3D grid, row-major (x fastest, then y, then z); every position maps to a light.

- `width` / `height` / `depth` — grid extent on each axis in lights (1–512).
- `serpentine` — boustrophedon-wire alternate rows (every other row runs in reverse, matching a snaked strip).

[Tests](../../../tests/unit-tests.md#gridlayout)

<a id="sphere"></a>

### Sphere

Lights on the surface of a hollow sphere — a one-light-thick shell inside a `(2·radius+1)³` box, no interior lights.

- `radius` — surface radius in light-units (1–64); the shell is every cell whose distance from the centre rounds to `radius`.

[Tests](../../../tests/unit-tests.md#spherelayout)

<a id="wheel"></a>

### Wheel

A bicycle-wheel: `spokes` straight rows radiate from a centre hub, each carrying `ledsPerSpoke` LEDs spaced one unit apart outward.

- `spokes` — number of spokes radiating from the hub (2–64).
- `ledsPerSpoke` — LEDs along each spoke, spaced one unit apart from the centre outward.

[Tests](../../../tests/unit-tests.md#wheellayout)

The [Layouts](../Layouts.md) container itself takes no controls — see its page for coordinate iteration, reordering, and rebuild propagation.

## Source

- [GridLayout.h](../../../../src/light/layouts/GridLayout.h)
- [SphereLayout.h](../../../../src/light/layouts/SphereLayout.h)
- [WheelLayout.h](../../../../src/light/layouts/WheelLayout.h)
