#include "doctest.h"
#include "light/Buffer.h"

TEST_CASE("Buffer allocate and accessors") {
    mm::Buffer buf;
    CHECK(buf.allocate(256, 3));
    CHECK(buf.count() == 256);
    CHECK(buf.channelsPerLight() == 3);
    CHECK(buf.bytes() == 768);
    CHECK(buf.data() != nullptr);
    CHECK(buf.span().size() == 768);
}

TEST_CASE("Buffer clear zeros data") {
    mm::Buffer buf;
    buf.allocate(10, 3);
    // Write some data
    buf.data()[0] = 0xFF;
    buf.data()[15] = 0xAB;
    buf.clear();
    for (size_t i = 0; i < buf.bytes(); i++) {
        CHECK(buf.data()[i] == 0);
    }
}

TEST_CASE("Buffer move constructor") {
    mm::Buffer a;
    a.allocate(100, 3);
    auto* ptr = a.data();

    mm::Buffer b(std::move(a));
    CHECK(b.data() == ptr);
    CHECK(b.count() == 100);
    CHECK(b.channelsPerLight() == 3);
    CHECK(a.data() == nullptr);
    CHECK(a.count() == 0);
}

TEST_CASE("Buffer move assignment") {
    mm::Buffer a;
    a.allocate(100, 3);
    auto* ptr = a.data();

    mm::Buffer b;
    b = std::move(a);
    CHECK(b.data() == ptr);
    CHECK(b.count() == 100);
    CHECK(a.data() == nullptr);
}

TEST_CASE("Buffer double free is safe") {
    mm::Buffer buf;
    buf.allocate(10, 3);
    buf.free();
    buf.free(); // should not crash
    CHECK(buf.data() == nullptr);
    CHECK(buf.count() == 0);
}

TEST_CASE("Buffer allocate with zero returns false") {
    mm::Buffer buf;
    CHECK_FALSE(buf.allocate(0, 3));
    CHECK_FALSE(buf.allocate(10, 0));
}
