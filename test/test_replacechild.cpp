#include "doctest.h"
#include "core/MoonModule.h"

#include <string>
#include <vector>

namespace {

// Records lifecycle calls in order, so a replace can be checked for the
// onBuildControls → setup → onAllocateMemory sequence the HTTP handler runs.
struct Trace {
    std::vector<std::string> calls;
};

class TracingModule : public mm::MoonModule {
public:
    Trace* trace = nullptr;
    void onBuildControls() override { if (trace) trace->calls.push_back(std::string(name()) + ":build"); }
    void setup() override { if (trace) trace->calls.push_back(std::string(name()) + ":setup"); }
    void onAllocateMemory() override { if (trace) trace->calls.push_back(std::string(name()) + ":alloc"); }
    void teardown() override { if (trace) trace->calls.push_back(std::string(name()) + ":teardown"); }
};

struct Fixture {
    mm::MoonModule parent;
    TracingModule a, b, c;
    Fixture() {
        a.setName("A");
        b.setName("B");
        c.setName("C");
        parent.addChild(&a);
        parent.addChild(&b);
        parent.addChild(&c);
    }
    std::string order() const {
        std::string s;
        for (uint8_t i = 0; i < parent.childCount(); i++) s += parent.child(i)->name();
        return s;
    }
};

} // namespace

TEST_CASE("replaceChildAt: swaps at the same position, siblings intact") {
    Fixture f;
    TracingModule fresh;
    fresh.setName("X");
    CHECK(f.order() == "ABC");

    mm::MoonModule* old = f.parent.replaceChildAt(1, &fresh);  // replace B

    CHECK(old == &f.b);
    CHECK(f.order() == "AXC");          // position 1 swapped, A and C untouched
    CHECK(f.parent.childCount() == 3);  // count unchanged
}

TEST_CASE("replaceChildAt: old child detached, fresh child parented") {
    Fixture f;
    TracingModule fresh;
    fresh.setName("X");

    mm::MoonModule* old = f.parent.replaceChildAt(0, &fresh);

    CHECK(old == &f.a);
    CHECK(old->parent() == nullptr);          // removed child detached
    CHECK(fresh.parent() == &f.parent);       // replacement linked to parent
}

TEST_CASE("replaceChildAt: out-of-range index returns nullptr, tree untouched") {
    Fixture f;
    TracingModule fresh;
    fresh.setName("X");

    CHECK(f.parent.replaceChildAt(99, &fresh) == nullptr);
    CHECK(f.order() == "ABC");
    CHECK(fresh.parent() == nullptr);  // fresh was not linked
}

TEST_CASE("replaceChildAt: null replacement returns nullptr") {
    Fixture f;
    CHECK(f.parent.replaceChildAt(0, nullptr) == nullptr);
    CHECK(f.order() == "ABC");
}

TEST_CASE("replace lifecycle: fresh module is built, set up, allocated in order") {
    // Mirrors what HttpServerModule::handleReplaceModule does to the replacement:
    // onBuildControls → setup → onAllocateMemory, then teardown on the old module.
    Fixture f;
    Trace trace;
    f.b.trace = &trace;
    TracingModule fresh;
    fresh.setName("X");
    fresh.trace = &trace;

    mm::MoonModule* old = f.parent.replaceChildAt(1, &fresh);
    fresh.onBuildControls();
    fresh.setup();
    fresh.onAllocateMemory();
    old->teardown();

    REQUIRE(trace.calls.size() == 4);
    CHECK(trace.calls[0] == "X:build");
    CHECK(trace.calls[1] == "X:setup");
    CHECK(trace.calls[2] == "X:alloc");
    CHECK(trace.calls[3] == "B:teardown");
}
