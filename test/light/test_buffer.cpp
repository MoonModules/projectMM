#include "doctest.h"
#include "light/Buffer.h"

using namespace mm::light;

TEST_CASE("Buffer allocate and count") {
    Buffer buf;
    CHECK(buf.count() == 0);
    CHECK(buf.allocate(100));
    CHECK(buf.count() == 100);
    CHECK(buf.bytes() == 300); // 100 * 3
}

TEST_CASE("Buffer clear sets all to black") {
    Buffer buf;
    buf.allocate(10);
    buf[0] = {255, 128, 64};
    buf.clear();
    CHECK(buf[0].r == 0);
    CHECK(buf[0].g == 0);
    CHECK(buf[0].b == 0);
}

TEST_CASE("Buffer fill") {
    Buffer buf;
    buf.allocate(5);
    RGB red{255, 0, 0};
    buf.fill(red);
    for (size_t i = 0; i < buf.count(); ++i) {
        CHECK(buf[i].r == 255);
        CHECK(buf[i].g == 0);
        CHECK(buf[i].b == 0);
    }
}

TEST_CASE("Buffer operator[] read/write") {
    Buffer buf;
    buf.allocate(3);
    buf[0] = {10, 20, 30};
    buf[1] = {40, 50, 60};
    buf[2] = {70, 80, 90};
    CHECK(buf[0].r == 10);
    CHECK(buf[1].g == 50);
    CHECK(buf[2].b == 90);
}

TEST_CASE("Buffer span access") {
    Buffer buf;
    buf.allocate(5);
    buf.fill({1, 2, 3});
    auto span = buf.pixels();
    CHECK(span.size() == 5);
    CHECK(span[0].r == 1);
    CHECK(span[4].b == 3);
}

TEST_CASE("Buffer reallocate frees old") {
    Buffer buf;
    buf.allocate(100);
    buf[99] = {1, 2, 3};
    buf.allocate(50);
    CHECK(buf.count() == 50);
    // Old data should be cleared
    CHECK(buf[0].r == 0);
}

TEST_CASE("Buffer move constructor") {
    Buffer a;
    a.allocate(10);
    a[0] = {42, 43, 44};
    Buffer b(std::move(a));
    CHECK(b.count() == 10);
    CHECK(b[0].r == 42);
    CHECK(a.count() == 0);
}

TEST_CASE("Buffer move assignment") {
    Buffer a, b;
    a.allocate(10);
    a[0] = {1, 2, 3};
    b = std::move(a);
    CHECK(b.count() == 10);
    CHECK(b[0].r == 1);
    CHECK(a.count() == 0);
}

TEST_CASE("Buffer double free safety") {
    Buffer buf;
    buf.allocate(10);
    buf.free();
    buf.free(); // should not crash
    CHECK(buf.count() == 0);
}

TEST_CASE("Buffer allocate zero") {
    Buffer buf;
    CHECK(buf.allocate(0));
    CHECK(buf.count() == 0);
}
