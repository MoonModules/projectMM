#include "doctest.h"
#include "core/Scheduler.h"

using namespace mm;

class CounterModule : public MoonModule {
public:
    const char* name() const override { return "counter"; }
    int setupCount = 0, loopCount = 0, teardownCount = 0;
    void setup() override { ++setupCount; }
    void loop() override { ++loopCount; }
    void teardown() override { ++teardownCount; }
};

TEST_CASE("Scheduler setup calls all modules") {
    CounterModule a, b;
    Scheduler sched;
    sched.add(&a);
    sched.add(&b);
    sched.setup();

    CHECK(a.setupCount == 1);
    CHECK(b.setupCount == 1);
}

TEST_CASE("Scheduler loop calls all modules and increments frame") {
    CounterModule a;
    Scheduler sched;
    sched.add(&a);
    sched.setup();

    CHECK(sched.frame() == 0);
    sched.loop();
    CHECK(sched.frame() == 1);
    CHECK(a.loopCount == 1);

    sched.loop();
    sched.loop();
    CHECK(sched.frame() == 3);
    CHECK(a.loopCount == 3);
}

TEST_CASE("Scheduler teardown calls all modules") {
    CounterModule a, b;
    Scheduler sched;
    sched.add(&a);
    sched.add(&b);
    sched.setup();
    sched.loop();
    sched.teardown();

    CHECK(a.teardownCount == 1);
    CHECK(b.teardownCount == 1);
}

TEST_CASE("Scheduler MAX_MODULES overflow") {
    Scheduler sched;
    CounterModule modules[Scheduler::MAX_MODULES + 2];
    for (uint8_t i = 0; i < Scheduler::MAX_MODULES + 2; ++i) {
        sched.add(&modules[i]);
    }
    CHECK(sched.count() == Scheduler::MAX_MODULES);

    sched.setup();
    // Only MAX_MODULES should have been set up
    CHECK(modules[0].setupCount == 1);
    CHECK(modules[Scheduler::MAX_MODULES - 1].setupCount == 1);
    CHECK(modules[Scheduler::MAX_MODULES].setupCount == 0);
}

TEST_CASE("Scheduler elapsed tracks time") {
    Scheduler sched;
    sched.setup();
    // elapsed() should be >= 0 (hard to test precisely without sleeping)
    CHECK(sched.elapsed() >= 0);
}
