// @module SphereLayout

#include "doctest.h"
#include "light/layouts/SphereLayout.h"

#include <vector>

// SphereLayout places lights on the surface of a hollow sphere — a one-light-
// thick lattice shell, centre excluded. These tests pin: the shell is hollow
// (no centre / interior point), lightCount() matches what forEachCoord emits,
// the points are symmetric about the centre, and the radius-1 base case.

namespace {

struct Pt { mm::lengthType x, y, z; };

std::vector<Pt> collectPoints(const mm::SphereLayout& s) {
    std::vector<Pt> pts;
    s.forEachCoord([](void* ctx, mm::nrOfLightsType, mm::lengthType x, mm::lengthType y, mm::lengthType z) {
        static_cast<std::vector<Pt>*>(ctx)->push_back({x, y, z});
    }, &pts);
    return pts;
}

} // namespace

// lightCount() must equal the number of points forEachCoord emits — they share
// one shell predicate, so allocation and fill can never disagree.
TEST_CASE("SphereLayout lightCount matches the iterator") {
    for (mm::lengthType r : {1, 2, 4, 8}) {
        mm::SphereLayout s;
        s.radius = r;
        auto pts = collectPoints(s);
        CHECK(s.lightCount() == pts.size());
        CHECK(s.lightCount() > 0);  // every radius produces a non-empty shell
    }
}

// The sphere is HOLLOW: the centre lattice point (r,r,r) is never emitted, and
// neither is any interior point (distance < radius-0.5 from centre).
TEST_CASE("SphereLayout is hollow — no centre or interior points") {
    mm::SphereLayout s;
    s.radius = 5;
    const int r = 5;
    auto pts = collectPoints(s);

    for (const auto& p : pts) {
        const int dx = p.x - r, dy = p.y - r, dz = p.z - r;
        const int d4 = 4 * (dx * dx + dy * dy + dz * dz);
        // On the half-open shell band [r-0.5, r+0.5): 4d^2 in [(2r-1)^2,(2r+1)^2).
        CHECK(d4 >= (2 * r - 1) * (2 * r - 1));   // not interior
        CHECK(d4 <  (2 * r + 1) * (2 * r + 1));   // not exterior
        CHECK_FALSE((dx == 0 && dy == 0 && dz == 0));  // never the centre
    }
}

// radius = 1 is the smallest hollow sphere: the 6 axis neighbours (d^2=1) plus
// the 12 edge points (d^2=2) of the centre — 18 lights, no centre.
TEST_CASE("SphereLayout radius 1 is the 18-point base shell") {
    mm::SphereLayout s;
    s.radius = 1;
    auto pts = collectPoints(s);
    CHECK(pts.size() == 18);
    // All within the 3x3x3 box (coords 0..2), none at the centre (1,1,1).
    for (const auto& p : pts) {
        CHECK(p.x >= 0); CHECK(p.x <= 2);
        CHECK(p.y >= 0); CHECK(p.y <= 2);
        CHECK(p.z >= 0); CHECK(p.z <= 2);
        CHECK_FALSE((p.x == 1 && p.y == 1 && p.z == 1));
    }
}

// The shell is symmetric about the centre: for every emitted point its mirror
// through the centre is also emitted (a sphere has no preferred direction).
TEST_CASE("SphereLayout shell is centre-symmetric") {
    mm::SphereLayout s;
    s.radius = 4;
    const int r = 4;
    auto pts = collectPoints(s);

    auto has = [&](mm::lengthType x, mm::lengthType y, mm::lengthType z) {
        for (const auto& p : pts) if (p.x == x && p.y == y && p.z == z) return true;
        return false;
    };
    for (const auto& p : pts) {
        // Mirror through centre: (r - d) on each axis.
        CHECK(has(static_cast<mm::lengthType>(2 * r - p.x),
                  static_cast<mm::lengthType>(2 * r - p.y),
                  static_cast<mm::lengthType>(2 * r - p.z)));
    }
}

// Physical indices are sequential 0..N-1 over the emitted shell points (no gaps
// from the unindexed lattice voids), so the buffer maps 1:1 to emitted lights.
TEST_CASE("SphereLayout emits sequential physical indices") {
    mm::SphereLayout s;
    s.radius = 3;
    std::vector<mm::nrOfLightsType> idxs;
    s.forEachCoord([](void* ctx, mm::nrOfLightsType idx, mm::lengthType, mm::lengthType, mm::lengthType) {
        static_cast<std::vector<mm::nrOfLightsType>*>(ctx)->push_back(idx);
    }, &idxs);
    REQUIRE(idxs.size() == s.lightCount());
    for (size_t i = 0; i < idxs.size(); i++) CHECK(idxs[i] == static_cast<mm::nrOfLightsType>(i));
}

// Default radius is a sensible small sphere (not 0, not huge).
TEST_CASE("SphereLayout default radius") {
    mm::SphereLayout s;
    CHECK(s.radius == 4);
    CHECK(s.lightCount() > 0);
}
