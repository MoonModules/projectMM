// @module DevicesModule

// Pins the pure device-identification logic DevicesModule uses to classify a
// discovered host and read its name from the HTTP probe body. Extracted into
// DeviceIdentify.h precisely so it's testable without network I/O. These tests
// guard the two bug-prone parts found on the bench:
//   - classify: a projectMM /api/state ("modules"), a WLED /json/info ("WLED"),
//     anything else generic — and a TRUNCATED projectMM body (deviceName past the
//     buffer) must still classify as projectMM off the early "modules" marker.
//   - extractDeviceName: projectMM's deviceName is a control OBJECT, so the parser
//     must read the control's "value" ("Bench P4"), NOT the first quoted token
//     after "deviceName" (which is the "text" TYPE field — the original bug).
//   - any garbage / hostile body yields generic + empty name (robustness).

#include "doctest.h"
#include "core/DeviceIdentify.h"

#include <cstring>

using mm::DevType;
using mm::classifyDevice;
using mm::extractDeviceName;
using mm::devTypeStr;

TEST_CASE("classifyDevice: projectMM from /api/state modules array") {
    const char* state = "{\"modules\":[{\"name\":\"System\"}]}";
    CHECK(classifyDevice(state, nullptr) == DevType::ProjectMM);
}

TEST_CASE("classifyDevice: WLED from /json/info brand") {
    const char* info = "{\"ver\":\"0.14\",\"brand\":\"WLED\",\"name\":\"WLED-Desk\"}";
    CHECK(classifyDevice(nullptr, info) == DevType::Wled);
}

TEST_CASE("classifyDevice: a live non-projectMM/non-WLED host is generic") {
    CHECK(classifyDevice("<html>router</html>", nullptr) == DevType::Generic);
    CHECK(classifyDevice(nullptr, "{\"some\":\"json\"}") == DevType::Generic);
    CHECK(classifyDevice(nullptr, nullptr) == DevType::Generic);
}

TEST_CASE("classifyDevice: a truncated projectMM body still classifies (modules is early)") {
    // The probe buffer can cut off before deviceName on a big device, but "modules"
    // is the very first key — classification must not depend on the full body.
    const char* truncated = "{\"modules\":[{\"name\":\"System\",\"type\":\"SystemModule\",\"contro";
    CHECK(classifyDevice(truncated, nullptr) == DevType::ProjectMM);
}

TEST_CASE("extractDeviceName: projectMM reads the deviceName control's value, not the type") {
    // The regression: deviceName is {"name":"deviceName","type":"text","value":"X"}.
    // A naive "first string after deviceName" grabs "text"; we must get "Bench P4".
    const char* state =
        "{\"modules\":[{\"name\":\"System\",\"controls\":["
        "{\"name\":\"deviceName\",\"type\":\"text\",\"value\":\"Bench P4\"},"
        "{\"name\":\"uptime\",\"type\":\"display\",\"value\":\"0:01\"}]}]}";
    char out[24] = {};
    extractDeviceName(DevType::ProjectMM, state, out, sizeof(out));
    CHECK(std::strcmp(out, "Bench P4") == 0);
}

TEST_CASE("extractDeviceName: WLED reads the top-level name field") {
    const char* info = "{\"brand\":\"WLED\",\"name\":\"WLED-Desk\",\"ver\":\"0.14\"}";
    char out[24] = {};
    extractDeviceName(DevType::Wled, info, out, sizeof(out));
    CHECK(std::strcmp(out, "WLED-Desk") == 0);
}

TEST_CASE("extractDeviceName: generic / garbage / null bodies yield empty") {
    char out[24] = {"sentinel"};
    extractDeviceName(DevType::Generic, "{\"modules\":[]}", out, sizeof(out));
    CHECK(out[0] == 0);                                  // generic has no name source
    extractDeviceName(DevType::ProjectMM, "garbage{{{", out, sizeof(out));
    CHECK(out[0] == 0);                                  // no deviceName → empty
    extractDeviceName(DevType::ProjectMM, nullptr, out, sizeof(out));
    CHECK(out[0] == 0);                                  // null body → empty, no crash
}

TEST_CASE("extractDeviceName: respects the output buffer size (no overflow)") {
    const char* state =
        "{\"modules\":[{\"controls\":[{\"name\":\"deviceName\",\"type\":\"text\","
        "\"value\":\"A-very-long-device-name-well-past-the-buffer\"}]}]}";
    char out[8] = {};
    extractDeviceName(DevType::ProjectMM, state, out, sizeof(out));
    CHECK(std::strlen(out) <= 7);                        // truncated, NUL-terminated
    CHECK(std::strncmp(out, "A-very-", 7) == 0);
}

TEST_CASE("devTypeStr maps every type") {
    CHECK(std::strcmp(devTypeStr(DevType::ProjectMM), "projectMM") == 0);
    CHECK(std::strcmp(devTypeStr(DevType::Wled), "WLED") == 0);
    CHECK(std::strcmp(devTypeStr(DevType::Generic), "generic") == 0);
}
