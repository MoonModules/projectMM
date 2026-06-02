// @module MoonModule

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

// setup() and teardown() each fire exactly when called and update their respective state flags.
TEST_CASE("MoonModule lifecycle") {
    TestModule mod;
    CHECK_FALSE(mod.setupCalled);
    mod.setup();
    CHECK(mod.setupCalled);
    mod.teardown();
    CHECK(mod.teardownCalled);
}

// name() starts empty; setName() copies the string into the internal 16-byte buffer.
TEST_CASE("MoonModule name") {
    TestModule mod;
    CHECK(mod.name()[0] == '\0');
    mod.setName("TestModule");
    CHECK(std::strcmp(mod.name(), "TestModule") == 0);
}

// typeName (set by the factory) is independent of name; setName doesn't touch typeName so a human-renamed module still serializes under its real type.
TEST_CASE("MoonModule typeName is independent of name") {
    TestModule mod;
    // Both empty by default
    CHECK(mod.name()[0] == '\0');
    CHECK(mod.typeName()[0] == '\0');

    // Factory sets typeName then name; later setName doesn't touch typeName
    mod.setTypeName("NoiseEffect");
    mod.setName("NoiseEffect");
    CHECK(std::strcmp(mod.typeName(), "NoiseEffect") == 0);

    mod.setName("Noise");   // human label override (matches main.cpp pattern)
    CHECK(std::strcmp(mod.name(), "Noise") == 0);
    CHECK(std::strcmp(mod.typeName(), "NoiseEffect") == 0);
}

// dirty()/markDirty()/clearDirty() round-trip cleanly (the bit FilesystemModule polls for save scheduling).
TEST_CASE("MoonModule dirty flag") {
    TestModule mod;
    CHECK_FALSE(mod.dirty());
    mod.markDirty();
    CHECK(mod.dirty());
    mod.clearDirty();
    CHECK_FALSE(mod.dirty());
}

// parent() starts null; setParent() records the upstream container for tree walks.
TEST_CASE("MoonModule parent") {
    TestModule parent;
    TestModule child;
    CHECK(child.parent() == nullptr);
    child.setParent(&parent);
    CHECK(child.parent() == &parent);
}

// Adding Uint8/Bool controls stores live pointers to the module's fields, so changes propagate either direction (field ↔ control->ptr).
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

// controls().clear() empties the list; calling onBuildControls() again repopulates it (the standard rebuild path).
TEST_CASE("ControlList clear and rebuild") {
    TestModule mod;
    mod.onBuildControls();
    CHECK(mod.controls().count() == 3);

    mod.controls().clear();
    CHECK(mod.controls().count() == 0);

    mod.onBuildControls();
    CHECK(mod.controls().count() == 3);
}

// addReadOnly binds a char buffer the UI can render; updating the buffer is visible through control.ptr.
TEST_CASE("ReadOnly control binding") {
    mm::MoonModule mod;
    char statusBuf[32] = "idle";
    mod.controls().addReadOnly("status", statusBuf, sizeof(statusBuf));

    CHECK(mod.controls().count() == 1);
    auto& ctrl = mod.controls()[0];
    CHECK(std::strcmp(ctrl.name, "status") == 0);
    CHECK(ctrl.type == mm::ControlType::ReadOnly);
    CHECK(std::strcmp(static_cast<char*>(ctrl.ptr), "idle") == 0);

    // Update the buffer — control reflects it
    std::strcpy(statusBuf, "running");
    CHECK(std::strcmp(static_cast<char*>(ctrl.ptr), "running") == 0);
}

// addSelect binds a uint8 + an options array (stored in aux) — control.max carries the option count.
TEST_CASE("Select control binding") {
    mm::MoonModule mod;
    uint8_t mode = 0;
    static constexpr const char* options[] = {"Off", "Auto", "Manual"};
    mod.controls().addSelect("mode", mode, options, 3);

    CHECK(mod.controls().count() == 1);
    auto& ctrl = mod.controls()[0];
    CHECK(std::strcmp(ctrl.name, "mode") == 0);
    CHECK(ctrl.type == mm::ControlType::Select);
    CHECK(*static_cast<uint8_t*>(ctrl.ptr) == 0);
    CHECK(ctrl.max == 3); // option count

    // Options pointer is stored in aux
    auto* opts = reinterpret_cast<const char* const*>(ctrl.aux);
    CHECK(std::strcmp(opts[0], "Off") == 0);
    CHECK(std::strcmp(opts[1], "Auto") == 0);
    CHECK(std::strcmp(opts[2], "Manual") == 0);

    // Change via pointer
    mode = 2;
    CHECK(*static_cast<uint8_t*>(ctrl.ptr) == 2);
}

// addProgress binds a uint32 plus a "total" value (in aux) — the UI renders value/total as a progress bar.
TEST_CASE("Progress control binding") {
    mm::MoonModule mod;
    uint32_t used = 1000;
    mod.controls().addProgress("heap", used, 4096);

    CHECK(mod.controls().count() == 1);
    auto& ctrl = mod.controls()[0];
    CHECK(std::strcmp(ctrl.name, "heap") == 0);
    CHECK(ctrl.type == mm::ControlType::Progress);
    CHECK(*static_cast<uint32_t*>(ctrl.ptr) == 1000);
    CHECK(ctrl.aux == 4096); // total

    // Update value
    used = 2048;
    CHECK(*static_cast<uint32_t*>(ctrl.ptr) == 2048);
}

// enabled defaults to true; setEnabled flips the universal enable gate (Scheduler and parent containers respect it).
TEST_CASE("Module enabled property") {
    mm::MoonModule mod;
    CHECK(mod.enabled() == true); // default enabled
    mod.setEnabled(false);
    CHECK(mod.enabled() == false);
    mod.setEnabled(true);
    CHECK(mod.enabled() == true);
}

// addBool binds a bool field — toggling the field updates control.ptr's view.
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
