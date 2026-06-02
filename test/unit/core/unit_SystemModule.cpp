// @module SystemModule

#include "doctest.h"
#include "core/SystemModule.h"

// On the desktop platform (MAC DE:AD:BE:EF:CA:FE), the auto-generated device name is "MM-CAFE" (last two MAC bytes).
TEST_CASE("SystemModule MAC-to-deviceName") {
    // Desktop platform returns MAC DE:AD:BE:EF:CA:FE
    // deviceName should be MM-CAFE (last two bytes)
    mm::SystemModule sys;
    sys.setup();
    CHECK(std::strcmp(sys.deviceName(), "MM-CAFE") == 0);
}

// After setup, SystemModule exposes exactly 12 controls on desktop, including a deviceName Text control bound to the MAC-derived name.
TEST_CASE("SystemModule controls") {
    mm::SystemModule sys;
    sys.setup();
    sys.onBuildControls();

    // Desktop control set is exactly 12: deviceName, uptime, fps, tickTimeUs,
    // maxBlock, filesystem, version, build, board, chip, sdk, bootReason. The
    // heap/psram/firmware progress bars are skipped on desktop (totalInternalHeap/
    // firmware partition report 0); the filesystem bar is present (partition has
    // a total). Exact count so a removed/renamed control fails the test instead
    // of slipping by.
    CHECK(sys.controls().count() == 12);

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

// The `board` control is always present and non-empty (either a real board key from build_info.h or the fallback "unknown").
TEST_CASE("SystemModule board control populated") {
    // The board control is wired in setup() from kBoardName (build_info.h).
    // Local desktop builds fall through to "unknown" because CMake doesn't
    // pass -DMM_BOARD_NAME; release builds get the real key. Either way,
    // the control must exist and be non-empty so the future OTA path has
    // something to read.
    mm::SystemModule sys;
    sys.setup();
    sys.onBuildControls();

    bool found = false;
    for (uint8_t i = 0; i < sys.controls().count(); i++) {
        if (std::strcmp(sys.controls()[i].name, "board") == 0) {
            CHECK(sys.controls()[i].type == mm::ControlType::ReadOnly);
            const char* val = static_cast<const char*>(sys.controls()[i].ptr);
            CHECK(val != nullptr);
            CHECK(val[0] != '\0');  // non-empty (either a real key or "unknown")
            found = true;
        }
    }
    CHECK(found);
}

// The `bootReason` control is populated from platform::resetReason; on desktop it reports "OK".
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
