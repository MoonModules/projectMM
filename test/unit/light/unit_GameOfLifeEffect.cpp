// @module GameOfLifeEffect

#include "doctest.h"
#include "light/effects/GameOfLifeEffect.h"

using namespace mm;

// The B#/S# parser turns a rule string into birth/survive neighbour sets. Conway = B3/S23.
TEST_CASE("GameOfLife: ruleset parser reads B/S sets") {
    GameOfLifeEffect gol;
    gol.ruleset = 1;                       // "Conway B3/S23"
    gol.parseRulesetForTest();
    CHECK(gol.birthForTest(3));
    CHECK_FALSE(gol.birthForTest(2));
    CHECK(gol.surviveForTest(2));
    CHECK(gol.surviveForTest(3));
    CHECK_FALSE(gol.surviveForTest(1));

    gol.ruleset = 2;                       // "HighLife B36/S23"
    gol.parseRulesetForTest();
    CHECK(gol.birthForTest(3));
    CHECK(gol.birthForTest(6));            // the HighLife extra birth rule
    CHECK_FALSE(gol.birthForTest(5));
}

// A 2×2 block is a Conway still life: every live cell has 3 neighbours (survives), and the
// surrounding dead cells never have exactly 3 (no births). It must be identical after a step.
TEST_CASE("GameOfLife: a 2x2 block is a stable still life") {
    GameOfLifeEffect gol;
    gol.ruleset = 1;
    REQUIRE(gol.allocateForTest(6, 6, 1));
    gol.setCellForTest(2, 2, 0, true);
    gol.setCellForTest(3, 2, 0, true);
    gol.setCellForTest(2, 3, 0, true);
    gol.setCellForTest(3, 3, 0, true);

    gol.stepForTest();

    // The block is unchanged…
    CHECK(gol.isAliveForTest(2, 2, 0));
    CHECK(gol.isAliveForTest(3, 2, 0));
    CHECK(gol.isAliveForTest(2, 3, 0));
    CHECK(gol.isAliveForTest(3, 3, 0));
    // …and nothing else was born.
    CHECK_FALSE(gol.isAliveForTest(1, 1, 0));
    CHECK_FALSE(gol.isAliveForTest(4, 4, 0));
}

// A horizontal 3-cell blinker oscillates to vertical after one step (period-2 oscillator). This is
// the canonical "the rules actually run" check: birth on 3, death of the ends (1 neighbour each).
TEST_CASE("GameOfLife: a 3-cell blinker flips orientation each step") {
    GameOfLifeEffect gol;
    gol.ruleset = 1;
    REQUIRE(gol.allocateForTest(7, 7, 1));
    // Horizontal: (2,3) (3,3) (4,3)
    gol.setCellForTest(2, 3, 0, true);
    gol.setCellForTest(3, 3, 0, true);
    gol.setCellForTest(4, 3, 0, true);

    gol.stepForTest();

    // Now vertical: (3,2) (3,3) (3,4); the horizontal ends died (1 neighbour), the centre survives,
    // and the cells above/below the centre were born (3 neighbours).
    CHECK(gol.isAliveForTest(3, 2, 0));
    CHECK(gol.isAliveForTest(3, 3, 0));
    CHECK(gol.isAliveForTest(3, 4, 0));
    CHECK_FALSE(gol.isAliveForTest(2, 3, 0));   // end died
    CHECK_FALSE(gol.isAliveForTest(4, 3, 0));   // end died

    gol.stepForTest();                          // flips back to horizontal
    CHECK(gol.isAliveForTest(2, 3, 0));
    CHECK(gol.isAliveForTest(4, 3, 0));
    CHECK_FALSE(gol.isAliveForTest(3, 2, 0));
}

// A lone cell (0 neighbours) dies — the dead-by-isolation rule, and a sanity check that an empty
// grid stays empty (no spontaneous births at count 0 under Conway).
TEST_CASE("GameOfLife: a lone cell dies and an empty grid stays empty") {
    GameOfLifeEffect gol;
    gol.ruleset = 1;
    REQUIRE(gol.allocateForTest(5, 5, 1));
    gol.setCellForTest(2, 2, 0, true);
    gol.stepForTest();
    CHECK_FALSE(gol.isAliveForTest(2, 2, 0));   // isolated → dead
    // Whole grid empty now.
    for (lengthType y = 0; y < 5; y++)
        for (lengthType x = 0; x < 5; x++)
            CHECK_FALSE(gol.isAliveForTest(x, y, 0));
}
