// @module Sort

// Pins the generic core sort primitive (mm::insertionSort) used for the small bounded
// collections the system holds (device lists, UI rows). Verifies it orders correctly,
// is stable (equal elements keep input order), and handles the trivial sizes (0/1).

#include "doctest.h"
#include "core/Sort.h"

#include <cstring>

TEST_CASE("insertionSort orders ints ascending") {
    int a[] = {5, 1, 4, 2, 8, 3};
    mm::insertionSort(a, 6, [](int x, int y) { return x < y; });
    int expect[] = {1, 2, 3, 4, 5, 8};
    CHECK(std::memcmp(a, expect, sizeof(a)) == 0);
}

TEST_CASE("insertionSort with a custom (descending) comparator") {
    int a[] = {3, 1, 2};
    mm::insertionSort(a, 3, [](int x, int y) { return x > y; });
    CHECK(a[0] == 3); CHECK(a[1] == 2); CHECK(a[2] == 1);
}

TEST_CASE("insertionSort orders C-strings (the device-name use case)") {
    const char* a[] = {"MM-70BC", "Bench P4", "MM-CAFE", "192.168.1.1"};
    mm::insertionSort(a, 4, [](const char* x, const char* y) { return std::strcmp(x, y) < 0; });
    CHECK(std::strcmp(a[0], "192.168.1.1") == 0);
    CHECK(std::strcmp(a[1], "Bench P4") == 0);
    CHECK(std::strcmp(a[2], "MM-70BC") == 0);
    CHECK(std::strcmp(a[3], "MM-CAFE") == 0);
}

TEST_CASE("insertionSort is stable — equal keys keep input order") {
    // Sort by .key only; .tag must stay in original relative order for equal keys.
    struct Row { int key; int tag; };
    Row a[] = {{1, 0}, {1, 1}, {0, 2}, {1, 3}};
    mm::insertionSort(a, 4, [](const Row& x, const Row& y) { return x.key < y.key; });
    CHECK(a[0].key == 0);                 // the single 0 sorts first
    CHECK(a[1].tag == 0);                 // then the 1s, in their original order
    CHECK(a[2].tag == 1);
    CHECK(a[3].tag == 3);
}

TEST_CASE("insertionSort handles empty and single-element arrays") {
    int none[1] = {7};
    mm::insertionSort(none, 0, [](int x, int y) { return x < y; });   // n=0: no-op, no OOB
    CHECK(none[0] == 7);
    int one[] = {42};
    mm::insertionSort(one, 1, [](int x, int y) { return x < y; });
    CHECK(one[0] == 42);
}
