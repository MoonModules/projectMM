// @module MoonLiveModifier
// @also MoonLive, ModifierBase, Layer

// A scripted modifier: the coordinate transform is a script the user edits live, not a C++ class
// that needs a rebuild and a reflash. This is the second binding of the MoonLive engine, and the
// tests below are mostly about the seam rather than the arithmetic — that the script's inputs
// really are the light's position, that a result really lands back in the mapping, and that a
// broken script degrades to a pass-through instead of taking the pipeline down.

#include "doctest.h"
#include "light/moonlive/MoonLiveModifier.h"
#include "platform/platform.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"
#include "light/layers/Layer.h"

#include <cstring>
#include <string>

using namespace mm;

namespace {
/// Run one coordinate through a modifier carrying `script`, and report where it landed.
Coord3D transform(const char* script, lengthType x, lengthType y, lengthType z,
                  lengthType w = 255, lengthType h = 255, lengthType d = 1) {
    MoonLiveModifier m;
    m.defineControls();
    if (script) m.setSource(script);
    m.prepare();
    Coord3D box{w, h, d};
    m.modifyLogicalSize(box);      // the Layer always does this before folding
    Coord3D pos{x, y, z};
    m.modifyLogical(pos);
    return pos;
}
}  // namespace

TEST_CASE("a scripted modifier mirrors the pattern, the way a hand-written one would") {
    // The default script. A mirror is the shape that makes a working binding obvious on a bench
    // strand — the pattern simply runs the other way.
    const Coord3D p = transform(nullptr, 10, 20, 0);
    CHECK(p.x == 244);          // width(255) - 1 - 10
    CHECK(p.y == 20);           // untouched axes stay put
    CHECK(p.z == 0);
}

TEST_CASE("the script reads the light's own position, not a fixed value") {
    // The whole seam in one assertion: `x` inside the script has to BE this light's x. If the
    // binding failed to write the input slots, every light would transform identically.
    const Coord3D a = transform("setXYZ(0, x, y, z);", 7, 3, 1);
    CHECK(a.x == 7);
    CHECK(a.y == 3);
    CHECK(a.z == 1);

    const Coord3D b = transform("setXYZ(0, x, y, z);", 200, 100, 2);
    CHECK(b.x == 200);
    CHECK(b.y == 100);
    CHECK(b.z == 2);
}

TEST_CASE("a script can swap axes, which is a transform no control could express") {
    const Coord3D p = transform("setXYZ(0, y, x, z);", 5, 60, 0);
    CHECK(p.x == 60);
    CHECK(p.y == 5);
}

TEST_CASE("a script can offset a coordinate, the scroll a modifier usually hard-codes") {
    const Coord3D p = transform("setXYZ(0, x + 4, y, z);", 10, 10, 0);
    CHECK(p.x == 14);
}

TEST_CASE("a broken script leaves the pattern alone rather than taking the layer down") {
    // Robustness, the hard rule: a user editing a script mid-show types something wrong at some
    // point. The coordinate passes through untransformed, the module carries the diagnostic, and
    // the pipeline keeps rendering.
    MoonLiveModifier m;
    m.defineControls();
    m.setSource("setXYZ(0, x, y");     // no closing paren, no semicolon
    m.prepare();
    Coord3D box{255, 255, 1};
    m.modifyLogicalSize(box);

    Coord3D pos{11, 22, 0};
    CHECK(m.modifyLogical(pos) == true);               // accepted, not rejected
    CHECK(pos.x == 11);                                // and unchanged
    CHECK(pos.y == 22);
    CHECK(m.severity() == MoonModule::Severity::Error);            // the reason is visible to the user
}

TEST_CASE("a coordinate too large for a script input passes through untransformed") {
    // A script input is one byte, so an axis beyond 255 cannot be handed to the script at all.
    // Passing it through unchanged is the honest degrade: wrapping it would silently place the
    // light somewhere it is not. The 16-bit element store that lifts this is backlogged.
    const Coord3D p = transform("setXYZ(0, 255 - x, y, z);", 300, 10, 0);
    CHECK(p.x == 300);                                 // untouched, not wrapped to 44
    CHECK(p.y == 10);
}

TEST_CASE("editing the script changes the transform without a rebuild of the firmware") {
    // The live-edit loop: the same module, a new script, a different mapping.
    MoonLiveModifier m;
    m.defineControls();
    m.prepare();
    Coord3D box{255, 255, 1};
    m.modifyLogicalSize(box);      // the Layer hands every modifier its box before folding

    Coord3D a{10, 20, 0};
    m.modifyLogical(a);
    CHECK(a.x == 244);                                 // the default mirror

    m.setSource("setXYZ(0, x, y, z);");
    m.prepare();

    Coord3D b{10, 20, 0};
    m.modifyLogical(b);
    CHECK(b.x == 10);                                  // now a pass-through
}


