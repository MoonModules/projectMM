// @module Layer
// @also RainbowEffect, NoiseEffect, PlasmaEffect, SpiralEffect, FireEffect, ParticlesEffect

#include "doctest.h"
#include "light/layers/Layer.h"
#include "light/layouts/GridLayout.h"
#include "light/effects/RainbowEffect.h"
#include "light/effects/NoiseEffect.h"
#include "light/effects/PlasmaEffect.h"
#include "light/effects/SpiralEffect.h"
#include "light/effects/FireEffect.h"
#include "light/effects/ParticlesEffect.h"

// Layer::extrude lets a low-dimensional effect "just work" on a higher-dimensional
// grid: the effect writes only its own slice (D2 → z=0 plane; D1 → column at x=0,z=0)
// and Layer copies that slice across the unused axes. These tests pin the
// behaviour so a D2 effect on a 3D grid produces identical z-slices, and a
// hypothetical D1 effect (1D runs along Y) produces identical columns and slices.
//
// A separate group of tests covers the other direction: a D3 effect run on a
// 2D or 1D layer. EffectBase's contract says the effect must honour the layer's
// actual dimensions (width/height/depth) at frame time, regardless of the D it
// declared. We pin that here for the two production D3 effects (Noise, Plasma)
// — if a future D3 effect hardcodes z bounds and overruns the buffer, the same
// test pattern applied to it will catch it (size check + no-zero-pixels).

// A D2 effect (Rainbow) on a 3D layer writes z=0 once; Layer::extrude copies that slice across every z>0 — slices are byte-identical.
TEST_CASE("D2 effect on 3D grid: z-slices are identical (Layer::extrude)") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 4;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    // Rainbow opts into D2 — it writes only z=0 and trusts extrude to fill z.
    // (The framework default is D3; D2 is the opt-in promise.)
    mm::RainbowEffect rainbow;
    layer.addChild(&rainbow);

    layer.onBuildState();
    layer.loop();

    auto* data = layer.buffer().data();
    const size_t sliceBytes = static_cast<size_t>(grid.width) * grid.height * 3;
    REQUIRE(layer.buffer().bytes() == sliceBytes * grid.depth);

    // Every z>0 slice must byte-equal z=0 — proves extrude copied the plane.
    for (mm::lengthType z = 1; z < grid.depth; z++) {
        const uint8_t* z0 = data;
        const uint8_t* zn = data + z * sliceBytes;
        for (size_t i = 0; i < sliceBytes; i++) {
            REQUIRE(zn[i] == z0[i]);
        }
    }
}

// A D1 stub: 1D runs along Y, so it writes a gradient down the (x=0) column and
// nothing else. The effect doesn't iterate x/z — extrude is expected to spread the
// column across x (then across z). Keeps the test independent of any specific D1
// production effect (none exist today; this pins the framework contract).
class D1StubEffect : public mm::EffectBase {
public:
    mm::Dim dimensions() const override { return mm::Dim::D1; }
    void loop() override {
        uint8_t* buf = buffer();
        mm::lengthType w = width();
        mm::lengthType h = height();
        uint8_t cpl = channelsPerLight();
        for (mm::lengthType y = 0; y < h; y++) {
            size_t offset = static_cast<size_t>(y) * w * cpl;   // the x=0 pixel of row y
            buf[offset + 0] = static_cast<uint8_t>(y * 4);
            buf[offset + 1] = static_cast<uint8_t>(255 - y * 4);
            buf[offset + 2] = 128;
        }
    }
};

// A D1 effect writes the x=0 column; extrude duplicates it across every x and every z-slice.
TEST_CASE("D1 effect on 3D grid: columns and z-slices are identical (Layer::extrude)") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 4;
    grid.depth = 3;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    D1StubEffect d1;
    layer.addChild(&d1);

    layer.onBuildState();
    layer.loop();

    auto* data = layer.buffer().data();
    const size_t cpl = 3;
    const size_t rowBytes = static_cast<size_t>(grid.width) * cpl;
    const size_t sliceBytes = rowBytes * grid.height;

    // Within z=0: in every row, each x>0 pixel equals the x=0 pixel (column spread across x).
    for (mm::lengthType y = 0; y < grid.height; y++) {
        const uint8_t* x0 = data + static_cast<size_t>(y) * rowBytes;   // the x=0 pixel
        for (mm::lengthType x = 1; x < grid.width; x++) {
            const uint8_t* xn = x0 + static_cast<size_t>(x) * cpl;
            for (size_t c = 0; c < cpl; c++) REQUIRE(xn[c] == x0[c]);
        }
    }

    // Across z: every z>0 slice equals z=0 slice.
    for (mm::lengthType z = 1; z < grid.depth; z++) {
        const uint8_t* z0 = data;
        const uint8_t* zn = data + z * sliceBytes;
        for (size_t i = 0; i < sliceBytes; i++) {
            REQUIRE(zn[i] == z0[i]);
        }
    }
}

// --- D3 effects on lower-dimensional layers ---------------------------------
// Contract pinned in EffectBase.h: a D3-declared effect must honour the layer's
// runtime dimensions (depth=1 means iterate only z=0; height=1 means iterate
// only y=0). Tests below verify the two shipped D3 effects (Noise, Plasma)
// produce a fully-written buffer of the expected size on 2D and 1D layers —
// catches a future D3 effect that hardcodes a fixed-depth loop.

