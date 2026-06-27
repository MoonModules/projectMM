// @module MoonLive

#include "doctest.h"
#include "core/moonlive/MoonLiveCompiler.h"
#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLive.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
#include "platform/platform.h"

#include <cstdint>
#include <cstring>
#include <vector>

// MoonLive IR + assembler, exercised through the front-end + the light builtin table (the IR
// builders are no longer hand-written — the parser builds the IR from source). The headline
// check is the BEHAVIORAL GOLDEN: a compiled `fill(...)` and the hand-encoded emitFill, run
// over the same buffer, produce identical output (the assembler keeps its own register
// convention, so this is behavioural equivalence, not byte-equality).

using namespace mm;

static moonlive::BuiltinTable kT = moonlive::lightBuiltins();

namespace {
using FillFn = void (*)(uint8_t*, uint32_t, uint8_t);
FillFn place(const uint8_t* code, size_t n, void*& blkOut, size_t cap = 256) {
    void* blk = platform::allocExec(cap);
    blkOut = blk;
    if (!blk) return nullptr;
    platform::writeExec(blk, code, n);
    return reinterpret_cast<FillFn>(blk);
}
}

TEST_CASE("MoonLive compiled fill is BEHAVIORALLY identical to the hand-encoded emitFill (golden)") {
    const uint8_t cases[][3] = {{0, 0, 255}, {10, 20, 200}, {255, 255, 255}, {0, 0, 0}, {1, 2, 3}};
    for (auto& c : cases) {
        // hand-encoded reference
        uint8_t handCode[256];
        size_t hn = moonlive::emitFill(handCode, sizeof(handCode), c[0], c[1], c[2]);
        REQUIRE(hn > 0);
        void* hb = nullptr;
        auto handFn = place(handCode, hn, hb);
        REQUIRE(handFn != nullptr);

        // compiled-from-source via the IR + assembler
        char src[64];
        std::snprintf(src, sizeof(src), "fill(%u, %u, %u);", c[0], c[1], c[2]);
        uint8_t irCode[256];
        auto r = moonlive::compileSource(src, kT, irCode, sizeof(irCode));
        REQUIRE(r.ok);
        void* ib = nullptr;
        auto irFn = place(irCode, r.len, ib);
        REQUIRE(irFn != nullptr);

        std::vector<uint8_t> a(8 * 3, 0xAB), b(8 * 3, 0xAB);
        handFn(a.data(), 8, 3);
        irFn(b.data(), 8, 3);
        CHECK(a == b);                          // identical output
        for (int i = 0; i < 8; i++) {
            CHECK(b[i*3+0] == c[0]); CHECK(b[i*3+1] == c[1]); CHECK(b[i*3+2] == c[2]);
        }
        platform::freeExec(hb, 256);
        platform::freeExec(ib, 256);
    }
}

TEST_CASE("MoonLive compiled fill is robust: zero lights writes nothing") {
    uint8_t code[256];
    auto r = moonlive::compileSource("fill(255, 0, 0);", kT, code, sizeof(code));
    REQUIRE(r.ok);
    void* blk = nullptr;
    auto fn = place(code, r.len, blk);
    REQUIRE(fn != nullptr);
    std::vector<uint8_t> buf(3, 0xAB);
    fn(buf.data(), 0, 3);                 // nLights == 0 → loop guard skips
    CHECK(buf[0] == 0xAB);
    platform::freeExec(blk, 256);
}

TEST_CASE("MoonLive compileSource degrades on a too-small code buffer") {
    uint8_t tiny[4];
    CHECK_FALSE(moonlive::compileSource("fill(0,0,255);", kT, tiny, sizeof(tiny)).ok);
    CHECK_FALSE(moonlive::compileSource("fill(0,0,255);", kT, nullptr, 0).ok);
}

TEST_CASE("MoonLive compiled setRGB writes one pixel; out-of-range is bounds-rejected") {
    uint8_t code[256];
    // in-range
    auto r = moonlive::compileSource("setRGB(3, 10, 20, 200);", kT, code, sizeof(code));
    REQUIRE(r.ok);
    void* blk = nullptr; auto fn = place(code, r.len, blk); REQUIRE(fn != nullptr);
    std::vector<uint8_t> buf(8 * 3, 0);
    fn(buf.data(), 8, 3);
    CHECK(buf[3*3] == 10); CHECK(buf[3*3+2] == 200);
    for (int i = 0; i < 8; i++) if (i != 3) CHECK(buf[i*3] == 0);
    platform::freeExec(blk, 256);

    // out-of-range index 99 on 8 lights → guarded, no write
    auto r2 = moonlive::compileSource("setRGB(99, 255, 255, 255);", kT, code, sizeof(code));
    REQUIRE(r2.ok);
    void* blk2 = nullptr; auto fn2 = place(code, r2.len, blk2); REQUIRE(fn2 != nullptr);
    std::vector<uint8_t> buf2(8 * 3, 0xAB);
    fn2(buf2.data(), 8, 3);
    for (auto v : buf2) CHECK(v == 0xAB);
    platform::freeExec(blk2, 256);
}
