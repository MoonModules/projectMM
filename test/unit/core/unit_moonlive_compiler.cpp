// @module MoonLive

#include "doctest.h"
#include "core/moonlive/MoonLiveCompiler.h"
#include "core/moonlive/MoonLive.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
#include <algorithm>

// MoonLive front-end: an expression grammar where any argument may be a nested call, and the
// functions (setRGB / fill / random16) are resolved against a host-registered BuiltinTable
// (the light domain's). The core compiler owns no LED vocabulary — these tests drive it
// through the light table the same way the binding does.

using namespace mm;

static moonlive::BuiltinTable kTable = moonlive::lightBuiltins();

// Compile + run a source on a w-light, 3-channel buffer; returns the rendered buffer.
static std::vector<uint8_t> render(const char* src, int nLights, uint32_t t = 0) {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile(src, kTable));
    REQUIRE(eng.ok());
    std::vector<uint8_t> buf(nLights * 3, 0);
    eng.run(buf.data(), nLights, 3, t);
    return buf;
}

TEST_CASE("compileSource: fill(r,g,b) fills every light") {
    auto buf = render("fill(10, 20, 200);", 8);
    for (int i = 0; i < 8; i++) { CHECK(buf[i*3]==10); CHECK(buf[i*3+1]==20); CHECK(buf[i*3+2]==200); }
}

TEST_CASE("compileSource: setRGB(index, r,g,b) writes one pixel") {
    auto buf = render("setRGB(3, 255, 0, 0);", 8);
    for (int i = 0; i < 8; i++) {
        uint8_t want = (i == 3) ? 255 : 0;
        CHECK(buf[i*3] == want);
    }
}

// REMARK #1: every argument is an expression — random16 in ANY slot.
TEST_CASE("compileSource: random16 works in any argument slot") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("setRGB(random16(8), random16(256), 30, 0);", kTable));
    REQUIRE(eng.ok());
    for (int run = 0; run < 32; run++) {
        std::vector<uint8_t> buf(8 * 3, 0);
        eng.run(buf.data(), 8, 3, 0);
        int lit = 0;
        for (int i = 0; i < 8; i++) if (buf[i*3] || buf[i*3+1] || buf[i*3+2]) lit++;
        CHECK(lit == 1);   // one random pixel, with a random red — exactly one lit
    }
}

// REMARK #2: a literal / random16 bound may be a uint16 (0..65535), not capped at 255.
TEST_CASE("compileSource: random16 accepts a uint16 bound (>255)") {
    moonlive::MoonLive eng;
    CHECK(eng.compile("setRGB(random16(65535), 0, 0, 255);", kTable));   // 65535 accepted
    CHECK(eng.compile("setRGB(1000, 0, 0, 255);", kTable));              // literal index > 255 ok
    uint8_t out[256];
    auto r = moonlive::compileSource("setRGB(70000, 0, 0, 0);", kTable, out, sizeof(out));
    CHECK_FALSE(r.ok);   // 70000 > 65535 → rejected
}

TEST_CASE("compileSource: out-of-range index is bounds-rejected at runtime") {
    auto buf = render("setRGB(5000, 255, 255, 255);", 8);   // 5000 >> 8 lights
    for (auto v : buf) CHECK(v == 0);                       // guarded — nothing written
}

TEST_CASE("compileSource rejects malformed programs with a diagnostic, never crashes") {
    uint8_t out[256];
    // Each of these MUST fail — assert it, so an accidental successful compile is caught (not
    // just "no crash"). Wrong arity, unknown name, unbalanced parens, trailing junk, empty.
    const char* bad[] = {
        "",                                  // empty
        "setRGB(0,0,0,0,0);",                // too many args
        "fill(0,0);",                        // too few args
        "wibble(1);",                        // unknown function
        "setRGB(0, 0, 0",                    // missing ')'  and ';'
        "fill(0,0,0)",                       // missing ';'
        "fill(0,0,0); extra",                // trailing junk
        "setRGB(random8(8), 0, 0, 0);",      // unknown nested function
    };
    for (auto s : bad) {
        auto r = moonlive::compileSource(s, kTable, out, sizeof(out));
        CHECK_FALSE(r.ok);                   // the parser contract: malformed → rejected
        CHECK(std::strlen(r.error) > 0);     // …with a diagnostic
    }
    // A value-returning function used as a void statement IS valid (result discarded).
    CHECK(moonlive::compileSource("random16(8);", kTable, out, sizeof(out)).ok);
}

TEST_CASE("MoonLive.compile(source) on a bad script leaves the engine !ok with an error") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile("setRGB(oops);", kTable));
    CHECK_FALSE(eng.ok());
    CHECK(std::strlen(eng.error()) > 0);
    std::vector<uint8_t> buf(3, 0xAB);
    eng.run(buf.data(), 1, 3, 0);
    CHECK(buf[0] == 0xAB);
}

