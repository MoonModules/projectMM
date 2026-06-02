// @module MoonModule

#include "doctest.h"
#include "core/MoonModule.h"

namespace {

class StubModule : public mm::MoonModule {};

// Helper: collect child names in order into a vector-like string for easy CHECK comparisons.
// Using setName() each child gets a tag we can read back.
struct Fixture {
    mm::MoonModule parent;
    StubModule a, b, c, d;
    Fixture() {
        a.setName("A");
        b.setName("B");
        c.setName("C");
        d.setName("D");
        parent.addChild(&a);
        parent.addChild(&b);
        parent.addChild(&c);
        parent.addChild(&d);
    }
    std::string order() const {
        std::string s;
        for (uint8_t i = 0; i < parent.childCount(); i++) {
            s += parent.child(i)->name();
        }
        return s;
    }
};

} // namespace

// Moving a child to its current index returns false and changes nothing.
TEST_CASE("moveChildTo: noop when already at target") {
    Fixture f;
    CHECK(f.order() == "ABCD");
    // a is at index 0; move to 0 → false (no-op signal).
    CHECK_FALSE(f.parent.moveChildTo(&f.a, 0));
    CHECK(f.order() == "ABCD");  // unchanged
}

// Moving a child forward shifts intervening children leftward to close the gap.
TEST_CASE("moveChildTo: forward (toward end)") {
    Fixture f;
    // Move B (index 1) to index 3 (last). Expect A C D B.
    CHECK(f.parent.moveChildTo(&f.b, 3));
    CHECK(f.order() == "ACDB");
}

// Moving a child backward shifts intervening children rightward to make room.
TEST_CASE("moveChildTo: backward (toward start)") {
    Fixture f;
    // Move D (index 3) to index 0. Expect D A B C.
    CHECK(f.parent.moveChildTo(&f.d, 0));
    CHECK(f.order() == "DABC");
}

// Single-position moves work in either direction (UI's up/down arrow buttons).
TEST_CASE("moveChildTo: one position swap (up arrow / down arrow)") {
    Fixture f;
    // Move C (index 2) up by 1 → index 1. Expect A C B D.
    CHECK(f.parent.moveChildTo(&f.c, 1));
    CHECK(f.order() == "ACBD");
    // Move C (now index 1) down by 1 → index 2. Back to A B C D.
    CHECK(f.parent.moveChildTo(&f.c, 2));
    CHECK(f.order() == "ABCD");
}

// A target index beyond childCount() is refused (returns false, tree untouched).
TEST_CASE("moveChildTo: out-of-range index rejected") {
    Fixture f;
    CHECK_FALSE(f.parent.moveChildTo(&f.a, 99));  // beyond childCount
    CHECK(f.order() == "ABCD");
}

// A module that isn't actually a child of the parent is refused.
TEST_CASE("moveChildTo: non-child rejected") {
    Fixture f;
    StubModule orphan;
    orphan.setName("X");
    CHECK_FALSE(f.parent.moveChildTo(&orphan, 0));
    CHECK(f.order() == "ABCD");  // tree untouched
}

// Middle-to-middle moves preserve the integrity of every sibling's index.
TEST_CASE("moveChildTo: middle-to-middle shifts correctly") {
    Fixture f;
    // Move B (index 1) to index 2. Expect A C B D.
    CHECK(f.parent.moveChildTo(&f.b, 2));
    CHECK(f.order() == "ACBD");
}
