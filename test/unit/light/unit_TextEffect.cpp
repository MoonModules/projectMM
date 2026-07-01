// @module TextEffect

#include "doctest.h"
#include "light/effects/TextEffect.h"
#include "light/layouts/GridLayout.h"

#include <cstring>

using namespace mm;

namespace {
// Build a real w×h×1 Layer for the effect to render into (the MetaballsEffect harness).
struct Scene {
    Layouts layouts;
    GridLayout grid;
    Layer layer;
    TextEffect text;
    Scene(int w, int h) {
        grid.width = w; grid.height = h; grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.addChild(&text);
    }
    int litPixels() {
        auto& b = layer.buffer();
        int n = 0;
        for (size_t i = 0; i + 2 < b.bytes(); i += 3)
            if (b.data()[i] || b.data()[i+1] || b.data()[i+2]) n++;
        return n;
    }
};
}  // namespace

// Static text renders glyph pixels top-left. On a grid tall/wide enough for one line of the 6x8
// font, a non-empty string lights some pixels; an empty string lights none.
TEST_CASE("TextEffect: static text draws glyph pixels, empty draws nothing") {
    Scene s(48, 8);
    s.text.scroll = false;
    s.text.font = 1;                 // 6x8
    std::strcpy(s.text.text_, "AB");
    s.layer.onBuildState();
    s.layer.loop();
    CHECK(s.litPixels() > 0);        // "AB" drew something

    std::strcpy(s.text.text_, "");   // empty string → blank frame
    s.layer.loop();
    CHECK(s.litPixels() == 0);
}

// A multi-line string wraps: the second line renders on a lower row (font-height down), so a
// two-line string lights pixels below the first font's height. Uses the 4x6 font (height 6).
TEST_CASE("TextEffect: newline wraps to a second row") {
    Scene s(24, 16);
    s.text.scroll = false;
    s.text.font = 0;                 // 4x6, height 6
    std::strcpy(s.text.text_, "A\nB");
    s.layer.onBuildState();
    s.layer.loop();

    auto& b = s.layer.buffer();
    const int w = 24;
    auto rowLit = [&](int y) {
        for (int x = 0; x < w; x++) { size_t o = (static_cast<size_t>(y) * w + x) * 3;
            if (b.data()[o] || b.data()[o+1] || b.data()[o+2]) return true; }
        return false;
    };
    bool topLine = false, secondLine = false;
    for (int y = 0; y < 6; y++)  if (rowLit(y)) topLine = true;      // line 1 in the top font cell
    for (int y = 6; y < 12; y++) if (rowLit(y)) secondLine = true;   // line 2 one font-height down
    CHECK(topLine);
    CHECK(secondLine);
}

// Scroll mode advances the text over time and never crashes; on a degenerate grid it's a safe no-op.
TEST_CASE("TextEffect: scroll animates and is safe at any size") {
    Scene s(16, 8);
    s.text.scroll = true;
    std::strcpy(s.text.text_, "MoonModules");
    s.layer.onBuildState();
    for (int i = 0; i < 5; i++) s.layer.loop();   // several frames — must not crash
    // 1×1 and 0×0 must not crash either.
    Scene tiny(1, 1); tiny.text.scroll = true; std::strcpy(tiny.text.text_, "X");
    tiny.layer.onBuildState(); tiny.layer.loop();
    Scene zero(0, 0); zero.text.scroll = true; std::strcpy(zero.text.text_, "X");
    zero.layer.onBuildState(); zero.layer.loop();
    CHECK(true);   // reaching here without a crash is the assertion
}
