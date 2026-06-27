// @module light_types

#include "doctest.h"
#include "light/light_types.h"

// Coord3D is the coordinate/size type the modifier fold interface mutates. The
// per-axis (Hadamard) operators are what let a modifier read like geometry
// (`pos = pos % size`); the % and / variants must guard a zero/degenerate axis so
// a fold over a 1-wide or empty axis can't divide by zero or wrap.

TEST_CASE("Coord3D arithmetic is per-axis") {
    mm::Coord3D a{10, 20, 30};
    mm::Coord3D b{1, 2, 3};
    CHECK((a + b) == mm::Coord3D{11, 22, 33});
    CHECK((a - b) == mm::Coord3D{9, 18, 27});
    CHECK((a * b) == mm::Coord3D{10, 40, 90});
}

TEST_CASE("Coord3D modulo and divide fold per axis") {
    mm::Coord3D pos{7, 5, 9};
    mm::Coord3D size{4, 4, 4};
    // The textbook Multiply fold: position within its tile, and which tile.
    CHECK((pos % size) == mm::Coord3D{3, 1, 1});
    CHECK((pos / size) == mm::Coord3D{1, 1, 2});
}

TEST_CASE("Coord3D % and / guard a zero or degenerate axis") {
    mm::Coord3D pos{7, 5, 3};
    // A 0-extent or 1-extent axis must not divide-by-zero or wrap; the coordinate
    // passes through (% ) or stays put-ish (/), so a fold over a flat axis is safe.
    mm::Coord3D zero{0, 1, 0};
    CHECK((pos % zero) == mm::Coord3D{7, 0, 3});   // x: %0 → unchanged, y: %1 → 0, z: %0 → unchanged
    CHECK((pos / zero) == mm::Coord3D{7, 5, 3});   // /0 → unchanged on x and z, /1 → unchanged on y
}

TEST_CASE("Coord3D equality") {
    CHECK(mm::Coord3D{1, 2, 3} == mm::Coord3D{1, 2, 3});
    CHECK(mm::Coord3D{1, 2, 3} != mm::Coord3D{1, 2, 4});
    CHECK(mm::Coord3D{} == mm::Coord3D{0, 0, 0});   // default-constructed is the origin
}