template<typename EffectT>
static void check_d3_on_2d(const char* tag) {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;  // 2D layer
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    EffectT effect;
    layer.addChild(&effect);

    layer.onBuildState();
    layer.loop();

    auto* data = layer.buffer().data();
    const size_t expectedBytes = static_cast<size_t>(grid.width) * grid.height * 3;
    REQUIRE_MESSAGE(layer.buffer().bytes() == expectedBytes, tag);

    // Some pixel must be non-zero — confirms the effect actually iterated and
    // wrote (a buffer of all zeros would mean the loop skipped everything).
    bool anyNonZero = false;
    for (size_t i = 0; i < expectedBytes; i++) {
        if (data[i] != 0) { anyNonZero = true; break; }
    }
    REQUIRE_MESSAGE(anyNonZero, tag);
}

template<typename EffectT>
static void check_d3_on_1d(const char* tag) {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 1;  // 1D layer
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    EffectT effect;
    layer.addChild(&effect);

    layer.onBuildState();
    layer.loop();

    auto* data = layer.buffer().data();
    const size_t expectedBytes = static_cast<size_t>(grid.width) * 3;
    REQUIRE_MESSAGE(layer.buffer().bytes() == expectedBytes, tag);

    bool anyNonZero = false;
    for (size_t i = 0; i < expectedBytes; i++) {
        if (data[i] != 0) { anyNonZero = true; break; }
    }
    REQUIRE_MESSAGE(anyNonZero, tag);
}

// NoiseEffect declared D3 still produces a valid image on a depth=1 layer (it honours the runtime depth instead of hardcoding z).
TEST_CASE("D3 effect on 2D layer: NoiseEffect produces a valid 2D image") {
    check_d3_on_2d<mm::NoiseEffect>("NoiseEffect on 2D layer");
}

// PlasmaEffect (D3) on a 2D layer same contract: valid 2D image, no buffer overrun.
TEST_CASE("D3 effect on 2D layer: PlasmaEffect produces a valid 2D image") {
    check_d3_on_2d<mm::PlasmaEffect>("PlasmaEffect on 2D layer");
}

// NoiseEffect (D3) on a 1D layer (height=depth=1) writes a valid strip and never overflows.
TEST_CASE("D3 effect on 1D layer: NoiseEffect produces a valid 1D strip") {
    check_d3_on_1d<mm::NoiseEffect>("NoiseEffect on 1D layer");
}

// PlasmaEffect (D3) on a 1D layer same contract: valid 1D strip, no overflow.
TEST_CASE("D3 effect on 1D layer: PlasmaEffect produces a valid 1D strip") {
    check_d3_on_1d<mm::PlasmaEffect>("PlasmaEffect on 1D layer");
}

// --- D2 effects on 3D layers: extrude must fill z ---------------------------
// Pins the contract that a D2-declared effect's z>0 slices come from extrude,
// not from the effect itself. Catches a regression where a D2 effect either
// fails to write z=0 (everything black) or where extrude no longer fills z
// (z>0 stays black). Covers one stateless (Checkerboard) and two stateful
// (Fire, Particles) effects so changes to onBuildState don't silently
// break the contract.

template<typename EffectT>
static void check_d2_on_3d(const char* tag) {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 3;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    EffectT effect;
    layer.addChild(&effect);

    layer.onBuildState();
    // Some effects (Fire, particles with random sparks) need a few frames
    // before they reliably produce visible output; one frame is enough for
    // the deterministic effects we test here.
    layer.loop();

    auto* data = layer.buffer().data();
    const size_t sliceBytes = static_cast<size_t>(grid.width) * grid.height * 3;
    REQUIRE_MESSAGE(layer.buffer().bytes() == sliceBytes * grid.depth, tag);

    // Contract: extrude duplicates z=0 across every z>0. Byte-for-byte equal.
    for (mm::lengthType z = 1; z < grid.depth; z++) {
        const uint8_t* z0 = data;
        const uint8_t* zn = data + z * sliceBytes;
        for (size_t i = 0; i < sliceBytes; i++) {
            REQUIRE_MESSAGE(zn[i] == z0[i], tag);
        }
    }
}

// SpiralEffect (D2) on a 3D layer: extrude copies z=0 to every z>0 (stateless D2 contract).
TEST_CASE("D2 effect on 3D layer: Spiral extruded across z") {
    check_d2_on_3d<mm::SpiralEffect>("SpiralEffect on 3D layer");
}

// FireEffect (D2, stateful — heat buffer sized to w×h) extrudes cleanly across z on a 3D layer.
TEST_CASE("D2 effect on 3D layer: Fire extruded across z (heat sized to w*h)") {
    check_d2_on_3d<mm::FireEffect>("FireEffect on 3D layer");
}

// ParticlesEffect (D2, stateful — trail sized to w×h×cpl) extrudes cleanly across z on a 3D layer.
TEST_CASE("D2 effect on 3D layer: Particles extruded across z (trail sized to w*h*cpl)") {
    check_d2_on_3d<mm::ParticlesEffect>("ParticlesEffect on 3D layer");
}
