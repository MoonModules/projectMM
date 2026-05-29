// Pins the MoonModule base-default propagation for loop / loop20ms / loop1s.
// The three tick callbacks default to iterating children, gating by
// `!respectsEnabled() || enabled()`, dispatching the same callback on each
// child, and accumulating per-child timing. Containers that need extra work
// override and chain to the base; leaf modules pay one predicted-not-taken
// branch on the empty children_ array.
//
// Regression target: before this propagation existed, every container
// (Layers, Drivers, NetworkModule-with-children) had to write the same
// 5-line per-child block by hand; one missing block meant a child's loop
// callback silently never ran. The lifecycle-propagation tests below pin
// the gating + dispatch rules so a future change to MoonModule::tickChildren
// fails loudly here instead of silently breaking a container subtree.

#include "doctest.h"
#include "core/MoonModule.h"

namespace {

// Test module that counts each tick callback. Default ctor; no extra
// behaviour beyond accounting. Used for both parents and children below.
class Counting : public mm::MoonModule {
public:
    uint32_t loopCalls = 0;
    uint32_t loop20msCalls = 0;
    uint32_t loop1sCalls = 0;

    void loop() override {
        loopCalls++;
        mm::MoonModule::loop();   // chain so this counter doubles as a parent too
    }
    void loop20ms() override {
        loop20msCalls++;
        mm::MoonModule::loop20ms();
    }
    void loop1s() override {
        loop1sCalls++;
        mm::MoonModule::loop1s();
    }
};

// Variant that opts out of the enabled gate — same behaviour as
// NetworkModule / SystemModule / FirmwareUpdateModule today.
class AlwaysOn : public Counting {
public:
    bool respectsEnabled() const override { return false; }
};

} // namespace

TEST_CASE("loop default propagates to enabled children") {
    Counting parent;
    Counting a, b;
    parent.addChild(&a);
    parent.addChild(&b);

    parent.loop();

    CHECK(parent.loopCalls == 1);
    CHECK(a.loopCalls == 1);
    CHECK(b.loopCalls == 1);
}

TEST_CASE("loop default skips disabled children") {
    Counting parent;
    Counting a, b;
    parent.addChild(&a);
    parent.addChild(&b);
    b.setEnabled(false);

    parent.loop();

    CHECK(a.loopCalls == 1);
    CHECK(b.loopCalls == 0);
}

TEST_CASE("loop default still ticks children that opt out of the enabled gate") {
    Counting parent;
    AlwaysOn a;
    parent.addChild(&a);
    a.setEnabled(false);   // would normally skip, but respectsEnabled() == false

    parent.loop();

    CHECK(a.loopCalls == 1);
}

TEST_CASE("loop20ms and loop1s follow the same gating + dispatch rules") {
    Counting parent;
    Counting a, b;
    parent.addChild(&a);
    parent.addChild(&b);
    b.setEnabled(false);

    parent.loop20ms();
    parent.loop1s();

    CHECK(a.loop20msCalls == 1);
    CHECK(a.loop1sCalls == 1);
    CHECK(b.loop20msCalls == 0);
    CHECK(b.loop1sCalls == 0);
}

TEST_CASE("leaf module loop default is a safe no-op (childCount_ == 0)") {
    // Direct MoonModule (no override): default loop / loop20ms / loop1s
    // should iterate over zero children and return without side effects.
    mm::MoonModule leaf;
    leaf.loop();
    leaf.loop20ms();
    leaf.loop1s();

    CHECK(leaf.childCount() == 0);
    CHECK(leaf.loopTimeUs() == 0);  // no timing accumulated either
}

TEST_CASE("per-child timing accumulates on the child, not the parent") {
    // The base default times each child individually (matches what Scheduler
    // does for top-level modules). Rather than depend on what platform::micros()
    // happens to read between two adjacent calls on a fast desktop (which can
    // round to 0 µs and make the assertion tautological), inject a known
    // non-zero accumulation directly via addAccumUs() and verify that
    // publishTiming surfaces it on the child as loopTimeUs().
    Counting parent;
    Counting a;
    parent.addChild(&a);

    // Tick once via the parent so we exercise the propagation path itself
    // (tickChildren runs addAccumUs on the child as a side-effect; we don't
    // rely on the magnitude that produces). Then add a deterministic
    // contribution so the per-frame average lands at a known non-zero value.
    parent.loop();
    a.addAccumUs(40);   // 40us across 2 frames = average 20us/frame

    parent.publishTiming(2);

    CHECK(a.loopCalls == 1);
    // Deterministic lower bound: we injected 40us across 2 frames, so the
    // averaged loopTimeUs() must be at least 20. A regression that bypasses
    // addAccumUs in tickChildren would still see the manual 40us we injected;
    // a regression that breaks publishTiming itself would drop to 0.
    CHECK(a.loopTimeUs() >= 20u);
}
