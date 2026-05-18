#include "doctest.h"
#include "light/Pixel.h"

using namespace mm::light;

static_assert(sizeof(RGB) == 3, "RGB must be exactly 3 bytes");

TEST_CASE("RGB construction and fields") {
    RGB c{10, 20, 30};
    CHECK(c.r == 10);
    CHECK(c.g == 20);
    CHECK(c.b == 30);
}

TEST_CASE("RGB::black and RGB::white") {
    auto b = RGB::black();
    CHECK(b.r == 0); CHECK(b.g == 0); CHECK(b.b == 0);
    auto w = RGB::white();
    CHECK(w.r == 255); CHECK(w.g == 255); CHECK(w.b == 255);
}

TEST_CASE("scale8 basics") {
    CHECK(scale8(255, 128) == 127);  // ~half
    CHECK(scale8(0, 128) == 0);
    CHECK(scale8(128, 0) == 0);
    CHECK(scale8(255, 255) == 254);  // (255*255)>>8 = 254
}

TEST_CASE("blend black to white") {
    auto result = blend(RGB::black(), RGB::white(), 128);
    // Should be approximately middle gray
    CHECK(result.r >= 120);
    CHECK(result.r <= 135);
    CHECK(result.g >= 120);
    CHECK(result.b >= 120);
}

TEST_CASE("blend extremes") {
    auto a = RGB{100, 50, 200};
    auto same = blend(a, a, 128);
    // Blending a color with itself should stay close
    CHECK(same.r >= 99);
    CHECK(same.r <= 101);
}

TEST_CASE("fromHSV red") {
    auto red = RGB::fromHSV(0, 255, 255);
    CHECK(red.r > 200);
    CHECK(red.g < 50);
    CHECK(red.b < 50);
}

TEST_CASE("fromHSV green") {
    auto green = RGB::fromHSV(85, 255, 255);
    CHECK(green.g > 200);
    CHECK(green.r < 50);
    CHECK(green.b < 50);
}

TEST_CASE("fromHSV blue") {
    auto blue = RGB::fromHSV(170, 255, 255);
    CHECK(blue.b > 200);
    CHECK(blue.r < 50);
    CHECK(blue.g < 50);
}

TEST_CASE("fromHSV zero saturation is white") {
    auto white = RGB::fromHSV(0, 0, 255);
    CHECK(white.r == 255);
    CHECK(white.g == 255);
    CHECK(white.b == 255);
}

TEST_CASE("fromHSV zero value is black") {
    auto black = RGB::fromHSV(0, 255, 0);
    CHECK(black.r == 0);
    CHECK(black.g == 0);
    CHECK(black.b == 0);
}
