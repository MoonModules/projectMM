#include "doctest.h"
#include "light/effects/CheckerboardEffect.h"
#include "light/effects/SpiralEffect.h"
#include "light/effects/PlasmaPaletteEffect.h"
#include "light/effects/RipplesEffect.h"
#include "light/effects/GlowParticlesEffect.h"
#include "light/effects/LavaLampEffect.h"
#include "light/layouts/GridLayout.h"

namespace {

struct Ctx {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;

    Ctx(int w, int h) {
        grid.width = static_cast<mm::lengthType>(w);
        grid.height = static_cast<mm::lengthType>(h);
        grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
    }

    bool hasNonZero() {
        auto& buf = layer.buffer();
        for (size_t i = 0; i < buf.bytes(); i++) {
            if (buf.data()[i] != 0) return true;
        }
        return false;
    }

    bool cornersDiffer() {
        auto* data = layer.buffer().data();
        size_t last = (static_cast<size_t>(grid.width) * grid.height - 1) * 3;
        return data[0] != data[last] || data[1] != data[last + 1] || data[2] != data[last + 2];
    }

    // For effects with localised features (e.g. ripples): scan all pixels for
    // at least two distinct RGB triplets.
    bool hasTwoDistinctColors() {
        auto* data = layer.buffer().data();
        size_t pixels = static_cast<size_t>(grid.width) * grid.height;
        for (size_t i = 1; i < pixels; i++) {
            size_t a = (i - 1) * 3;
            size_t b = i * 3;
            if (data[a] != data[b] || data[a + 1] != data[b + 1] || data[a + 2] != data[b + 2]) {
                return true;
            }
        }
        return false;
    }
};

} // namespace

#define STATELESS_EFFECT_TEST(EFFECT) \
    TEST_CASE(#EFFECT " writes non-zero RGB") { \
        Ctx ctx(16, 16); \
        mm::EFFECT effect; \
        ctx.layer.addChild(&effect); \
        ctx.layer.onAllocateMemory(); \
        ctx.layer.loop(); \
        CHECK(ctx.hasNonZero()); \
    } \
    TEST_CASE(#EFFECT " spatial variation") { \
        Ctx ctx(32, 32); \
        mm::EFFECT effect; \
        ctx.layer.addChild(&effect); \
        ctx.layer.onAllocateMemory(); \
        ctx.layer.loop(); \
        CHECK(ctx.cornersDiffer()); \
    }

TEST_CASE("CheckerboardEffect writes non-zero RGB") {
    Ctx ctx(16, 16);
    mm::CheckerboardEffect effect;
    ctx.layer.addChild(&effect);
    ctx.layer.onAllocateMemory();
    ctx.layer.loop();
    CHECK(ctx.hasNonZero());
}

TEST_CASE("CheckerboardEffect spatial variation") {
    Ctx ctx(32, 32);
    mm::CheckerboardEffect effect;
    effect.cell_size = 4;
    ctx.layer.addChild(&effect);
    ctx.layer.onAllocateMemory();
    ctx.layer.loop();
    auto* data = ctx.layer.buffer().data();
    CHECK(data[0] != data[4 * 3]);
}
STATELESS_EFFECT_TEST(SpiralEffect)
STATELESS_EFFECT_TEST(PlasmaPaletteEffect)
STATELESS_EFFECT_TEST(GlowParticlesEffect)
STATELESS_EFFECT_TEST(LavaLampEffect)

// RipplesEffect has localised features (thin rings); corner-pair check is
// too strict, so we scan for any two distinct pixels instead.
TEST_CASE("RipplesEffect writes non-zero RGB") {
    Ctx ctx(16, 16);
    mm::RipplesEffect effect;
    ctx.layer.addChild(&effect);
    ctx.layer.onAllocateMemory();
    ctx.layer.loop();
    CHECK(ctx.hasNonZero());
}

TEST_CASE("RipplesEffect spatial variation") {
    Ctx ctx(32, 32);
    mm::RipplesEffect effect;
    ctx.layer.addChild(&effect);
    ctx.layer.onAllocateMemory();
    ctx.layer.loop();
    CHECK(ctx.hasTwoDistinctColors());
}
