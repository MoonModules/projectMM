#include "doctest.h"
#include "light/modules/layouts/WheelLayout.h"
#include <set>
#include <vector>

using namespace mm::light;

TEST_CASE("WheelLayout default controls") {
    WheelLayout wheel;
    wheel.addControls();
    CHECK(wheel.spokes() == 8);
    CHECK(wheel.ledsPerSpoke() == 10);
    CHECK(wheel.pixelCount() == 80);
}

TEST_CASE("WheelLayout forEachCoord yields correct count") {
    WheelLayout wheel;
    wheel.addControls();

    size_t count = 0;
    wheel.forEachCoord([&](uint32_t, int16_t, int16_t, int16_t) {
        ++count;
    });
    CHECK(count == 80); // 8 * 10
}

TEST_CASE("WheelLayout coordinates have angular distribution") {
    WheelLayout wheel;
    wheel.addControls();

    // Collect unique (x,y) positions
    std::set<std::pair<int16_t, int16_t>> positions;
    wheel.forEachCoord([&](uint32_t, int16_t x, int16_t y, int16_t) {
        positions.insert({x, y});
    });

    // Should have many unique positions (not all on one line)
    // With 8 spokes * 10 LEDs, some may overlap near center
    CHECK(positions.size() > 40);
}

TEST_CASE("WheelLayout change spokes") {
    WheelLayout wheel;
    wheel.addControls();
    wheel.setControl(0, uint16_t(4));
    CHECK(wheel.spokes() == 4);
    CHECK(wheel.pixelCount() == 40);

    size_t count = 0;
    wheel.forEachCoord([&](uint32_t, int16_t, int16_t, int16_t) { ++count; });
    CHECK(count == 40);
}