// VREG REUSE: a chain of calls must fit the small device register file. Each argument temp dies
// once its call consumes it and is recycled, so peak register pressure stays low no matter how
// many calls a statement nests — setRGB with all four arguments a random16 still compiles.
TEST_CASE("a multi-call statement reuses dead vregs and stays within the register budget") {
    moonlive::BuiltinTable t = moonlive::lightBuiltins();
    uint8_t out[768];
    for (const char* s : {
            "setRGB(random16(64), random16(256), 30, 0);",                          // 2 calls
            "setRGB(random16(128), random16(256), random16(256), 0);",              // 3 calls
            "setRGB(random16(128), random16(256), random16(256), random16(256));",  // 4 calls
         }) {
        auto r = moonlive::compileSource(s, t, out, sizeof(out));
        CHECK(r.ok);          // without vreg reuse the 3-/4-call cases overflow the register file
        CHECK(r.len > 0);
    }
}

// DOMAIN-NEUTRAL: the core compiler owns no function names. With an EMPTY table it knows
// nothing — `setRGB`/`fill`/`random16` are all "unknown function". The LED vocabulary lives
// only in the host's table; a different host registers different names. (Remark #3.)
TEST_CASE("core compiler has no built-in functions of its own (empty table → all unknown)") {
    moonlive::BuiltinTable empty;
    uint8_t out[256];
    for (const char* s : {"setRGB(0,0,0,0);", "fill(0,0,0);", "random16(8);"}) {
        auto r = moonlive::compileSource(s, empty, out, sizeof(out));
        CHECK_FALSE(r.ok);                       // the core doesn't know any of these
        CHECK(std::strlen(r.error) > 0);
    }
    // A host can register an arbitrary name against the same neutral machinery.
    moonlive::BuiltinTable custom;
    custom.add({"paint", 4, false, moonlive::BuiltinKind::Inline, nullptr, moonlive::InlineOp::StoreElem});
    auto r = moonlive::compileSource("paint(2, 9, 8, 7);", custom, out, sizeof(out));
    CHECK(r.ok);                                 // a different name, same core path
}

TEST_CASE("MoonLive recompiling swaps the program live (fill <-> setRGB)") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("fill(0,0,255);", kTable));
    std::vector<uint8_t> buf(4 * 3, 0);
    eng.run(buf.data(), 4, 3, 0);
    CHECK(buf[0*3+2] == 255); CHECK(buf[3*3+2] == 255);

    REQUIRE(eng.compile("setRGB(1, 255, 0, 0);", kTable));
    std::fill(buf.begin(), buf.end(), 0);
    eng.run(buf.data(), 4, 3, 0);
    CHECK(buf[1*3+0] == 255); CHECK(buf[0] == 0);
}

// STAGE 1 CONTROLS — parse layer: a `uint8_t name = def; // @control min..max` declaration
// surfaces a DeclaredControl, and a declared name used in a statement resolves to it.
TEST_CASE("compileSource: a control declaration surfaces a DeclaredControl") {
    uint8_t out[768];
    auto r = moonlive::compileSource(
        "uint8_t speed = 50; // @control 0..99\nsetRGB(speed, 0, 0, 255);", kTable, out, sizeof(out));
    REQUIRE(r.ok);
    REQUIRE(r.controlCount == 1);
    const auto& c = r.controls[0];
    CHECK(std::strncmp(c.name, "speed", c.nameLen) == 0);
    CHECK(c.nameLen == 5);
    CHECK(c.min == 0); CHECK(c.max == 99); CHECK(c.def == 50); CHECK(c.offset == 0);
    CHECK(c.type == moonlive::CtrlType::Uint8);

    // No annotation → default 0..255; two controls get sequential offsets.
    auto r2 = moonlive::compileSource(
        "uint8_t a = 10;\nuint8_t b = 20; // @control 1..7\nsetRGB(a, b, 0, 0);", kTable, out, sizeof(out));
    REQUIRE(r2.ok);
    REQUIRE(r2.controlCount == 2);
    CHECK(r2.controls[0].max == 255); CHECK(r2.controls[0].offset == 0);   // a: no anno
    CHECK(r2.controls[1].min == 1); CHECK(r2.controls[1].max == 7); CHECK(r2.controls[1].offset == 1);
}

TEST_CASE("compileSource: malformed control declarations fail with a diagnostic, never crash") {
    uint8_t out[768];
    const char* bad[] = {
        "uint8_t speed 50; setRGB(0,0,0,0);",                        // missing '='
        "uint8_t speed = 300; setRGB(0,0,0,0);",                     // default > 255
        "uint8_t speed = 50; // @control 99..0\nsetRGB(0,0,0,0);",   // reversed range
        "uint8_t speed = 50;",                                       // no statement
        "uint8_t = 50; setRGB(0,0,0,0);",                            // no name
        "uint8_t s = 1; uint8_t s = 2; setRGB(0,0,0,0);",            // duplicate name
    };
    for (auto s : bad) {
        auto r = moonlive::compileSource(s, kTable, out, sizeof(out));
        CHECK_FALSE(r.ok);
        CHECK(std::strlen(r.error) > 0);
    }
}
