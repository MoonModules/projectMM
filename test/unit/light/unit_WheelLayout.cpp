// @module WheelLayout

#include "doctest.h"
#include "light/layouts/WheelLayout.h"

#include <vector>

// WheelLayout places lights along straight spokes radiating from a centre hub.
// These tests pin: lightCount() == spokes*ledsPerSpoke and matches the iterator,
// every coordinate is non-negative (the wheel is centre-shifted into the address
// space), and the index sequence is dense [0, count).

namespace {

struct Pt { mm::nrOfLightsType idx; mm::lengthType x, y; };

std::vector<Pt> collect(const mm::WheelLayout& wheel) {
    std::vector<Pt> pts;
    wheel.forEachCoord([](void* ctx, mm::nrOfLightsType idx, mm::lengthType x, mm::lengthType y, mm::lengthType) {
        static_cast<std::vector<Pt>*>(ctx)->push_back({idx, x, y});
    }, &pts);
    return pts;
}

} // namespace

TEST_CASE("WheelLayout lightCount = spokes * ledsPerSpoke and matches the iterator") {
    mm::WheelLayout w;
    w.spokes = 8; w.ledsPerSpoke = 10;
    CHECK(w.lightCount() == 80);
    CHECK(collect(w).size() == 80);
}

TEST_CASE("WheelLayout indices are dense [0, count)") {
    mm::WheelLayout w;
    w.spokes = 6; w.ledsPerSpoke = 5;
    auto pts = collect(w);
    REQUIRE(pts.size() == 30);
    for (mm::nrOfLightsType i = 0; i < pts.size(); i++) CHECK(pts[i].idx == i);
}

TEST_CASE("WheelLayout coordinates are non-negative (centre-shifted into address space)") {
    mm::WheelLayout w;
    w.spokes = 16; w.ledsPerSpoke = 12;
    for (const auto& p : collect(w)) {
        CHECK(p.x >= 0);
        CHECK(p.y >= 0);
        CHECK(p.x <= 2 * 12);   // within the (2*ledsPerSpoke) bounding box
        CHECK(p.y <= 2 * 12);
    }
}

TEST_CASE("WheelLayout different spoke counts give different layouts") {
    mm::WheelLayout a, b;
    a.spokes = 4; a.ledsPerSpoke = 8;
    b.spokes = 12; b.ledsPerSpoke = 8;
    CHECK(a.lightCount() != b.lightCount());
}
