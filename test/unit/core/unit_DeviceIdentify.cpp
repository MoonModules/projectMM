// @module DevicePlugin
// @also DevicesModule

// Pins the device-interop plugin classification: each plugin claims one mDNS service
// and turns a resolved hit into a Device kind. Pure host logic — feed a synthetic
// MdnsHost (the POD the platform listener delivers), assert the classification, with
// no network. The plugins are the "second caller" that makes the seam testable.

#include "doctest.h"
#include "core/DevicePlugin.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

using namespace mm;

namespace {
// Build a synthetic resolved mDNS hit.
platform::MdnsHost host(const char* service, const char* name, bool isMM) {
    platform::MdnsHost h{};
    h.ip[0] = 192; h.ip[1] = 168; h.ip[2] = 1; h.ip[3] = 50;
    std::snprintf(h.hostname, sizeof(h.hostname), "%s", name);
    std::snprintf(h.service, sizeof(h.service), "%s", service);
    h.isProjectMM = isMM;
    return h;
}
}  // namespace

TEST_CASE("MmPlugin claims an _http._tcp hit carrying the mm=1 TXT") {
    MmPlugin p;
    CHECK(std::strcmp(p.service(), "_http") == 0);
    CHECK(std::strcmp(p.proto(), "_tcp") == 0);

    DiscoveredDevice d;
    REQUIRE(p.classify(host("_http", "Bench-P4", /*isMM=*/true), d));
    CHECK(d.type == DevType::ProjectMM);
    CHECK(std::strcmp(d.name, "Bench-P4") == 0);
}

TEST_CASE("MmPlugin declines a generic _http._tcp box (no mm=1)") {
    MmPlugin p;
    DiscoveredDevice d;
    // A web device on the shared _http._tcp service without our marker is not us.
    CHECK_FALSE(p.classify(host("_http", "some-printer", /*isMM=*/false), d));
}

TEST_CASE("WledPlugin claims a _wled._tcp hit as WLED") {
    WledPlugin p;
    CHECK(std::strcmp(p.service(), "_wled") == 0);

    DiscoveredDevice d;
    REQUIRE(p.classify(host("_wled", "wled-desk", /*isMM=*/false), d));
    CHECK(d.type == DevType::Wled);
    CHECK(std::strcmp(d.name, "wled-desk") == 0);
}

TEST_CASE("Plugins tolerate an empty hostname (the module supplies the IP fallback)") {
    // A hit with no resolved name still classifies by type; DevicesModule fills the
    // display name from the IP, so an empty name here is acceptable, not a crash.
    WledPlugin p;
    DiscoveredDevice d;
    REQUIRE(p.classify(host("_wled", "", /*isMM=*/false), d));
    CHECK(d.type == DevType::Wled);
    CHECK(d.name[0] == '\0');
}