// Arithmetic is what makes a scripted modifier worth having: without it a script can only pass a
// coordinate through or swap two axes. These pin the operator set and, more importantly, that
// precedence is real — `2 + 3 * 4` silently giving 20 would corrupt every non-trivial transform.
TEST_CASE("a script computes with the usual precedence, so a transform means what it reads like") {
    // Multiplication binds tighter than addition.
    CHECK(transform("setXYZ(0, 2 + 3 * 4, y, z);", 0, 0, 0).x == 14);
    // Parentheses override it.
    CHECK(transform("setXYZ(0, (2 + 3) * 4, y, z);", 0, 0, 0).x == 20);
    // Subtraction, which no ISA here has an instruction for: a - b is emitted as a + (b * -1).
    CHECK(transform("setXYZ(0, 100 - 40, y, z);", 0, 0, 0).x == 60);
    // Left-associative, so 100 - 40 - 20 is 40 rather than 80.
    CHECK(transform("setXYZ(0, 100 - 40 - 20, y, z);", 0, 0, 0).x == 40);
    // The coordinate inputs compose with all of it — this is the shape a real modifier uses.
    CHECK(transform("setXYZ(0, x * 2 + 1, y, z);", 10, 0, 0).x == 21);
}

TEST_CASE("a scaled mirror, the transform this binding exists to make possible") {
    // Two operators and an input in one expression: reflect, then halve. Expressible now, and not
    // expressible at all before arithmetic landed.
    const Coord3D p = transform("setXYZ(0, (255 - x) * 2, y, z);", 100, 5, 0);
    CHECK(p.x == 54);      // (255-100)*2 = 310, truncated into the byte the input slot holds
    CHECK(p.y == 5);
}

// The cost question this binding raises: modifyLogical is a native CALL per light, so a large
// fixture pays it once per light on every mapping rebuild. It is the cold path (a rebuild, not a
// frame), but "cold" is not a licence to be slow — a 16k-light wall rebuilding must not stall.
TEST_CASE("transforming a wall's worth of lights stays within a rebuild's budget") {
    MoonLiveModifier m;
    m.defineControls();
    m.setSource("setXYZ(0, 255 - x, y, z);");
    m.prepare();

    constexpr int kLights = 16384;          // the 128x128 wall
    const uint32_t t0 = platform::micros();
    for (int i = 0; i < kLights; i++) {
        Coord3D pos{static_cast<lengthType>(i & 0xFF), static_cast<lengthType>((i >> 8) & 0xFF), 0};
        m.modifyLogical(pos);
    }
    const uint32_t us = platform::micros() - t0;
    MESSAGE("16384 scripted transforms took " << us << " us (" << (us * 1000.0 / kLights) << " ns each)");
    // A rebuild already walks every light several times; adding a script call must stay in the
    // same order, not become the thing that dominates. 50 ms is far above any measurement here and
    // still imperceptible for a one-off rebuild — it catches an accidental per-call compile.
    CHECK(us < 50000);
}

// Editing a script has to CHANGE WHAT IS ON SCREEN, which is not the same as recompiling.
// modifyLogical is the static hook: it runs while the Layer builds its mapping, so a Layer that is
// never told to rebuild keeps serving the mapping it made from the previous script. The symptom is
// the worst kind — everything reports success, the new code is compiled and resident, and the
// preview simply does not move.
TEST_CASE("editing a script asks the layer to rebuild, so the change is visible") {
    MoonLiveModifier m;
    m.defineControls();
    m.prepare();
    CHECK(m.consumeNeedsRebuild() == true);    // the first compile needs one too
    CHECK(m.consumeNeedsRebuild() == false);   // and it is consumed, not sticky

    m.setSource("setXYZ(0, y, x, z);");
    m.prepare();
    CHECK(m.consumeNeedsRebuild() == true);    // an edit asks again
}

// A script edit has to reach the LAYER, not just the engine. `modifyLogical` is the static hook: it
// runs while the Layer builds its mapping, so recompiling alone changes nothing on screen — the
// Layer keeps serving the mapping it built from the previous script. That failure is the worst kind
// to diagnose from the outside: the compile succeeds, the new code is resident and correct, the
// module reports no error, and the preview simply does not move.
//
// This pins the request. The fold itself is verified by the direct modifyLogical cases above, the
// same way every other modifier in this codebase is tested — building a whole Layer to re-check the
// Layer's own fold would be testing Layer, not this module.
TEST_CASE("editing a script asks the layer to rebuild its mapping") {
    MoonLiveModifier m;
    m.defineControls();
    m.setSource("setXYZ(0, x, y, z);");
    m.prepare();
    CHECK(m.consumeNeedsRebuild() == true);     // the first compile needs one too
    CHECK(m.consumeNeedsRebuild() == false);    // and it is consumed, not sticky

    m.setSource("setXYZ(0, 7 - x, y, z);");
    m.prepare();
    CHECK(m.consumeNeedsRebuild() == true);     // an edit asks again

    // And the new script really is what runs now — the mapping the Layer rebuilds will use this.
    Coord3D pos{0, 0, 0};
    m.modifyLogical(pos);
    CHECK(pos.x == 7);
}

