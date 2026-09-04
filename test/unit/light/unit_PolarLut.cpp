// @module polar
// @also math16, ScratchBuffer

// The polar address table: the angle and radius of every pixel, computed once instead of per frame.
// These pin what a radial effect depends on when it reads the table instead of calling atan16 and
// dist16: that the address is the same one it would have computed, that the center and the edges
// land where the geometry says, that resizing the grid rebuilds it, and that a device too tight for
// the tables still renders rather than crashing.

#include "doctest.h"
#include "core/MoonModule.h"
#include "light/polar.h"
#include "platform/platform.h"

#include <cstdint>

using namespace mm;

namespace {

/// A bare module to own the tables, the fixture the ScratchBuffer tests use.
struct Owner : MoonModule {
    PolarLut lut{*this};
};

/// The signed difference between two angles, the short way around the circle.
int32_t angleDelta(angle16 a, angle16 b) {
    int32_t d = static_cast<int32_t>(a) - static_cast<int32_t>(b);
    if (d > 32768) d -= 65536;
    if (d < -32768) d += 65536;
    return d < 0 ? -d : d;
}

}  // namespace

TEST_CASE("the table gives the same angle a radial effect would compute per pixel") {
    Owner o;
    REQUIRE(o.lut.prepare(32, 32, /*wide=*/true));
    const int32_t cx = 16, cy = 16;
    for (uint16_t y = 0; y < 32; y++) {
        for (uint16_t x = 0; x < 32; x++) {
            const angle16 expected = atan16(static_cast<int32_t>(y) - cy, static_cast<int32_t>(x) - cx);
            CHECK(o.lut.angleAt(x, y) == expected);
        }
    }
}

TEST_CASE("the 8-bit table is the same address at 256 steps, which is what it costs half the memory for") {
    Owner o;
    REQUIRE(o.lut.prepare(24, 24, /*wide=*/false));
    const int32_t cx = 12, cy = 12;
    int32_t worst = 0;
    for (uint16_t y = 0; y < 24; y++)
        for (uint16_t x = 0; x < 24; x++) {
            const angle16 exact = atan16(static_cast<int32_t>(y) - cy, static_cast<int32_t>(x) - cx);
            const int32_t d = angleDelta(o.lut.angleAt(x, y), exact);
            worst = d > worst ? d : worst;
        }
    CHECK(worst < 256);          // within one 8-bit step of the exact angle, never further
}

TEST_CASE("the radius runs from nothing at the center to full scale at the furthest corner") {
    Owner o;
    REQUIRE(o.lut.prepare(33, 33, true));
    CHECK(o.lut.radiusAt(16, 16) == 0);              // the center pixel
    CHECK(o.lut.radiusAt(0, 0) == 65535);            // a corner, the furthest point
    CHECK(o.lut.radiusAt(32, 32) > 60000);
    // Halfway out along an axis is about half the scale of the diagonal corner.
    CHECK(o.lut.radiusAt(32, 16) > 40000);
    CHECK(o.lut.radiusAt(32, 16) < 55000);
}

TEST_CASE("a wide panel fills to its edges instead of banding in a circle inside it") {
    // The aspect-ratio property: radii scale against the furthest corner, so a 64x16 strip reaches
    // full scale at its ends rather than saturating everywhere past the short axis.
    Owner o;
    REQUIRE(o.lut.prepare(64, 16, true));
    CHECK(o.lut.radiusAt(0, 8) > 55000);             // the far end of the long axis
    CHECK(o.lut.radiusAt(32, 0) < 40000);            // the edge of the short axis is nearer
    CHECK(o.lut.radiusAt(32, 8) == 0);
}

TEST_CASE("angles point the way the geometry says") {
    Owner o;
    REQUIRE(o.lut.prepare(65, 65, true));
    // atan16(dy, dx): zero along +x, a quarter turn per axis, going the way atan2 goes.
    CHECK(angleDelta(o.lut.angleAt(64, 32), 0) < 512);
    CHECK(angleDelta(o.lut.angleAt(32, 64), 16384) < 512);
    CHECK(angleDelta(o.lut.angleAt(0, 32), 32768) < 512);
    CHECK(angleDelta(o.lut.angleAt(32, 0), 49152) < 512);
}

TEST_CASE("opposite sides of the center face opposite ways") {
    Owner o;
    REQUIRE(o.lut.prepare(41, 41, true));
    for (uint16_t k = 1; k < 20; k++) {
        const angle16 a = o.lut.angleAt(static_cast<uint16_t>(20 + k), 20);
        const angle16 b = o.lut.angleAt(static_cast<uint16_t>(20 - k), 20);
        CHECK(angleDelta(a, static_cast<angle16>(b + 32768)) < 512);
        CHECK(o.lut.radiusAt(static_cast<uint16_t>(20 + k), 20) == o.lut.radiusAt(static_cast<uint16_t>(20 - k), 20));
    }
}

TEST_CASE("resizing the grid rebuilds the table for the new geometry") {
    Owner o;
    REQUIRE(o.lut.prepare(16, 16, true));
    const uint16_t small = o.lut.radiusAt(0, 0);
    const std::size_t smallBytes = o.lut.bytes();

    REQUIRE(o.lut.prepare(64, 64, true));
    CHECK(o.lut.bytes() == smallBytes * 16);         // 16x the pixels
    CHECK(o.lut.radiusAt(0, 0) == small);            // a corner is still a corner
    CHECK(o.lut.radiusAt(32, 32) == 0);              // but the center moved with the grid
}

