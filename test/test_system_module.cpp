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

    // Should have controls: uptime, fps, tickTimeUs, maxBlock, deviceName, chip, sdk
    // On desktop, freeHeap returns 0, so progress bars are skipped
    CHECK(sys.controls().count() >= 6); // uptime, fps, tickTimeUs, maxBlock, deviceName, chip, sdk

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
