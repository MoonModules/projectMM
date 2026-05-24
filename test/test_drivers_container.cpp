#include "doctest.h"
#include "light/drivers/Drivers.h"

// Regression: the UI's enable/disable toggle on a child driver (e.g. ArtNet,
// Preview) was a no-op — the driver kept running. Cause: Drivers::loop() called
// child(i)->loop() unconditionally, skipping the per-child `enabled` check that
// Layer::loop() does for effects and Layers::loop() does for child Layers.
// (The Scheduler only walks top-level modules, so it never sees these children.)
//
// These tests pin the gate so the regression can't return silently. A stub
// driver counts its loop calls; toggling `enabled` must flip whether the count
// advances.

namespace {

// Minimal DriverBase stub: counts loop calls. Ignores the source buffer it
// would normally consume — this test cares only about whether loop() runs.
class CountingDriver : public mm::DriverBase {
public:
    void setSourceBuffer(mm::Buffer*) override {}
    void loop() override { loopCalls++; }
    int loopCalls = 0;
};

} // namespace

TEST_CASE("Drivers::loop() skips disabled child drivers") {
    mm::Drivers drivers;
    CountingDriver a, b;
    drivers.addChild(&a);
    drivers.addChild(&b);

    // Both enabled by default → both tick.
    drivers.loop();
    CHECK(a.loopCalls == 1);
    CHECK(b.loopCalls == 1);

    // Disable `a` → only `b` ticks.
    a.setEnabled(false);
    drivers.loop();
    CHECK(a.loopCalls == 1);  // unchanged
    CHECK(b.loopCalls == 2);

    // Disable `b` too → neither ticks.
    b.setEnabled(false);
    drivers.loop();
    CHECK(a.loopCalls == 1);
    CHECK(b.loopCalls == 2);

    // Re-enable `a` → only `a` ticks.
    a.setEnabled(true);
    drivers.loop();
    CHECK(a.loopCalls == 2);
    CHECK(b.loopCalls == 2);
}
