// @module FirmwareUpdateModule

#include "doctest.h"
#include "core/FirmwareUpdateModule.h"
#include <cstring>

// The `firmware` control is always present and non-empty (either a real firmware key from
// build_info.h or the fallback "unknown"). Moved here from SystemModule — the firmware card now
// owns firmware identity (version/build/firmware) + the partition usage.
TEST_CASE("FirmwareUpdateModule firmware control populated") {
    // The firmware control is wired in setup() from kFirmwareName (build_info.h).
    // Local desktop builds fall through to "unknown" because CMake doesn't
    // pass -DMM_FIRMWARE_NAME; release builds get the real key. Either way,
    // the control must exist and be non-empty so the OTA / install-picker path
    // has something to read. (See docs/architecture.md § Firmware vs board —
    // "firmware" is the compiled-binary variant; the physical board is separate.)
    mm::FirmwareUpdateModule fw;
    fw.setup();
    fw.onBuildControls();

    bool found = false;
    for (uint8_t i = 0; i < fw.controls().count(); i++) {
        if (std::strcmp(fw.controls()[i].name, "firmware") == 0) {
            CHECK(fw.controls()[i].type == mm::ControlType::ReadOnly);
            const char* val = static_cast<const char*>(fw.controls()[i].ptr);
            CHECK(val != nullptr);
            CHECK(val[0] != '\0');  // non-empty (either a real key or "unknown")
            found = true;
        }
    }
    CHECK(found);

    // version + build are part of this module's firmware-identity controls, so they're present too.
    // firmwarePartition is gated on platform::firmwarePartition() > 0 (the app-partition size), so it
    // appears on a real device but NOT on desktop/test where firmwarePartition() returns 0 — assert
    // its TYPE only when present (a Progress control), rather than its presence, so this stays valid
    // on both.
    bool hasVersion = false, hasBuild = false;
    for (uint8_t i = 0; i < fw.controls().count(); i++) {
        const auto& c = fw.controls()[i];
        if (std::strcmp(c.name, "version") == 0) hasVersion = true;
        if (std::strcmp(c.name, "build") == 0) hasBuild = true;
        if (std::strcmp(c.name, "firmwarePartition") == 0) CHECK(c.type == mm::ControlType::Progress);
    }
    CHECK(hasVersion);
    CHECK(hasBuild);
}
