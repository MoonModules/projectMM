// @module BlendMap
// @also MappingLUT

#include "doctest.h"
#include "light/layers/BlendMap.h"
#include "platform/platform.h"

#include <cstring>

// Identity mapping (logical N → physical N) leaves every byte unchanged.
TEST_CASE("blendMap identity (no LUT) copies buffer") {
    mm::Buffer src, dst;
    src.allocate(4, 3);
    dst.allocate(4, 3);

    // Fill src with known data
    for (size_t i = 0; i < src.bytes(); i++) {
        src.data()[i] = static_cast<uint8_t>(i + 1);
    }

    mm::MappingLUT lut;
    lut.setIdentity(4);

    mm::blendMap(src, dst, lut, 3);

    for (size_t i = 0; i < dst.bytes(); i++) {
        CHECK(dst.data()[i] == src.data()[i]);
    }
}

// One logical light routed to multiple physical positions copies the colour to each (mirror-style mappings work).
TEST_CASE("blendMap 1:N mapping duplicates pixels") {
    // 2 logical lights, 4 physical lights
    // Logical 0 → physical {0, 3}
    // Logical 1 → physical {1, 2}
    mm::Buffer src, dst;
    src.allocate(2, 3);  // 2 logical lights
    dst.allocate(4, 3);  // 4 physical lights

    // Set logical light 0 to red, light 1 to blue
    src.data()[0] = 255; src.data()[1] = 0; src.data()[2] = 0;    // red
    src.data()[3] = 0;   src.data()[4] = 0; src.data()[5] = 255;  // blue

    mm::MappingLUT lut;
    lut.build(2, 4);
    mm::nrOfLightsType map0[] = {0, 3};
    lut.setMapping(0, map0, 2);
    mm::nrOfLightsType map1[] = {1, 2};
    lut.setMapping(1, map1, 2);
    lut.finalize();

    mm::blendMap(src, dst, lut, 3);

    // Physical 0 = red (from logical 0)
    CHECK(dst.data()[0] == 255);
    CHECK(dst.data()[1] == 0);
    CHECK(dst.data()[2] == 0);

    // Physical 1 = blue (from logical 1)
    CHECK(dst.data()[3] == 0);
    CHECK(dst.data()[4] == 0);
    CHECK(dst.data()[5] == 255);

    // Physical 2 = blue (from logical 1, mirrored)
    CHECK(dst.data()[6] == 0);
    CHECK(dst.data()[7] == 0);
    CHECK(dst.data()[8] == 255);

    // Physical 3 = red (from logical 0, mirrored)
    CHECK(dst.data()[9] == 255);
    CHECK(dst.data()[10] == 0);
    CHECK(dst.data()[11] == 0);
}

// A paged LUT (forced via the maxAllocBlock test cap) must produce a
// byte-identical dst to a single-alloc LUT with the same mapping. Paging is an
// allocation detail; blendMap output must not depend on it. This is the
// end-to-end pin for the no-PSRAM-fragmentation fix.
TEST_CASE("blendMap is identical for single-alloc and paged LUTs") {
    struct Cap { ~Cap() { mm::platform::setTestMaxAllocBlock(0); } } cap;

    // 200 logical → up to 5000 physical destinations: large enough that paging
    // spans multiple 4096-entry pages, with a run crossing a page boundary.
    const mm::nrOfLightsType logical = 200, physical = 5000;
    mm::Buffer src, dstSingle, dstPaged;
    src.allocate(logical, 3);
    dstSingle.allocate(physical, 3);
    dstPaged.allocate(physical, 3);
    for (size_t i = 0; i < src.bytes(); i++) src.data()[i] = static_cast<uint8_t>((i * 37) & 0xFF);

    // Build the same mapping into both LUTs; each logical light maps to ~25
    // physicals (200 × 25 = 5000), filled sequentially so runs straddle pages.
    auto fill = [&](mm::MappingLUT& lut) {
        REQUIRE(lut.build(logical, physical));
        mm::nrOfLightsType phys = 0;
        for (mm::nrOfLightsType li = 0; li < logical; li++) {
            mm::nrOfLightsType run[25];
            for (int k = 0; k < 25; k++) run[k] = phys++;
            lut.setMapping(li, run, 25);
        }
        lut.finalize();
    };

    mm::MappingLUT single, paged;
    mm::platform::setTestMaxAllocBlock(0);          // unlimited → single block
    fill(single);
    CHECK_FALSE(single.isPaged());
    mm::platform::setTestMaxAllocBlock(4096);       // tiny block → force paging
    fill(paged);
    CHECK(paged.isPaged());

    mm::blendMap(src, dstSingle, single, 3);
    mm::blendMap(src, dstPaged, paged, 3);

    REQUIRE(dstSingle.bytes() == dstPaged.bytes());
    CHECK(std::memcmp(dstSingle.data(), dstPaged.data(), dstSingle.bytes()) == 0);
}

