#include "doctest.h"
#include "platform/Alloc.h"
#include "platform/Timing.h"

TEST_CASE("platform::alloc returns non-null for valid size") {
    void* p = mm::platform::alloc(1024);
    REQUIRE(p != nullptr);
    mm::platform::free(p);
}

TEST_CASE("platform::alloc zero bytes") {
    void* p = mm::platform::alloc(0);
    // implementation-defined but should not crash
    mm::platform::free(p);
}

TEST_CASE("platform::millis returns increasing values") {
    uint32_t a = mm::platform::millis();
    uint32_t b = mm::platform::millis();
    CHECK(b >= a);
}

TEST_CASE("platform::micros returns increasing values") {
    uint64_t a = mm::platform::micros();
    uint64_t b = mm::platform::micros();
    CHECK(b >= a);
}

TEST_CASE("platform::micros has higher resolution than millis") {
    uint32_t ms = mm::platform::millis();
    uint64_t us = mm::platform::micros();
    // micros should be at least millis * 1000 (with some tolerance)
    CHECK(us >= static_cast<uint64_t>(ms) * 1000ULL);
}
