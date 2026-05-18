#include "doctest.h"
#include "light/modules/effects/RainbowEffect.h"
#include "light/Buffer.h"

using namespace mm::light;

TEST_CASE("RainbowEffect name and controls") {
    RainbowEffect rainbow;
    rainbow.addControls();
    CHECK(std::strcmp(rainbow.name(), "Rainbow 2D") == 0);
    CHECK(rainbow.controlCount() == 1);
}

TEST_CASE("RainbowEffect produces non-zero pixels") {
    RainbowEffect rainbow;
    rainbow.addControls();

    Buffer buf;
    buf.allocate(10);
    RenderContext ctx{buf.pixels(), 10, 1, 1, 0};
    rainbow.setContext(ctx);
    rainbow.loop();

    bool hasColor = false;
    for (size_t i = 0; i < buf.count(); ++i) {
        if (buf[i].r != 0 || buf[i].g != 0 || buf[i].b != 0) {
            hasColor = true;
            break;
        }
    }
    CHECK(hasColor);
}

TEST_CASE("RainbowEffect pixels are different (not solid)") {
    RainbowEffect rainbow;
    rainbow.addControls();

    Buffer buf;
    buf.allocate(10);
    RenderContext ctx{buf.pixels(), 10, 1, 1, 0};
    rainbow.setContext(ctx);
    rainbow.loop();

    bool foundDifferent = false;
    for (size_t i = 1; i < buf.count(); ++i) {
        if (buf[i].r != buf[0].r || buf[i].g != buf[0].g || buf[i].b != buf[0].b) {
            foundDifferent = true;
            break;
        }
    }
    CHECK(foundDifferent);
}

TEST_CASE("RainbowEffect is deterministic") {
    RainbowEffect rainbow;
    rainbow.addControls();

    Buffer buf1, buf2;
    buf1.allocate(10);
    buf2.allocate(10);

    RenderContext ctx1{buf1.pixels(), 10, 1, 1, 42};
    rainbow.setContext(ctx1);
    rainbow.loop();

    RenderContext ctx2{buf2.pixels(), 10, 1, 1, 42};
    rainbow.setContext(ctx2);
    rainbow.loop();

    for (size_t i = 0; i < 10; ++i) {
        CHECK(buf1[i].r == buf2[i].r);
        CHECK(buf1[i].g == buf2[i].g);
        CHECK(buf1[i].b == buf2[i].b);
    }
}

TEST_CASE("RainbowEffect different speed changes pattern") {
    RainbowEffect rainbow;
    rainbow.addControls();

    Buffer buf1, buf2;
    buf1.allocate(10);
    buf2.allocate(10);

    RenderContext ctx1{buf1.pixels(), 10, 1, 1, 5};
    rainbow.setContext(ctx1);
    rainbow.loop();

    rainbow.setControl(0, uint16_t(10)); // change speed
    RenderContext ctx2{buf2.pixels(), 10, 1, 1, 5};
    rainbow.setContext(ctx2);
    rainbow.loop();

    bool different = false;
    for (size_t i = 0; i < 10; ++i) {
        if (buf1[i].r != buf2[i].r) { different = true; break; }
    }
    CHECK(different);
}
