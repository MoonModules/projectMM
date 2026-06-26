// @module MoonLive

#include "doctest.h"
#include "core/moonlive/MoonLiveCompiler.h"
#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLive.h"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>

// MoonLive Stage 2: the minimal real front-end (lex → parse → codegen) for one statement,
// `fill(r,g,b);`. The headline property is the GOLDEN-BYTES EQUIVALENCE — parsing the source
// produces byte-for-byte the same machine code the hand-written emitFill produces, so the
// front-end provably introduces no codegen of its own (it just drives the same emitter). The
// rest pins the parser's diagnostics, the no-language-leak proof at its cheapest.

using namespace mm;

TEST_CASE("compileSource emits the SAME bytes as the hand-written emitFill (golden)") {
    const uint8_t cases[][3] = {{0, 0, 255}, {10, 20, 200}, {255, 255, 255}, {0, 0, 0}, {1, 2, 3}};
    for (auto& c : cases) {
        char src[64];
        std::snprintf(src, sizeof(src), "fill(%u, %u, %u);", c[0], c[1], c[2]);

        uint8_t fromSource[256];
        auto r = moonlive::compileSource(src, fromSource, sizeof(fromSource));
        REQUIRE(r.ok);
        REQUIRE(r.len > 0);

        uint8_t golden[256];
        size_t glen = moonlive::emitFill(golden, sizeof(golden), c[0], c[1], c[2]);
        REQUIRE(glen == r.len);
        CHECK(std::memcmp(fromSource, golden, glen) == 0);   // byte-for-byte identical
    }
}

TEST_CASE("compileSource tolerates whitespace and no spaces") {
    uint8_t a[256], b[256];
    auto ra = moonlive::compileSource("fill(0,0,255);", a, sizeof(a));
    auto rb = moonlive::compileSource("  fill (  0 ,0,  255 )  ;  ", b, sizeof(b));
    REQUIRE(ra.ok); REQUIRE(rb.ok);
    REQUIRE(ra.len == rb.len);
    CHECK(std::memcmp(a, b, ra.len) == 0);
}

TEST_CASE("compileSource rejects malformed programs with a diagnostic, never crashes") {
    uint8_t out[256];
    struct Case { const char* src; };
    const Case bad[] = {
        {""},                       // empty
        {"fil(0,0,255);"},          // wrong name
        {"fill 0,0,255);"},         // missing (
        {"fill(0,0);"},             // too few args
        {"fill(0,0,255,9);"},       // too many args
        {"fill(0,0,300);"},         // out of range
        {"fill(0,x,255);"},         // non-number
        {"fill(0,0,255)"},          // missing ;
        {"fill(0,0,255); extra"},   // trailing junk
        {"fill(0,0,255);;"},        // extra ;
        {"@#$"},                    // garbage
    };
    for (auto& c : bad) {
        auto r = moonlive::compileSource(c.src, out, sizeof(out));
        CHECK_FALSE(r.ok);
        CHECK(std::strlen(r.error) > 0);   // a human-readable message, not empty
    }
}

TEST_CASE("compileSource reports a too-small code buffer (degrades)") {
    uint8_t tiny[2];
    auto r = moonlive::compileSource("fill(0,0,255);", tiny, sizeof(tiny));
    CHECK_FALSE(r.ok);
}

TEST_CASE("MoonLive.compile(source) compiles and runs the parsed program") {
    moonlive::MoonLive engine;
    REQUIRE(engine.compile("fill(0, 0, 255);"));   // blue, from source text
    REQUIRE(engine.ok());

    std::vector<uint8_t> buf(4 * 3, 0xAB);
    engine.run(buf.data(), 4, 3, /*t*/ 0);
    for (int i = 0; i < 4; i++) {
        CHECK(buf[i*3 + 0] == 0);
        CHECK(buf[i*3 + 1] == 0);
        CHECK(buf[i*3 + 2] == 255);
    }
}

TEST_CASE("MoonLive.compile(source) on a bad script leaves the engine !ok with an error") {
    moonlive::MoonLive engine;
    CHECK_FALSE(engine.compile("fill(oops);"));
    CHECK_FALSE(engine.ok());
    CHECK(std::strlen(engine.error()) > 0);
    // run() after a failed compile is a safe no-op.
    std::vector<uint8_t> buf(3, 0xAB);
    engine.run(buf.data(), 1, 3, 0);
    CHECK(buf[0] == 0xAB);   // untouched — nothing rendered
}

TEST_CASE("MoonLive recompiling a new source swaps the program live") {
    moonlive::MoonLive engine;
    REQUIRE(engine.compile("fill(255, 0, 0);"));   // red
    std::vector<uint8_t> buf(3, 0);
    engine.run(buf.data(), 1, 3, 0);
    CHECK(buf[0] == 255); CHECK(buf[2] == 0);

    REQUIRE(engine.compile("fill(0, 0, 255);"));   // recompile -> blue
    engine.run(buf.data(), 1, 3, 0);
    CHECK(buf[0] == 0); CHECK(buf[2] == 255);
}