// The black-screen bug, pinned. A mirror written against a hard-coded 255 sends every light of a
// 16-wide grid to x≈240, the Layer drops each one as out of bounds, and the fixture goes dark —
// with no error anywhere, because the script compiled and ran perfectly. A script therefore has to
// be able to read the EXTENT it is folding within, and the default has to use it.
TEST_CASE("the default script mirrors within the grid it is given, not a fixed 255") {
    // A 16-wide grid: x=0 must land on the far end of THAT grid, 15 — not 245.
    const Coord3D p = transform(nullptr, 0, 0, 0, /*w=*/16, /*h=*/16, /*d=*/1);
    CHECK(p.x == 15);
    CHECK(p.y == 0);

    // Every coordinate has to stay inside the box, or the Layer discards it.
    for (lengthType i = 0; i < 16; i++) {
        const Coord3D q = transform(nullptr, i, 0, 0, 16, 16, 1);
        CAPTURE(i);
        CHECK(q.x >= 0);
        CHECK(q.x < 16);
    }
}

TEST_CASE("a script can read the grid extent it is folding within") {
    CHECK(transform("setXYZ(0, width, y, z);",  0, 0, 0, 32, 16, 1).x == 32);
    CHECK(transform("setXYZ(0, height, y, z);", 0, 0, 0, 32, 16, 1).x == 16);
}

// The black-screen failure end to end, through a real Layer. Byte arithmetic wraps: a script that
// computes a negative x (a mirror against a box that has not been established, or simply a bad
// expression) lands at 255, which is outside every real grid — so the Layer discards every light
// and the fixture goes dark with no error reported anywhere. The fold has to REJECT a coordinate it
// cannot place, which is what modifyLogical's bool return is for.
TEST_CASE("a script that computes a position outside the grid does not black out the fixture") {
    Layouts layouts;
    auto* grid = new GridLayout();
    grid->width = 16; grid->height = 16; grid->depth = 1;
    layouts.addChild(grid);

    Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    auto* mod = new MoonLiveModifier();
    mod->defineControls();
    mod->setSource("setXYZ(0, x + 200, y, z);");   // deliberately off the end of a 16-wide grid
    layer.addChild(mod);
    layouts.applyState();
    layer.applyState();

    // Render and count what reaches the buffer. A wholly dark frame is the bug.
    uint8_t* buf = const_cast<uint8_t*>(layer.buffer().data());
    std::memset(buf, 0, layer.buffer().bytes());
    draw::Canvas cv = draw::Canvas::of(layer.buffer(), layer.width(), layer.height(), layer.depth());
    draw::fill(cv, RGB{255, 255, 255});
    int lit = 0;
    for (nrOfLightsType i = 0; i < layer.buffer().count(); i++) if (buf[i * 3]) lit++;
    INFO("lit lights: " << lit << " of " << layer.buffer().count());
    CHECK(lit > 0);
}





// The bug that made the fixture render NOTHING, and the reason it was so hard to see: every part
// worked in isolation. The script compiled, the fold produced correct coordinates, and every light
// was accepted — but the mapping was being rebuilt on EVERY frame, so the pipeline never got to
// draw. The cause is a cycle: prepare() asked for a rebuild, the Layer's rebuild IS applyState(),
// and applyState() calls prepare() again. A rebuild must therefore be requested only when the
// script actually changed.
TEST_CASE("re-preparing with an unchanged script does not ask for another rebuild") {
    MoonLiveModifier m;
    m.defineControls();
    m.prepare();
    CHECK(m.consumeNeedsRebuild() == true);    // the first compile needs one

    // What the Layer does when it honours that request: applyState() → prepare(). If this asks
    // again, the two call each other forever and nothing ever renders.
    m.prepare();
    CHECK(m.consumeNeedsRebuild() == false);
    m.prepare();
    CHECK(m.consumeNeedsRebuild() == false);

    // A real edit still asks.
    m.setSource("setXYZ(0, y, x, z);");
    m.prepare();
    CHECK(m.consumeNeedsRebuild() == true);
}

// print(v) is the only way to see inside a running script: one that compiles cleanly and renders
// wrong gives no other clue. It returns its argument so it can wrap any sub-expression without
// changing the result — `print(x)` where `x` stood still computes x.
TEST_CASE("print reports a value without changing what the script computes") {
    // Wrapping the coordinate in print() must leave the transform identical.
    CHECK(transform("setXYZ(0, print(x), y, z);", 7, 3, 0, 16, 16, 1).x == 7);
    // And it composes inside arithmetic.
    CHECK(transform("setXYZ(0, print(width - 1 - x), y, z);", 0, 0, 0, 16, 16, 1).x == 15);
}
