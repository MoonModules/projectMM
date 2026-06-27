// @module MoonLive

#include "doctest.h"
#include "core/moonlive/MoonLive.h"
#include "core/moonlive/moonlive_emit.h"
#include "platform/platform.h"

#include <cstdint>
#include <vector>

// MoonLive Stage 1a: the load-bearing slice — emit a fixed-colour fill as native code,
// place it in executable memory, and call it over a buffer. These tests run the WHOLE path
// in-process on the desktop host backend (the host ISA's emit + platform::allocExec/
// writeExec + a real call), so they prove emit → exec → call → buffer-write works off
// hardware. The Xtensa backend is validated by the live S3 run, not here.

using namespace mm;

TEST_CASE("MoonLive emitFill produces a non-empty routine") {
    uint8_t code[256];
    size_t n = moonlive::emitFill(code, sizeof(code), 1, 2, 3);
    CHECK(n > 0);
    CHECK(n <= sizeof(code));
}

TEST_CASE("MoonLive emitFill rejects a too-small buffer (degrades, no overrun)") {
    uint8_t tiny[2];
    CHECK(moonlive::emitFill(tiny, sizeof(tiny), 1, 2, 3) == 0);
}

TEST_CASE("MoonLive emitFill/emitAnimatedFill reject a null output buffer (no crash)") {
    CHECK(moonlive::emitFill(nullptr, 256, 1, 2, 3) == 0);   // ample cap, null out → 0, not a deref
    CHECK(moonlive::emitAnimatedFill(nullptr, 256) == 0);
}

TEST_CASE("MoonLive compiles and fills a buffer with the chosen colour") {
    moonlive::MoonLive engine;
    REQUIRE(engine.compile(/*r*/ 10, /*g*/ 20, /*b*/ 200));
    REQUIRE(engine.ok());

    // A 5-light, 3-channel buffer pre-filled with a sentinel so a missed write shows.
    std::vector<uint8_t> buf(5 * 3, 0xAB);
    engine.run(buf.data(), 5, 3, /*t*/ 0);

    for (int i = 0; i < 5; i++) {
        CHECK(buf[i*3 + 0] == 10);
        CHECK(buf[i*3 + 1] == 20);
        CHECK(buf[i*3 + 2] == 200);
    }
}

TEST_CASE("MoonLive run on zero lights writes nothing (robust to empty)") {
    moonlive::MoonLive engine;
    REQUIRE(engine.compile(255, 0, 0));
    std::vector<uint8_t> buf(3, 0xAB);
    engine.run(buf.data(), 0, 3, 0);          // nLights == 0
    CHECK(buf[0] == 0xAB);                  // untouched
}

// The native routines write channels +0/+1/+2 per light, so a layer with fewer than 3
// channels per light can't hold RGB — run() must leave it untouched, not overrun it.
TEST_CASE("MoonLive run is a no-op on sub-RGB buffers (cpl 1 and 2)") {
    moonlive::MoonLive engine;
    REQUIRE(engine.compile(255, 255, 255));
    for (uint8_t cpl : {uint8_t(1), uint8_t(2)}) {
        std::vector<uint8_t> buf(8 * cpl, 0xAB);   // exact size — an RGB write WOULD overrun
        engine.run(buf.data(), 8, cpl, 0);
        for (auto v : buf) CHECK(v == 0xAB);       // every byte untouched, no out-of-bounds
    }
    // null buffer is also a safe no-op.
    engine.run(nullptr, 8, 3, 0);
}

TEST_CASE("MoonLive recompile swaps the colour; free returns to !ok") {
    moonlive::MoonLive engine;
    REQUIRE(engine.compile(1, 1, 1));
    std::vector<uint8_t> buf(3, 0);
    engine.run(buf.data(), 1, 3, 0);
    CHECK(buf[0] == 1);

    REQUIRE(engine.compile(9, 8, 7));       // recompile a new colour
    engine.run(buf.data(), 1, 3, 0);
    CHECK(buf[0] == 9); CHECK(buf[1] == 8); CHECK(buf[2] == 7);

    engine.free();
    CHECK_FALSE(engine.ok());
    // run() after free is a safe no-op (no call through null).
    engine.run(buf.data(), 1, 3, 0);
}

TEST_CASE("MoonLive animated fill derives colour from the per-frame t") {
    moonlive::MoonLive engine;
    REQUIRE(engine.compileAnimated());
    REQUIRE(engine.ok());
    std::vector<uint8_t> buf(4 * 3, 0);

    // red = (t>>3)&0xFF, green=0, blue=64. Two different t -> two different reds, proving
    // the runtime arg reaches the emitted native code and changes its output.
    engine.run(buf.data(), 4, 3, /*t*/ 0);
    for (int i = 0; i < 4; i++) {
        CHECK(buf[i*3 + 0] == 0);    // t>>3 == 0
        CHECK(buf[i*3 + 1] == 0);
        CHECK(buf[i*3 + 2] == 64);
    }

    engine.run(buf.data(), 4, 3, /*t*/ 800);   // 800>>3 = 100
    for (int i = 0; i < 4; i++) {
        CHECK(buf[i*3 + 0] == 100);
        CHECK(buf[i*3 + 2] == 64);
    }

    engine.run(buf.data(), 4, 3, /*t*/ 2048);  // 2048>>3 = 256 -> &0xFF = 0
    CHECK(buf[0] == 0);                          // wraps at 256, as a byte does
}

TEST_CASE("platform allocExec returns usable executable memory, freeExec releases it") {
    void* blk = platform::allocExec(64);
    REQUIRE(blk != nullptr);
    // Copy the emitted fill in via writeExec (the IRAM/cache-safe path) and call it.
    uint8_t code[256];
    size_t n = moonlive::emitFill(code, sizeof(code), 7, 7, 7);
    platform::writeExec(blk, code, n);
    auto fn = reinterpret_cast<moonlive::FillFn>(blk);
    std::vector<uint8_t> buf(3, 0);
    fn(buf.data(), 1, 3);
    CHECK(buf[0] == 7);
    platform::freeExec(blk, 64);
}
