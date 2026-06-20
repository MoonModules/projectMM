// @module SystemModule

#include "doctest.h"
#include "core/SystemModule.h"

#include <cstring>

namespace {
// Stand-in Peripheral child: counts the lifecycle callbacks a real sensor would
// use (setup to init hardware, loop20ms/loop1s to poll + format). Pins that
// SystemModule's overridden setup()/loop1s() chain to base — without that, an
// added peripheral would never initialise or poll.
class CountingPeripheral : public mm::MoonModule {
public:
    mm::ModuleRole role() const override { return mm::ModuleRole::Peripheral; }
    uint32_t setupCalls = 0, loop20msCalls = 0, loop1sCalls = 0;
    void setup() override { setupCalls++; }
    void loop20ms() override { loop20msCalls++; }
    void loop1s() override { loop1sCalls++; }
};
} // namespace

// On the desktop platform (MAC DE:AD:BE:EF:CA:FE), the auto-generated device name is "MM-CAFE" (last two MAC bytes).
TEST_CASE("SystemModule MAC-to-deviceName") {
    // Desktop platform returns MAC DE:AD:BE:EF:CA:FE
    // deviceName should be MM-CAFE (last two bytes)
    mm::SystemModule sys;
    sys.setup();
    CHECK(std::strcmp(sys.deviceName(), "MM-CAFE") == 0);
}

// deviceName is bound as a Text control to the MAC-derived default ("MM-CAFE" on the desktop platform).
TEST_CASE("SystemModule deviceName control") {
    mm::SystemModule sys;
    sys.setup();
    sys.onBuildControls();

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

namespace {
// Overwrite SystemModule's deviceName buffer through its bound control pointer —
// the same buffer the persistence overlay and an /api/control write target. Lets a
// test seed an invalid name and then drive the module's sanitisation.
void writeDeviceName(mm::SystemModule& sys, const char* value) {
    for (uint8_t i = 0; i < sys.controls().count(); i++) {
        if (std::strcmp(sys.controls()[i].name, "deviceName") == 0) {
            char* buf = static_cast<char*>(sys.controls()[i].ptr);
            std::strncpy(buf, value, 23);
            buf[23] = 0;
            return;
        }
    }
    // No `deviceName` control found — a setup regression. Fail loudly rather than
    // silently no-op, which would let the calling test "pass" against a stale buffer.
    REQUIRE_MESSAGE(false, "writeDeviceName: no 'deviceName' control on SystemModule");
}
} // namespace

// deviceName is the single network identity, so SystemModule keeps it a valid hostname.
// A live edit to an invalid value ("My Room!") is coerced on the next loop1s tick
// (mm::sanitizeHostname), the same path mDNS/AP/DHCP read — so they never see spaces.
TEST_CASE("SystemModule sanitises a live deviceName edit") {
    mm::SystemModule sys;
    sys.setup();
    sys.onBuildControls();
    writeDeviceName(sys, "My Living Room!");
    sys.loop1s();                                   // the tick that coerces it
    CHECK(std::strcmp(sys.deviceName(), "My-Living-Room") == 0);
}

// An all-invalid name collapses to empty after sanitising; the MAC fallback then fills
// it, so deviceName is never empty (mDNS/AP/DHCP always have a name to register).
TEST_CASE("SystemModule falls back to the MAC name when deviceName is all-invalid") {
    mm::SystemModule sys;
    sys.setup();
    sys.onBuildControls();
    writeDeviceName(sys, "!@#$");
    sys.loop1s();
    CHECK(std::strcmp(sys.deviceName(), "MM-CAFE") == 0);   // desktop MAC fallback
}

// An already-valid name is left untouched (idempotent) — a normal user name survives.
TEST_CASE("SystemModule leaves a valid deviceName unchanged") {
    mm::SystemModule sys;
    sys.setup();
    sys.onBuildControls();
    writeDeviceName(sys, "Bench-S3");
    sys.loop1s();
    CHECK(std::strcmp(sys.deviceName(), "Bench-S3") == 0);
}

// The `firmware` control is always present and non-empty (either a real firmware key from build_info.h or the fallback "unknown").
TEST_CASE("SystemModule firmware control populated") {
    // The firmware control is wired in setup() from kFirmwareName (build_info.h).
    // Local desktop builds fall through to "unknown" because CMake doesn't
    // pass -DMM_FIRMWARE_NAME; release builds get the real key. Either way,
    // the control must exist and be non-empty so the OTA path has something
    // to read. (See docs/architecture.md § Firmware vs board — "firmware" is
    // the compiled-binary variant; the physical board is a separate concept.)
    mm::SystemModule sys;
    sys.setup();
    sys.onBuildControls();

    bool found = false;
    for (uint8_t i = 0; i < sys.controls().count(); i++) {
        if (std::strcmp(sys.controls()[i].name, "firmware") == 0) {
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

// SystemModule accepts user-added Peripheral children (sensors/actuators the
// user solders on); the role string drives the type-picker filter + add policy.
TEST_CASE("SystemModule accepts peripheral children") {
    mm::SystemModule sys;
    CHECK(std::strcmp(sys.acceptsChildRoles(), "peripheral") == 0);
}

// Regression: SystemModule overrides setup() and loop1s(); both must chain to
// MoonModule's base so a Peripheral child's setup()/loop1s() actually fire.
// Without the chain a sensor would never init or poll (the "children miss
// callbacks" trap from history/decisions.md). loop20ms() isn't overridden, so
// the base default already propagates it.
TEST_CASE("SystemModule propagates lifecycle to a peripheral child") {
    mm::SystemModule sys;
    CountingPeripheral periph;
    sys.addChild(&periph);

    sys.setup();
    CHECK(periph.setupCalls == 1);   // setup() chained to base

    sys.loop1s();
    CHECK(periph.loop1sCalls == 1);  // loop1s() chained to base

    sys.loop20ms();
    CHECK(periph.loop20msCalls == 1); // base default (not overridden) propagates
}

// roleName maps the new Peripheral enum to its lowercase API string.
TEST_CASE("Peripheral role name") {
    CHECK(std::strcmp(mm::roleName(mm::ModuleRole::Peripheral), "peripheral") == 0);
}
