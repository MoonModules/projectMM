// @module Layouts

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/SphereLayout.h"

#include <vector>

// Tree mutation on the Layouts container: add one, add more than one, replace a
// layout with a different type, and remove a layout. These exercise the same
// addChild / replaceChildAt / removeChild primitives the HTTP handlers
// (handleAddModule / handleReplaceModule / handleDeleteModule) drive, but at the
// container level so the light-count / coordinate stitching is verified too.
//
// Stack-allocated layouts here own themselves — removeChild / replaceChildAt do
// NOT delete (the caller owns), so no leak and no double-free on scope exit.

namespace {

mm::nrOfLightsType countCoords(const mm::Layouts& layouts) {
    mm::nrOfLightsType n = 0;
    layouts.forEachCoord([](void* ctx, mm::nrOfLightsType, mm::lengthType, mm::lengthType, mm::lengthType) {
        (*static_cast<mm::nrOfLightsType*>(ctx))++;
    }, &n);
    return n;
}

} // namespace

// Add a single layout: the container reports its light count and iterates it.
TEST_CASE("Layouts add one layout") {
    mm::Layouts layouts;
    mm::GridLayout g;
    g.width = 4; g.height = 4; g.depth = 1;   // 16 lights

    REQUIRE(layouts.addChild(&g));
    CHECK(layouts.childCount() == 1);
    CHECK(layouts.totalLightCount() == 16);
    CHECK(countCoords(layouts) == 16);  // iterator agrees with the count
}

// Add more than one layout (mixed types): counts sum, indices stitch end-to-end.
TEST_CASE("Layouts add multiple layouts of different types") {
    mm::Layouts layouts;
    mm::GridLayout g;
    g.width = 4; g.height = 1; g.depth = 1;   // 4 lights, indices 0..3
    mm::SphereLayout s;
    s.radius = 1;                              // 18-light shell, indices 4..21

    REQUIRE(layouts.addChild(&g));
    REQUIRE(layouts.addChild(&s));
    CHECK(layouts.childCount() == 2);

    const mm::nrOfLightsType expected = g.lightCount() + s.lightCount();
    CHECK(layouts.totalLightCount() == expected);
    CHECK(countCoords(layouts) == expected);

    // Indices stitch: the sphere's lights start where the grid's end (no holes,
    // no overlap). Collect indices and verify a contiguous 0..expected-1 range.
    std::vector<mm::nrOfLightsType> idxs;
    layouts.forEachCoord([](void* ctx, mm::nrOfLightsType idx, mm::lengthType, mm::lengthType, mm::lengthType) {
        static_cast<std::vector<mm::nrOfLightsType>*>(ctx)->push_back(idx);
    }, &idxs);
    REQUIRE(idxs.size() == expected);
    for (mm::nrOfLightsType i = 0; i < expected; i++) CHECK(idxs[i] == i);
}

// Replace a layout with a different type at the same slot: the other layouts and
// their order are preserved; only the replaced slot's contribution changes.
TEST_CASE("Layouts replace a layout with another type") {
    mm::Layouts layouts;
    mm::GridLayout a;
    a.width = 2; a.height = 1; a.depth = 1;   // 2 lights
    mm::GridLayout b;
    b.width = 5; b.height = 1; b.depth = 1;   // 5 lights — the one we replace
    layouts.addChild(&a);
    layouts.addChild(&b);
    CHECK(layouts.totalLightCount() == 7);

    // Replace slot 1 (the 5-light grid) with a sphere. replaceChildAt returns
    // the old child (caller owns it; we don't delete — it's stack-allocated).
    mm::SphereLayout s;
    s.radius = 2;
    mm::MoonModule* old = layouts.replaceChildAt(1, &s);
    CHECK(old == &b);
    CHECK(layouts.childCount() == 2);
    CHECK(layouts.child(0) == &a);     // slot 0 untouched
    CHECK(layouts.child(1) == &s);     // slot 1 now the sphere

    // Total is slot-0 grid (2) + the sphere's shell, and the iterator agrees.
    const mm::nrOfLightsType expected = a.lightCount() + s.lightCount();
    CHECK(layouts.totalLightCount() == expected);
    CHECK(countCoords(layouts) == expected);
}

// Remove a layout: it leaves the tree, the remaining layouts shift to close the
// gap, and the total drops by exactly the removed layout's light count.
TEST_CASE("Layouts remove a layout") {
    mm::Layouts layouts;
    mm::GridLayout a;
    a.width = 3; a.height = 1; a.depth = 1;   // 3 lights
    mm::GridLayout b;
    b.width = 4; b.height = 1; b.depth = 1;   // 4 lights
    mm::GridLayout c;
    c.width = 5; c.height = 1; c.depth = 1;   // 5 lights
    layouts.addChild(&a);
    layouts.addChild(&b);
    layouts.addChild(&c);
    CHECK(layouts.totalLightCount() == 12);

    // Remove the middle one. removeChild does not delete (a/b/c are on the stack).
    REQUIRE(layouts.removeChild(&b));
    CHECK(layouts.childCount() == 2);
    CHECK(layouts.child(0) == &a);     // surviving layouts keep order, gap closed
    CHECK(layouts.child(1) == &c);
    CHECK(layouts.totalLightCount() == 8);   // 3 + 5, b's 4 gone
    CHECK(countCoords(layouts) == 8);

    // Removing a layout not present returns false and changes nothing.
    mm::GridLayout notAdded;
    CHECK_FALSE(layouts.removeChild(&notAdded));
    CHECK(layouts.childCount() == 2);
}