TEST_CASE("preparing the same geometry again costs nothing") {
    Owner o;
    REQUIRE(o.lut.prepare(32, 32, false));
    const angle16 before = o.lut.angleAt(7, 9);
    const std::size_t bytes = o.lut.bytes();
    for (int i = 0; i < 100; i++) CHECK(o.lut.prepare(32, 32, false));   // as a tick() would
    CHECK(o.lut.angleAt(7, 9) == before);
    CHECK(o.lut.bytes() == bytes);
}

TEST_CASE("switching precision live does not hold both tables") {
    // An effect offering precision as a control switches while running; the memory must follow.
    Owner o;
    REQUIRE(o.lut.prepare(32, 32, false));
    const std::size_t narrow = o.lut.bytes();
    CHECK(narrow == 32 * 32 * 2);                    // one byte of angle and one of radius

    REQUIRE(o.lut.prepare(32, 32, true));
    CHECK(o.lut.bytes() == 32 * 32 * 4);             // two bytes each, and NOT six
    CHECK(o.lut.wide());

    REQUIRE(o.lut.prepare(32, 32, false));
    CHECK(o.lut.bytes() == narrow);
    CHECK_FALSE(o.lut.wide());
}

TEST_CASE("the tables are the module's memory, freed with it") {
    Owner o;
    CHECK(o.dynamicBytes() == 0);
    REQUIRE(o.lut.prepare(48, 48, true));
    CHECK(o.dynamicBytes() == 48 * 48 * 4);
    REQUIRE(o.lut.prepare(48, 48, false));
    CHECK(o.dynamicBytes() == 48 * 48 * 2);
}

TEST_CASE("an empty grid reports not ready instead of building a table of nothing") {
    Owner o;
    CHECK_FALSE(o.lut.prepare(0, 0, false));
    CHECK_FALSE(o.lut.ready());
    CHECK(o.lut.angle(0) == 0);                      // and reads are safe, not a fault
    CHECK(o.lut.radius(0) == 0);
}

TEST_CASE("a single-pixel grid is its own center") {
    Owner o;
    REQUIRE(o.lut.prepare(1, 1, true));
    CHECK(o.lut.radiusAt(0, 0) == 0);
    CHECK(o.lut.maxRadius() >= 1);                   // never zero: nothing divides by it
}

TEST_CASE("the index form and the coordinate form address the same pixel") {
    // A pixel loop reads by running index; both must agree or a loop silently shears the field.
    Owner o;
    REQUIRE(o.lut.prepare(20, 12, true));
    for (uint16_t y = 0; y < 12; y++)
        for (uint16_t x = 0; x < 20; x++) {
            const std::size_t i = static_cast<std::size_t>(y) * 20 + x;
            CHECK(o.lut.angle(i) == o.lut.angleAt(x, y));
            CHECK(o.lut.radius(i) == o.lut.radiusAt(x, y));
        }
}

TEST_CASE("the table is refused rather than taking the last of a small heap") {
    // On a device without PSRAM the tables are a real fraction of the heap. Asking for a wall-sized
    // grid must leave the reserve that protects stacks, WiFi and HTTP intact, and the caller then
    // computes the address per pixel instead. The desktop reports unlimited heap, so this pins the
    // arithmetic of the gate rather than the allocation.
    Owner o;
    const std::size_t reserve = platform::HEAP_RESERVE;
    const std::size_t free = platform::freeHeap();
    if (free == 0) {
        // Desktop: unlimited, so even a wall-sized table is granted and the fallback is unused.
        CHECK(o.lut.prepare(128, 128, true));
        CHECK(o.lut.bytes() == 128 * 128 * 4);
    } else {
        const std::size_t budget = free > reserve ? free - reserve : 0;
        const std::size_t want = 128u * 128u * 4u;
        CHECK(o.lut.prepare(128, 128, true) == (budget >= want));
    }
}

TEST_CASE("a refused table leaves nothing allocated behind") {
    Owner o;
    REQUIRE(o.lut.prepare(32, 32, false));
    CHECK(o.dynamicBytes() > 0);
    CHECK_FALSE(o.lut.prepare(0, 0, false));         // an impossible grid
    CHECK(o.lut.bytes() == 0);
    CHECK(o.dynamicBytes() == 0);
    CHECK_FALSE(o.lut.ready());
}

TEST_CASE("releasing the table gives the memory back and the next prepare rebuilds it") {
    Owner o;
    REQUIRE(o.lut.prepare(48, 48, false));
    const std::size_t held = o.dynamicBytes();
    CHECK(held == 48 * 48 * 2);
    o.lut.release();
    CHECK(o.dynamicBytes() == 0);
    CHECK_FALSE(o.lut.ready());
    REQUIRE(o.lut.prepare(48, 48, false));
    CHECK(o.dynamicBytes() == held);
    CHECK(o.lut.radiusAt(24, 24) == 0);              // and it is the same table as before
}
