#include "doctest.h"
#include "core/MoonModule.h"

namespace {

class TestModule : public mm::MoonModule {
public:
    uint8_t brightness = 128;
    uint8_t speed = 60;
    bool enabled = true;

    bool setupCalled = false;
    bool teardownCalled = false;

    void setup() override { setupCalled = true; }
    void teardown() override { teardownCalled = true; }

    void onBuildControls() override {
        controls_.addUint8("brightness", brightness, 0, 255);
        controls_.addUint8("speed", speed, 1, 255);
        controls_.addBool("enabled", enabled);
    }
};

} // namespace

TEST_CASE("MoonModule lifecycle") {
    TestModule mod;
    CHECK_FALSE(mod.setupCalled);
    mod.setup();
    CHECK(mod.setupCalled);
    mod.teardown();
    CHECK(mod.teardownCalled);
}

TEST_CASE("MoonModule name") {
    TestModule mod;
    CHECK(mod.name() == nullptr);
    mod.setName("TestModule");
    CHECK(std::strcmp(mod.name(), "TestModule") == 0);
}

TEST_CASE("MoonModule parent") {
    TestModule parent;
    TestModule child;
    CHECK(child.parent() == nullptr);
    child.setParent(&parent);
    CHECK(child.parent() == &parent);
}

TEST_CASE("Control binding via ControlList") {
    TestModule mod;
    mod.onBuildControls();

    CHECK(mod.controls().count() == 3);
    CHECK(std::strcmp(mod.controls()[0].name, "brightness") == 0);
    CHECK(mod.controls()[0].type == mm::ControlType::Uint8);
    CHECK(mod.controls()[0].ptr == &mod.brightness);

    // Verify binding: changing the variable is visible via the pointer
    mod.brightness = 200;
    CHECK(*static_cast<uint8_t*>(mod.controls()[0].ptr) == 200);

    // Verify binding: changing via pointer updates the variable
    *static_cast<uint8_t*>(mod.controls()[0].ptr) = 50;
    CHECK(mod.brightness == 50);
}

TEST_CASE("ControlList clear and rebuild") {
    TestModule mod;
    mod.onBuildControls();
    CHECK(mod.controls().count() == 3);

    mod.controls().clear();
    CHECK(mod.controls().count() == 0);

    mod.onBuildControls();
    CHECK(mod.controls().count() == 3);
}

TEST_CASE("Bool control binding") {
    TestModule mod;
    mod.onBuildControls();

    auto& ctrl = mod.controls()[2];
    CHECK(std::strcmp(ctrl.name, "enabled") == 0);
    CHECK(ctrl.type == mm::ControlType::Bool);
    CHECK(*static_cast<bool*>(ctrl.ptr) == true);

    mod.enabled = false;
    CHECK(*static_cast<bool*>(ctrl.ptr) == false);
}
