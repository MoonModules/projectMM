// Layer buffer persistence + the collected per-frame fade (fadeToBlackBy).
//
// The Layer does NOT clear its buffer each frame (FastLED/WLED/MoonLight model): the buffer holds the
// previous frame so effects can fade it for trails or read prior pixels. Trail effects call
// layer->fadeToBlackBy(amt); the Layer collects the amount (MIN across effects) and applies ONE fade
// pass at the start of the next frame. These cases pin: (1) persistence — a pixel written one frame is
// still there the next; (2) fadeToBlackBy decays the persisted buffer once per frame; (3) MIN combine
// when several effects request a fade; (4) the collected amount resets after it is consumed.

#include "doctest.h"
#include "light/layers/Layers.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/effects/EffectBase.h"
#include "light/draw.h"

namespace {

// A test effect that writes one red pixel at (0,0) only on its FIRST loop, then does nothing —
// so any red still present on later frames proves the buffer persisted (was not cleared).
struct WriteOnceEffect : mm::EffectBase {
    int calls = 0;
    const char* tags() const override { return ""; }
    mm::Dim dimensions() const override { return mm::Dim::D3; }
    void loop() override {
        if (calls++ == 0) {
            mm::Buffer& b = layer()->buffer();
            mm::Coord3D dims{width(), height(), depth()};
            mm::draw::pixel(b, dims, {0, 0, 0}, {255, 0, 0});
        }
    }
};

// A test effect that requests a fade of `amt` every frame and never writes — so it only decays
// whatever the buffer already holds.
struct FadeOnlyEffect : mm::EffectBase {
    uint8_t amt = 0;
    const char* tags() const override { return ""; }
    mm::Dim dimensions() const override { return mm::Dim::D3; }
    void loop() override { layer()->fadeToBlackBy(amt); }
};

struct Scene {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    Scene(int w, int h) {
        grid.width = w; grid.height = h; grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
    }
};

}  // namespace

TEST_CASE("Layer: buffer persists across frames (no per-frame clear)") {
    Scene s(4, 4);
    WriteOnceEffect once;
    s.layer.addChild(&once);
    s.layer.onBuildState();

    s.layer.loop();                              // frame 0: writes red at (0,0)
    CHECK(s.layer.buffer().data()[0] == 255);
    s.layer.loop();                              // frame 1: writes nothing
    // The pixel is STILL there — the Layer did not wipe it. (An auto-clear would show 0 here.)
    CHECK(s.layer.buffer().data()[0] == 255);
    s.layer.loop();                              // frame 2
    CHECK(s.layer.buffer().data()[0] == 255);
}

TEST_CASE("Layer: fadeToBlackBy decays the persisted buffer once per frame") {
    Scene s(4, 4);
    WriteOnceEffect once;
    FadeOnlyEffect fade; fade.amt = 128;          // ~half each frame
    s.layer.addChild(&once);
    s.layer.addChild(&fade);
    s.layer.onBuildState();

    s.layer.loop();                              // frame 0: once writes 255; fade collected for next frame
    CHECK(s.layer.buffer().data()[0] == 255);    // not faded yet (consume is at NEXT frame start)
    s.layer.loop();                              // frame 1: consume fade (255 → ~127), once writes nothing
    const uint8_t after1 = s.layer.buffer().data()[0];
    CHECK(after1 < 255);
    CHECK(after1 > 0);
    s.layer.loop();                              // frame 2: fade again
    CHECK(s.layer.buffer().data()[0] < after1);  // strictly darker — it keeps decaying
}

TEST_CASE("Layer: multiple fade requests combine with MIN (gentlest wins, longest trail)") {
    Scene s(4, 4);
    WriteOnceEffect once;
    FadeOnlyEffect gentle; gentle.amt = 8;        // long trail
    FadeOnlyEffect harsh;  harsh.amt = 200;       // short trail
    s.layer.addChild(&once);
    s.layer.addChild(&gentle);
    s.layer.addChild(&harsh);
    s.layer.onBuildState();

    s.layer.loop();                              // frame 0: 255 written; both fades collected → MIN = 8
    s.layer.loop();                              // frame 1: consume MIN(8,200)=8 → keep = 247/255 of 255
    const uint8_t v = s.layer.buffer().data()[0];
    // With the gentle amount (8) the pixel stays near full; the harsh 200 would have crushed it to ~55.
    CHECK(v > 230);                              // proves MIN (gentle) won, not MAX/AVG
}

TEST_CASE("Layer: collected fade resets after it is consumed") {
    Scene s(4, 4);
    WriteOnceEffect once;
    // A fade effect that requests a fade only on the FIRST frame, then stops.
    struct OnceFade : mm::EffectBase {
        int n = 0;
        const char* tags() const override { return ""; }
        mm::Dim dimensions() const override { return mm::Dim::D3; }
        void loop() override { if (n++ == 0) layer()->fadeToBlackBy(128); }
    } oneFade;
    s.layer.addChild(&once);
    s.layer.addChild(&oneFade);
    s.layer.onBuildState();

    s.layer.loop();                              // frame 0: 255 written, fade(128) collected
    s.layer.loop();                              // frame 1: consume once → ~127, no new fade requested
    const uint8_t after1 = s.layer.buffer().data()[0];
    s.layer.loop();                              // frame 2: NO fade pending → value must hold, not decay again
    CHECK(s.layer.buffer().data()[0] == after1); // stable: the collected amount did not linger
}