// An additive (overwrites=false) LUT folding two sources onto one physical light
// adds and clamps at 255 (no overflow). overwrites=false is the opt-in for the
// rare overlap case (future multi-layer compositing); the default copy path
// would instead overwrite, so this pins the additive contract explicitly.
TEST_CASE("blendMap additive clamping (overwrites=false)") {
    mm::Buffer src, dst;
    src.allocate(1, 3);
    dst.allocate(1, 3);

    src.data()[0] = 200; src.data()[1] = 200; src.data()[2] = 200;

    // Map logical 0 to physical 0 TWICE — forces double-add with clamping
    mm::MappingLUT lut;
    lut.build(1, 2);
    lut.setOverwrites(false);  // opt into additive blending
    mm::nrOfLightsType map[] = {0, 0};
    lut.setMapping(0, map, 2);
    lut.finalize();

    mm::blendMap(src, dst, lut, 3);
    // 200 + 200 = 400 → clamped to 255
    CHECK(dst.data()[0] == 255);
    CHECK(dst.data()[1] == 255);
    CHECK(dst.data()[2] == 255);
}

// The default (overwrites=true) path plain-copies: two sources mapped to the
// same physical means the LAST writer wins, no addition. Pins the fast path.
TEST_CASE("blendMap overwrite path: last write wins, no add") {
    mm::Buffer src, dst;
    src.allocate(2, 3);
    dst.allocate(1, 3);
    src.data()[0] = 200; src.data()[1] = 200; src.data()[2] = 200;  // logical 0
    src.data()[3] = 50;  src.data()[4] = 50;  src.data()[5] = 50;   // logical 1

    mm::MappingLUT lut;
    lut.build(2, 2);  // overwrites_ defaults true
    mm::nrOfLightsType m0[] = {0};
    mm::nrOfLightsType m1[] = {0};  // both logical lights → physical 0
    lut.setMapping(0, m0, 1);
    lut.setMapping(1, m1, 1);
    lut.finalize();

    mm::blendMap(src, dst, lut, 3);
    // Copy, not add: logical 1 (last) wins → 50, not 250-clamped.
    CHECK(dst.data()[0] == 50);
    CHECK(dst.data()[1] == 50);
    CHECK(dst.data()[2] == 50);
}

// Sparse overwrite mapping clears untouched physical cells. A sphere-style
// layout maps only a subset of the physical box to a source; the rest must end
// up black, not retain stale data from a previous frame. Pre-fills dst dirty
// and asserts unmapped cells are zeroed — fails if BlendMap's dst.clear() is
// removed (the regression target).
TEST_CASE("blendMap overwrite path clears untouched cells (sparse mapping)") {
    mm::Buffer src, dst;
    src.allocate(2, 3);   // 2 logical lights
    dst.allocate(4, 3);   // 4 physical; only 2 are mapped
    src.data()[0] = 10; src.data()[1] = 20; src.data()[2] = 30;   // logical 0
    src.data()[3] = 40; src.data()[4] = 50; src.data()[5] = 60;   // logical 1

    // Dirty the whole dst so a missing clear would leave stale bytes behind.
    for (size_t i = 0; i < dst.bytes(); i++) dst.data()[i] = 0xFF;

    mm::MappingLUT lut;
    lut.build(2, 2);                 // overwrites_ defaults true
    mm::nrOfLightsType m0[] = {0};   // logical 0 → physical 0
    mm::nrOfLightsType m1[] = {2};   // logical 1 → physical 2 (1 and 3 unmapped)
    lut.setMapping(0, m0, 1);
    lut.setMapping(1, m1, 1);
    lut.finalize();

    mm::blendMap(src, dst, lut, 3);

    // Mapped cells hold their source values.
    CHECK(dst.data()[0] == 10); CHECK(dst.data()[1] == 20); CHECK(dst.data()[2] == 30);
    CHECK(dst.data()[6] == 40); CHECK(dst.data()[7] == 50); CHECK(dst.data()[8] == 60);
    // Unmapped cells (physical 1 and 3) were cleared, not left dirty.
    CHECK(dst.data()[3] == 0); CHECK(dst.data()[4] == 0); CHECK(dst.data()[5] == 0);
    CHECK(dst.data()[9] == 0); CHECK(dst.data()[10] == 0); CHECK(dst.data()[11] == 0);
}
