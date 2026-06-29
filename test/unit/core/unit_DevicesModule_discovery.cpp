// @module DevicesModule
// @also DevicePlugin

// Drives the full mDNS discovery pipeline on the host — feed synthetic MdnsHost hits
// through injectMdnsHitForTest() (the same entry the platform listener callback uses)
// and assert the resulting device list: classification, the projectMM>WLED type
// priority, and that a hit for one device never contaminates another's name/type.
// Pure host logic, no network — the test seam makes the private upsert path testable.

#include "doctest.h"
#include "core/DevicesModule.h"
#include "core/JsonSink.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace mm;

namespace {

platform::MdnsHost hit(const char* service, const char* name, bool isMM,
                       uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    platform::MdnsHost h{};
    h.ip[0] = a; h.ip[1] = b; h.ip[2] = c; h.ip[3] = d;
    std::snprintf(h.hostname, sizeof(h.hostname), "%s", name);
    std::snprintf(h.service, sizeof(h.service), "%s", service);
    h.isProjectMM = isMM;
    return h;
}

// Find the serialized row for an IP; return its full JSON (or "" if absent).
std::string rowFor(DevicesModule& dev, const char* ip) {
    for (uint8_t i = 0; i < dev.listRowCount(); i++) {
        mm::JsonSink sink;
        dev.writeListRow(sink, i);
        if (std::strstr(sink.data(), ip)) return sink.data();
    }
    return "";
}

}  // namespace

TEST_CASE("DevicesModule: a _wled hit lists a WLED device with its name") {
    DevicesModule dev;
    dev.injectMdnsHitForTest(hit("_wled", "wled-desk", false, 192, 168, 1, 50));
    std::string row = rowFor(dev, "192.168.1.50");
    CHECK(std::strstr(row.c_str(), "\"type\":\"WLED\"") != nullptr);
    CHECK(std::strstr(row.c_str(), "wled-desk") != nullptr);
}

TEST_CASE("DevicesModule: a _http+mm=1 hit lists a projectMM device") {
    DevicesModule dev;
    dev.injectMdnsHitForTest(hit("_http", "MM-Bench", true, 192, 168, 1, 60));
    std::string row = rowFor(dev, "192.168.1.60");
    CHECK(std::strstr(row.c_str(), "\"type\":\"projectMM\"") != nullptr);
    CHECK(std::strstr(row.c_str(), "MM-Bench") != nullptr);
}

TEST_CASE("DevicesModule: a generic _http hit (no mm=1) is NOT listed as projectMM") {
    DevicesModule dev;
    dev.injectMdnsHitForTest(hit("_http", "some-printer", false, 192, 168, 1, 70));
    // No plugin claims a bare _http hit, so it's not added at all.
    CHECK(dev.listRowCount() == 0);
}

// The P4-bench bug: two DIFFERENT devices (a WLED and a projectMM peer) must each keep
// their OWN name + type. The bug showed a projectMM name ("MM-S3") on the WLED's IP,
// typed WLED — a cross-contamination between hits.
TEST_CASE("DevicesModule: distinct devices don't cross-contaminate name or type") {
    DevicesModule dev;
    dev.injectMdnsHitForTest(hit("_wled", "wled-desk",  false, 192, 168, 1, 186));  // a WLED
    dev.injectMdnsHitForTest(hit("_http", "MM-S3", true, 192, 168, 1, 157));        // a projectMM peer

    std::string wled = rowFor(dev, "192.168.1.186");
    std::string mm   = rowFor(dev, "192.168.1.157");
    // The WLED row keeps the WLED name + WLED type — NOT the projectMM peer's name.
    CHECK(std::strstr(wled.c_str(), "wled-desk") != nullptr);
    CHECK(std::strstr(wled.c_str(), "\"type\":\"WLED\"") != nullptr);
    CHECK(std::strstr(wled.c_str(), "MM-S3") == nullptr);   // the contamination bug
    // The projectMM row keeps its own name + type.
    CHECK(std::strstr(mm.c_str(), "MM-S3") != nullptr);
    CHECK(std::strstr(mm.c_str(), "\"type\":\"projectMM\"") != nullptr);
}

// A peer RENAME must propagate: when a projectMM device's mDNS instance name changes
// (it carries the deviceName), a later hit with the new name updates the row in place —
// the live-update requirement (no re-query). The name comes from the mDNS announcement
// itself, so a re-sighting is what carries the new name.
TEST_CASE("DevicesModule: a peer rename updates the existing row's name") {
    DevicesModule dev;
    dev.injectMdnsHitForTest(hit("_http", "MM-OldName", true, 192, 168, 1, 100));
    REQUIRE(std::strstr(rowFor(dev, "192.168.1.100").c_str(), "MM-OldName") != nullptr);
    // Same IP, same device, NEW announced name → the row reflects the rename.
    dev.injectMdnsHitForTest(hit("_http", "MM-NewName", true, 192, 168, 1, 100));
    std::string row = rowFor(dev, "192.168.1.100");
    CHECK(std::strstr(row.c_str(), "MM-NewName") != nullptr);
    CHECK(std::strstr(row.c_str(), "MM-OldName") == nullptr);   // old name gone
}

// But a lower-authority hit must NOT clobber the good name: a projectMM device also
// answers _wled with its host name; that _wled hit must not overwrite the deviceName
// the _http(mm=1) hit set.
TEST_CASE("DevicesModule: a _wled hit does not overwrite a projectMM device's deviceName") {
    DevicesModule dev;
    dev.injectMdnsHitForTest(hit("_http", "MM-Real", true, 192, 168, 1, 110));   // real deviceName
    dev.injectMdnsHitForTest(hit("_wled", "mm-real-host", false, 192, 168, 1, 110)); // host name
    std::string row = rowFor(dev, "192.168.1.110");
    CHECK(std::strstr(row.c_str(), "MM-Real") != nullptr);            // deviceName kept
    CHECK(std::strstr(row.c_str(), "mm-real-host") == nullptr);
    CHECK(std::strstr(row.c_str(), "\"type\":\"projectMM\"") != nullptr);
}

// A projectMM device advertises BOTH _http(mm=1) AND _wled — its _wled hit must not
// downgrade it to WLED. Order: _wled hit first, then the _http hit raises it.
TEST_CASE("DevicesModule: a projectMM device seen on _wled first is raised, not stuck WLED") {
    DevicesModule dev;
    dev.injectMdnsHitForTest(hit("_wled", "MM-Peer", false, 192, 168, 1, 90));  // WLED-shaped first
    dev.injectMdnsHitForTest(hit("_http", "MM-Peer", true,  192, 168, 1, 90));  // then mm=1
    std::string row = rowFor(dev, "192.168.1.90");
    CHECK(std::strstr(row.c_str(), "\"type\":\"projectMM\"") != nullptr);   // raised
    CHECK(std::strstr(row.c_str(), "\"type\":\"WLED\"") == nullptr);
}
