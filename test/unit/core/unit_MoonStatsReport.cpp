// @module MoonStatsReport

#include "doctest.h"
#include "core/MoonStatsModule.h"

#include <cstring>
#include <string>

#include "core/SystemModule.h"

namespace {

/// A module carrying exactly the controls a real device would expose that MUST NEVER be reported:
/// the identifying ones (device name, MAC), the secret ones (SSID, password), and free text the
/// user typed. Built to look as much like the real System module as possible, because a builder
/// that walked the tree rather than naming its fields would happily emit all of these.
class LeakyModule : public mm::MoonModule {
public:
    LeakyModule() { setName("System"); }

    void defineControls() override {
        std::strncpy(deviceName_, "ewoud-livingroom", sizeof(deviceName_) - 1);
        std::strncpy(mac_, "A4:CF:12:9B:33:07", sizeof(mac_) - 1);
        std::strncpy(ssid_, "Travelrouter", sizeof(ssid_) - 1);
        std::strncpy(password_, "hunter2-secret", sizeof(password_) - 1);
        std::strncpy(note_, "my bedroom wall, do not touch", sizeof(note_) - 1);
        std::strncpy(chip_, "ESP32-S3", sizeof(chip_) - 1);
        std::strncpy(flash_, "16MB", sizeof(flash_) - 1);

        controls_.addText("deviceName", deviceName_, sizeof(deviceName_));
        controls_.addReadOnly("mac", mac_, sizeof(mac_));
        controls_.addText("ssid", ssid_, sizeof(ssid_));
        controls_.addPassword("password", password_, sizeof(password_));
        controls_.addText("note", note_, sizeof(note_));
        controls_.addReadOnly("chip", chip_, sizeof(chip_));
        controls_.addReadOnly("flash", flash_, sizeof(flash_));
    }

private:
    char deviceName_[32] = {};
    char mac_[24] = {};
    char ssid_[32] = {};
    char password_[32] = {};
    char note_[40] = {};
    char chip_[16] = {};
    char flash_[8] = {};
};

std::string report(mm::MoonStatsEvent event = mm::MoonStatsEvent::Install,
                   const char* id = nullptr,
                   const char* version = "4.0.0",
                   const char* previous = nullptr) {
    LeakyModule sys;
    sys.defineControls();
    mm::MoonModule* tree[] = {&sys};
    mm::JsonSink sink;
    mm::buildMoonStatsReport(sink, tree, 1, event, id, version, previous);
    return std::string(sink.data(), sink.size());
}

}  // namespace

/// The report never carries anything that identifies the person or their network, however much of
/// it the module tree holds.
///
/// This is the privacy policy made executable. The policy promises no device name, no network
/// addresses, no credentials and no free text the user typed, and the tree here holds all four
/// sitting beside the hardware fields that ARE reported. A builder that emitted what it found
/// rather than naming each field would fail this the first time it ran.
TEST_CASE("the usage report cannot carry identifying or secret values") {
    const std::string json = report();

    // The VALUES, which is what would actually harm someone.
    CHECK(json.find("ewoud-livingroom") == std::string::npos);
    CHECK(json.find("A4:CF:12:9B:33:07") == std::string::npos);
    CHECK(json.find("Travelrouter") == std::string::npos);
    CHECK(json.find("hunter2-secret") == std::string::npos);
    CHECK(json.find("my bedroom wall") == std::string::npos);

    // The KEYS, so a later refactor cannot reintroduce the field with an empty value and look
    // harmless while the next change fills it in.
    CHECK(json.find("deviceName") == std::string::npos);
    CHECK(json.find("\"mac\"") == std::string::npos);
    CHECK(json.find("ssid") == std::string::npos);
    CHECK(json.find("password") == std::string::npos);
    CHECK(json.find("note") == std::string::npos);
}

/// The hardware facts the report exists for do arrive, so the test above is not passing merely
/// because the builder emits nothing.
TEST_CASE("the usage report carries the hardware facts it exists to collect") {
    const std::string json = report();
    CHECK(json.find("ESP32-S3") != std::string::npos);
    CHECK(json.find("16MB") != std::string::npos);
    CHECK(json.find("\"chip\"") != std::string::npos);
    CHECK(json.find("\"flash\"") != std::string::npos);
}

/// An install and an upgrade are told apart by the report itself, with no identifier involved: a
/// previous version present means the firmware changed under an existing install.
TEST_CASE("an upgrade is distinguished from a fresh install by the previous version") {
    const std::string fresh = report(mm::MoonStatsEvent::Install, nullptr, "4.0.0", nullptr);
    CHECK(fresh.find("\"event\":\"install\"") != std::string::npos);
    CHECK(fresh.find("previousVersion") == std::string::npos);

    const std::string upgraded = report(mm::MoonStatsEvent::Upgrade, nullptr, "4.1.0", "4.0.0");
    CHECK(upgraded.find("\"event\":\"upgrade\"") != std::string::npos);
    CHECK(upgraded.find("\"previousVersion\":\"4.0.0\"") != std::string::npos);
}

/// A report built without consent carries no installation id at all, rather than an empty or
/// placeholder one: nothing is generated until the user says yes.
TEST_CASE("no installation id appears until one is supplied") {
    CHECK(report().find("installationId") == std::string::npos);

    const std::string withId = report(mm::MoonStatsEvent::Install,
                                      "66b1706d30ff5c0fb1c6fdd7f6fe1151");
    CHECK(withId.find("\"installationId\":\"66b1706d30ff5c0fb1c6fdd7f6fe1151\"")
          != std::string::npos);
}

/// Only enabled modules are named, so the report describes what a device actually runs.
TEST_CASE("the report names the modules that are enabled") {
    const std::string json = report();
    CHECK(json.find("\"modules\":[\"System\"]") != std::string::npos);
}
