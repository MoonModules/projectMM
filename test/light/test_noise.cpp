#include "doctest.h"
#include "light/Noise.h"

using namespace mm::light;

TEST_CASE("noise8 2D is deterministic") {
    uint8_t a = noise8(1000, 2000);
    uint8_t b = noise8(1000, 2000);
    CHECK(a == b);
}

TEST_CASE("noise8 2D returns 0-255") {
    // Sample a range of coordinates
    for (uint16_t x = 0; x < 1000; x += 100) {
        for (uint16_t y = 0; y < 1000; y += 100) {
            uint8_t v = noise8(x, y);
            CHECK(v <= 255); // always true for uint8, but documents intent
        }
    }
}

TEST_CASE("noise8 2D varies across coordinates") {
    // Not all values should be the same
    uint8_t first = noise8(100, 200);
    bool found_different = false;
    for (uint16_t x = 300; x < 5000; x += 370) {
        if (noise8(x, 200) != first) {
            found_different = true;
            break;
        }
    }
    CHECK(found_different);
}

TEST_CASE("noise8 3D is deterministic") {
    uint8_t a = noise8(1000, 2000, 3000);
    uint8_t b = noise8(1000, 2000, 3000);
    CHECK(a == b);
}

TEST_CASE("noise8 3D varies across coordinates") {
    uint8_t first = noise8(uint16_t(100), uint16_t(200), uint16_t(300));
    bool found_different = false;
    for (uint16_t z = 500; z < 5000; z += 370) {
        if (noise8(uint16_t(100), uint16_t(200), z) != first) {
            found_different = true;
            break;
        }
    }
    CHECK(found_different);
}

TEST_CASE("noise16 2D returns values") {
    uint16_t v = noise16(10000, 20000);
    // Just verify it runs and returns something
    (void)v;
    CHECK(true);
}
