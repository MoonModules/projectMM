# Layouts

Every layout, one compact row each. A layout maps light indices to physical `(x, y, z)` positions — it defines the *shape* an [effect](../effects/effects.md) draws onto and a [driver](../drivers/) sends out. The [Layouts](../Layouts.md) container holds one or more layout children and composes them into one coordinate space; a [Layer](../Layer.md) renders over that combined space.

Columns: **Name** (with its `tags()` emoji — see the [tag emoji legend](../../../architecture.md#tag-emoji-legend)), **Preview**, **Description**, **Controls**, **Tests**. The per-library file split is future work — see the [folder-structure decision](../../../backlog/folder-structure-proposal.md). Preview gifs land with the [MoonLight migration](../../../history/plans/Plan-20260630%20-%20MoonLight%20migration%20(multi-stage).md) (MoonLight documents matching layout nodes — Panel ≈ Grid, Wheel — at <https://moonmodules.org/MoonLight/moonlight/layouts/>; Sphere is projectMM-native).

## projectMM-native layouts

| Name | Preview | Description | Controls | Tests |
|---|---|---|---|---|
| <a id="grid"></a>**Grid** | — | A dense 3D grid, row-major (x fastest, then y, then z); every position maps to a light. `serpentine` boustrophedon-wires alternate rows. | `width`, `height`, `depth`, `serpentine` | [tests](../../../tests/unit-tests.md#gridlayout) |
| <a id="sphere"></a>**Sphere** | — | Lights on the surface of a hollow sphere — a one-light-thick shell inside a `(2·radius+1)³` box, no interior lights. | `radius` | [tests](../../../tests/unit-tests.md#spherelayout) |
| <a id="wheel"></a>**Wheel** | — | A bicycle-wheel: `spokes` straight rows radiate from a centre hub, each carrying `ledsPerSpoke` LEDs spaced one unit apart outward. | `spokes`, `ledsPerSpoke` | [tests](../../../tests/unit-tests.md#wheellayout) |

The [Layouts](../Layouts.md) container itself takes no controls — see its page for coordinate iteration, reordering, and rebuild propagation.

## Source

- [GridLayout.h](../../../../src/light/layouts/GridLayout.h)
- [SphereLayout.h](../../../../src/light/layouts/SphereLayout.h)
- [WheelLayout.h](../../../../src/light/layouts/WheelLayout.h)
