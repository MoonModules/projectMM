#include "doctest.h"
#include "light/modules/modifiers/RotateModifier.h"
#include "light/Buffer.h"

using namespace mm::light;

TEST_CASE("RotateModifier default speed") {
    RotateModifier rotate;
    rotate.addControls();
    CHECK(rotate.speed() == 1);
}

TEST_CASE("RotateModifier frame 0 no change") {
    RotateModifier rotate;
    rotate.addControls();

    Buffer buf;
    buf.allocate(4);
    buf[0] = {10, 0, 0};
    buf[1] = {20, 0, 0};
    buf[2] = {30, 0, 0};
    buf[3] = {40, 0, 0};

    rotate.transformPixels(buf.pixels(), 0, 2, 2);
    // frame 0, angle = 0, no rotation
    CHECK(buf[0].r == 10);
    CHECK(buf[1].r == 20);
    CHECK(buf[2].r == 30);
    CHECK(buf[3].r == 40);
}

TEST_CASE("RotateModifier different frames produce different output") {
    RotateModifier rotate;
    rotate.addControls();

    Buffer buf1, buf2;
    buf1.allocate(16);
    buf2.allocate(16);

    // Fill both with same pattern
    for (size_t i = 0; i < 16; ++i) {
        RGB c = {static_cast<uint8_t>(i * 15), 0, 0};
        buf1[i] = c;
        buf2[i] = c;
    }

    rotate.transformPixels(buf1.pixels(), 10, 4, 4);
    rotate.transformPixels(buf2.pixels(), 100, 4, 4);

    bool different = false;
    for (size_t i = 0; i < 16; ++i) {
        if (buf1[i].r != buf2[i].r) { different = true; break; }
    }
    CHECK(different);
}

TEST_CASE("RotateModifier higher speed rotates more") {
    RotateModifier rotate;
    rotate.addControls();

    Buffer buf1, buf2;
    buf1.allocate(16);
    buf2.allocate(16);

    for (size_t i = 0; i < 16; ++i) {
        RGB c = {static_cast<uint8_t>(i * 15), 0, 0};
        buf1[i] = c;
        buf2[i] = c;
    }

    rotate.transformPixels(buf1.pixels(), 10, 4, 4);

    rotate.setControl(0, uint16_t(10)); // speed = 10
    rotate.transformPixels(buf2.pixels(), 10, 4, 4);

    bool different = false;
    for (size_t i = 0; i < 16; ++i) {
        if (buf1[i].r != buf2[i].r) { different = true; break; }
    }
    CHECK(different);
}
