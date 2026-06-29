// @module DevicesModule
// @also DevicePlugin

// Drives the full UDP discovery pipeline on the host — feed synthetic presence packets
// through injectPacketForTest() (the same entry the live recvFrom loop uses) and assert
// the resulting device list: classification, projectMM vs WLED typing, that one device's
// packet never contaminates another's name/type, and live rename. Pure host logic, no
// network — the test seam makes the private upsert path testable.

#include "doctest.h"
#include "core/DevicesModule.h"
#include "core/WledPacket.h"
#include "core/JsonSink.h"

#include <cstdint>
#include <cstring>
#include <string>

using namespace mm;

namespace {

// Inject a presence packet from `a.b.c.d` with `name`; `mm` marks it a projectMM peer.
void inject(DevicesModule& dev, const char* name, bool mm,
            uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    const uint8_t ip[4] = {a, b, c, d};
    uint8_t pkt[WledPacket::kSize];
    WledPacket::build(pkt, ip, name, /*boardType=*/34, /*lightsOn=*/true);
    if (mm) WledPacket::stampMmMarker(pkt);
    dev.injectPacketForTest(pkt, sizeof(pkt), ip);
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

TEST_CASE("DevicesModule: a plain WLED packet lists a WLED device with its name") {
    DevicesModule dev;
    inject(dev, "wled-desk", /*mm=*/false, 192, 168, 1, 50);
    std::string row = rowFor(dev, "192.168.1.50");
    CHECK(std::strstr(row.c_str(), "\"type\":\"WLED\"") != nullptr);
    CHECK(std::strstr(row.c_str(), "wled-desk") != nullptr);
}

TEST_CASE("DevicesModule: a projectMM-marked packet lists a projectMM device") {
    DevicesModule dev;
    inject(dev, "MM-Bench", /*mm=*/true, 192, 168, 1, 60);
    std::string row = rowFor(dev, "192.168.1.60");
    CHECK(std::strstr(row.c_str(), "\"type\":\"projectMM\"") != nullptr);
    CHECK(std::strstr(row.c_str(), "MM-Bench") != nullptr);
}

TEST_CASE("DevicesModule: a short / garbage datagram is ignored, never listed") {
    DevicesModule dev;
    const uint8_t ip[4] = {192, 168, 1, 70};
    const uint8_t garbage[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    dev.injectPacketForTest(garbage, sizeof(garbage), ip);
    CHECK(dev.listRowCount() == 0);
}

// The P4-bench bug: two DIFFERENT devices (a WLED and a projectMM peer) must each keep
// their OWN name + type — no cross-contamination between packets.
TEST_CASE("DevicesModule: distinct devices don't cross-contaminate name or type") {
    DevicesModule dev;
    inject(dev, "wled-desk", /*mm=*/false, 192, 168, 1, 186);  // a WLED
    inject(dev, "MM-S3",     /*mm=*/true,  192, 168, 1, 157);  // a projectMM peer

    std::string wled = rowFor(dev, "192.168.1.186");
    std::string mm   = rowFor(dev, "192.168.1.157");
    CHECK(std::strstr(wled.c_str(), "wled-desk") != nullptr);
    CHECK(std::strstr(wled.c_str(), "\"type\":\"WLED\"") != nullptr);
    CHECK(std::strstr(wled.c_str(), "MM-S3") == nullptr);   // the contamination bug
    CHECK(std::strstr(mm.c_str(), "MM-S3") != nullptr);
    CHECK(std::strstr(mm.c_str(), "\"type\":\"projectMM\"") != nullptr);
}

// A peer RENAME must propagate: a later packet from the same IP with a new name updates
// the row in place — the live-update requirement (the name rides the presence packet).
TEST_CASE("DevicesModule: a peer rename updates the existing row's name") {
    DevicesModule dev;
    inject(dev, "MM-OldName", /*mm=*/true, 192, 168, 1, 100);
    REQUIRE(std::strstr(rowFor(dev, "192.168.1.100").c_str(), "MM-OldName") != nullptr);
    inject(dev, "MM-NewName", /*mm=*/true, 192, 168, 1, 100);   // same device, new name
    std::string row = rowFor(dev, "192.168.1.100");
    CHECK(std::strstr(row.c_str(), "MM-NewName") != nullptr);
    CHECK(std::strstr(row.c_str(), "MM-OldName") == nullptr);
}

// A projectMM device stays projectMM even when a later plain-WLED packet arrives from the
// same address — the type only RAISES toward projectMM, never downgrades. (A projectMM peer
// could be seen via an unmarked packet too; that must not relabel it WLED.)
TEST_CASE("DevicesModule: a projectMM device is not downgraded by a later WLED packet") {
    DevicesModule dev;
    inject(dev, "MM-Peer", /*mm=*/true,  192, 168, 1, 90);   // first: a projectMM-marked packet
    inject(dev, "MM-Peer", /*mm=*/false, 192, 168, 1, 90);   // later: a plain WLED packet, same IP
    std::string row = rowFor(dev, "192.168.1.90");
    CHECK(std::strstr(row.c_str(), "\"type\":\"projectMM\"") != nullptr);
    CHECK(std::strstr(row.c_str(), "\"type\":\"WLED\"") == nullptr);
}
