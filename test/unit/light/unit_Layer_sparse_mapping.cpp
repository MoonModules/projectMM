// @module Layer

#include "doctest.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/SphereLayout.h"
#include "light/modifiers/MirrorModifier.h"

// The driver/output buffer must hold ONLY the real lights, never the dense
// bounding box. A sphere defines a 9x9x9 (=729) render grid but only 210 shell
// lights; the MappingLUT extracts those 210 into the driver buffer (what ArtNet
// and the preview consume). These tests pin:
//  - sparse layout (sphere) builds a LUT whose destinations are driver indices
//    in [0, lightCount) — never a box index >= lightCount (the latent overflow);
//  - dense grid stays on the identity fast path (no LUT) — unchanged;
//  - a sphere + Mirror modifier still maps into driver-index space.
// The Layer buffer (where effects render) stays the dense box in all cases.

namespace {

// Build a Layer over a single layout, run onBuildState, return it wired.
struct LayerRig {
    mm::Layouts group;
    mm::Layer layer;

    explicit LayerRig(mm::LayoutBase* layout, uint8_t cpl = 3) {
        group.addChild(layout);
        layer.setLayouts(&group);
        layer.setChannelsPerLight(cpl);
        layer.onBuildControls();
        layer.onBuildState();
    }
};

} // namespace

// Dense grid: every box cell is a light, so no LUT — the identity/memcpy fast
// path is preserved exactly (the grid short-circuit).
TEST_CASE("Layer: dense grid stays on the identity path (no LUT)") {
    mm::GridLayout g;
    g.width = 8; g.height = 8; g.depth = 1;   // 64 lights, box == sparse
    LayerRig rig(&g);

    CHECK_FALSE(rig.layer.lut().hasLUT());          // identity, no table
    CHECK(rig.layer.physicalLightCount() == 64);
    CHECK(rig.layer.buffer().count() == 64);        // render buffer == box == lights
}

// Sparse sphere: a LUT is built; its destinations are driver indices in
// [0, lightCount), and the render buffer stays the dense bounding box.
TEST_CASE("Layer: sparse sphere builds a box->driver LUT, no out-of-range index") {
    mm::SphereLayout s;
    s.radius = 4;                                   // 210 shell lights in a 9^3 box
    const mm::nrOfLightsType N = s.lightCount();
    CHECK(N == 210);
    LayerRig rig(&s);

    CHECK(rig.layer.lut().hasLUT());                // sparse → a real LUT exists
    CHECK(rig.layer.physicalLightCount() == N);     // driver buffer = real lights, not box
    CHECK(rig.layer.buffer().count() == 9 * 9 * 9); // render buffer = dense box (729)

    // Every LUT destination is a driver index < N — the fix for the latent
    // overflow where box indices (0..728) were written into an N-sized buffer.
    const mm::MappingLUT& lut = rig.layer.lut();
    mm::nrOfLightsType maxDest = 0;
    mm::nrOfLightsType totalDests = 0;
    for (mm::nrOfLightsType li = 0; li < lut.logicalCount(); li++) {
        lut.forEachDestination(li, [&](mm::nrOfLightsType d) {
            if (d > maxDest) maxDest = d;
            totalDests++;
        });
    }
    CHECK(totalDests == N);        // exactly the 210 shell cells map to a light
    CHECK(maxDest < N);            // no destination is >= N (no overflow)
    CHECK(lut.logicalCount() == 9 * 9 * 9);  // one logical entry per box cell
}

// Sphere + Mirror: the modifier's box-coordinate destinations are translated
// into driver-index space; no destination escapes [0, lightCount).
TEST_CASE("Layer: sphere + mirror maps into driver-index space") {
    mm::SphereLayout s;
    s.radius = 4;
    const mm::nrOfLightsType N = s.lightCount();

    mm::Layouts group;
    group.addChild(&s);
    mm::Layer layer;
    layer.setLayouts(&group);
    layer.setChannelsPerLight(3);
    mm::MirrorModifier mirror;
    mirror.mirrorX = true;
    layer.addChild(&mirror);
    layer.onBuildControls();
    layer.onBuildState();

    CHECK(layer.lut().hasLUT());
    CHECK(layer.physicalLightCount() == N);

    const mm::MappingLUT& lut = layer.lut();
    for (mm::nrOfLightsType li = 0; li < lut.logicalCount(); li++) {
        lut.forEachDestination(li, [&](mm::nrOfLightsType d) {
            CHECK(d < N);   // mirror destinations are driver indices, never box indices
        });
    }
}
