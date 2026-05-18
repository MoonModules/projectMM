#include "doctest.h"
#include "light/modules/effects/NoiseEffect.h"
#include "light/Buffer.h"

using namespace mm::light;

TEST_CASE("NoiseEffect name and controls") {
    NoiseEffect noise;
    noise.addControls();
    CHECK(std::strcmp(noise.name(), "Noise 2D") == 0);
    CHECK(noise.controlCount() == 2);
}

TEST_CASE("NoiseEffect produces non-zero pixels") {
    NoiseEffect noise;
    noise.addControls();

    Buffer buf;
    buf.allocate(16);
    RenderContext ctx{buf.pixels(), 4, 4, 1, 0};
    noise.setContext(ctx);
    noise.loop();

    bool hasColor = false;
    for (size_t i = 0; i < buf.count(); ++i) {
        if (buf[i].r != 0 || buf[i].g != 0 || buf[i].b != 0) {
            hasColor = true;
            break;
        }
    }
    CHECK(hasColor);
}

TEST_CASE("NoiseEffect pixels vary (not constant)") {
    NoiseEffect noise;
    noise.addControls();

    Buffer buf;
    buf.allocate(16);
    RenderContext ctx{buf.pixels(), 4, 4, 1, 0};
    noise.setContext(ctx);
    noise.loop();

    bool foundDifferent = false;
    for (size_t i = 1; i < buf.count(); ++i) {
        if (buf[i].r != buf[0].r || buf[i].g != buf[0].g) {
            foundDifferent = true;
            break;
        }
    }
    CHECK(foundDifferent);
}

TEST_CASE("NoiseEffect is deterministic") {
    NoiseEffect noise;
    noise.addControls();

    Buffer buf1, buf2;
    buf1.allocate(16);
    buf2.allocate(16);

    RenderContext ctx1{buf1.pixels(), 4, 4, 1, 10};
    noise.setContext(ctx1);
    noise.loop();

    RenderContext ctx2{buf2.pixels(), 4, 4, 1, 10};
    noise.setContext(ctx2);
    noise.loop();

    for (size_t i = 0; i < 16; ++i) {
        CHECK(buf1[i].r == buf2[i].r);
        CHECK(buf1[i].g == buf2[i].g);
        CHECK(buf1[i].b == buf2[i].b);
    }
}

TEST_CASE("NoiseEffect different scale changes pattern") {
    NoiseEffect noise;
    noise.addControls();

    Buffer buf1, buf2;
    buf1.allocate(16);
    buf2.allocate(16);

    RenderContext ctx1{buf1.pixels(), 4, 4, 1, 0};
    noise.setContext(ctx1);
    noise.loop();

    noise.setControl(1, uint16_t(100)); // change scale
    RenderContext ctx2{buf2.pixels(), 4, 4, 1, 0};
    noise.setContext(ctx2);
    noise.loop();

    bool different = false;
    for (size_t i = 0; i < 16; ++i) {
        if (buf1[i].r != buf2[i].r) { different = true; break; }
    }
    CHECK(different);
}
