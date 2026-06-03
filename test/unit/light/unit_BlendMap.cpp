// @module BlendMap
// @also MappingLUT

#include "doctest.h"
#include "light/layers/BlendMap.h"

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

// Two logical lights writing into the same physical light add and clamp at 255 (no overflow).
TEST_CASE("blendMap additive clamping") {
    mm::Buffer src, dst;
    src.allocate(1, 3);
    dst.allocate(1, 3);

    src.data()[0] = 200; src.data()[1] = 200; src.data()[2] = 200;

    // Map logical 0 to physical 0 TWICE — forces double-add with clamping
    mm::MappingLUT lut;
    lut.build(1, 2);
    mm::nrOfLightsType map[] = {0, 0};
    lut.setMapping(0, map, 2);
    lut.finalize();

    mm::blendMap(src, dst, lut, 3);
    // 200 + 200 = 400 → clamped to 255
    CHECK(dst.data()[0] == 255);
    CHECK(dst.data()[1] == 255);
    CHECK(dst.data()[2] == 255);
}
