#include "doctest.h"
#include "core/SystemModule.h"

TEST_CASE("SystemModule MAC-to-deviceName") {
    // Desktop platform returns MAC DE:AD:BE:EF:CA:FE
    // deviceName should be MM-CAFE (last two bytes)
    mm::SystemModule sys;
    sys.setup();
    CHECK(std::strcmp(sys.deviceName(), "MM-CAFE") == 0);
}

TEST_CASE("SystemModule controls") {
    mm::SystemModule sys;
    sys.setup();
    sys.onBuildControls();

    // Should have controls: uptime, fps, tickTimeUs, maxBlock, deviceName, chip, sdk, bootReason
    // On desktop, freeHeap returns 0, so progress bars are skipped
    CHECK(sys.controls().count() >= 6);

    // Find deviceName control
    bool found = false;
    for (uint8_t i = 0; i < sys.controls().count(); i++) {
        if (std::strcmp(sys.controls()[i].name, "deviceName") == 0) {
            CHECK(sys.controls()[i].type == mm::ControlType::Text);
            CHECK(std::strcmp(static_cast<char*>(sys.controls()[i].ptr), "MM-CAFE") == 0);
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("SystemModule bootReason control populated") {
    // The bootReason control is wired in setup() (from platform::resetReason). On
    // desktop the platform stub always returns "OK". The UI uses this to set the
    // reboot button's crashed-state styling — see ui-spec.md.
    mm::SystemModule sys;
    sys.setup();
    sys.onBuildControls();

    bool found = false;
    for (uint8_t i = 0; i < sys.controls().count(); i++) {
        if (std::strcmp(sys.controls()[i].name, "bootReason") == 0) {
            CHECK(sys.controls()[i].type == mm::ControlType::ReadOnly);
            const char* val = static_cast<const char*>(sys.controls()[i].ptr);
            CHECK(val != nullptr);
            CHECK(val[0] != '\0');  // non-empty
            // Desktop stub always reports "OK"
            CHECK(std::strcmp(val, "OK") == 0);
            found = true;
        }
    }
    CHECK(found);
}
