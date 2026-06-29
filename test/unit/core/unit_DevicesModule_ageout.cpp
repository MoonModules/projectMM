// @module DevicesModule

// Pins the timestamp-based age-out: a discovered device that stops being re-announced
// over mDNS is dropped after kStaleMs, while a still-fresh device and the self entry
// stay. Discovery is mDNS (each device re-announces; the listener re-queries each
// service periodically), so freshness is a per-device lastSeenMs timestamp and the
// drop is an "unheard too long" check. Virtual time (platform::setTestNowMs) drives it
// deterministically, no network or wall clock.
//
// The module's age-out runs in loop1s(); the test restores a cached list (the public
// persistence entry point), advances virtual time, and ticks loop1s() to observe which
// rows survive via listRowCount(). The mDNS listener loop1s() also polls is inert here
// (no network / no mDNS on the host), so the only state change the test exercises is
// age-out.

#include "doctest.h"
#include "core/DevicesModule.h"
#include "core/JsonSink.h"
#include "platform/platform.h"

#include <cstdint>
#include <cstring>

using namespace mm;

namespace {

// RAII: restore the real clock on scope exit, even if a REQUIRE/CHECK throws mid-test
// — otherwise a failing assertion would leave virtual time frozen for later cases.
struct ClockGuard { ~ClockGuard() { platform::setTestNowMs(0); } };

// True if the cached device at `ip` is still in the list (a serialized-row scan, the
// only public window into the rows). The self entry the live tick adds is ignored —
// we assert specifically on the two restored, non-self devices' survival.
bool present(const DevicesModule& dev, const char* ip) {
    for (uint8_t i = 0; i < dev.listRowCount(); i++) {
        mm::JsonSink sink;
        dev.writeListRow(sink, i);
        if (std::strstr(sink.data(), ip)) return true;
    }
    return false;
}

// Restore two CACHED devices at t0; optionally re-confirm A with a live packet (so it's
// promoted off `cached`); advance to t0+dt; tick once. Returns whether A (192.168.1.20) is
// still present. A cached device is on a short probation; a live-confirmed one gets 24 h.
bool aPresentAfter(uint32_t t0, uint32_t dt, bool reconfirmA) {
    ClockGuard guard;   // real clock restored on return, even if a REQUIRE below fails
    platform::setTestNowMs(t0);
    DevicesModule dev;
    // Mirrors the persistence-overlay shape: the whole module JSON object, the array
    // under the control's key — restoreList navigates `member(root, "devices")`.
    const char* cached =
        "{\"devices\":["
        "{\"name\":\"A\",\"ip\":\"192.168.1.20\",\"type\":\"WLED\"},"
        "{\"name\":\"B\",\"ip\":\"192.168.1.21\",\"type\":\"WLED\"}]}";
    REQUIRE(dev.restoreList(cached, "devices"));
    REQUIRE(dev.listRowCount() == 2);
    if (reconfirmA) {
        // A live WLED presence packet from A's IP — clears `cached`, gives it the 24 h window.
        const uint8_t ip[4] = {192, 168, 1, 20};
        uint8_t pkt[WledPacket::kSize];
        WledPacket::build(pkt, ip, "A", /*boardType=*/34, /*lightsOn=*/true);
        dev.injectPacketForTest(pkt, sizeof(pkt), ip);
    }
    platform::setTestNowMs(t0 + dt);
    dev.loop1s();                           // runs ageOut() against the advanced clock
    return present(dev, "192.168.1.20");    // ClockGuard restores the real clock on return
}

}  // namespace

// A cached (restored-but-never-re-heard) device is on a short probation, NOT the full 24 h
// — else a long-gone persisted device would survive forever across reboots (its clock
// resets to "boot" each restore). It drops once past kCachedGraceMs.
TEST_CASE("DevicesModule: a cached device survives just under the probation window") {
    CHECK(aPresentAfter(1000, 50u * 1000u, /*reconfirmA=*/false) == true);   // 50s < 60s probation
}

TEST_CASE("DevicesModule: a cached device drops once past the probation window") {
    CHECK(aPresentAfter(1000, 70u * 1000u, /*reconfirmA=*/false) == false);  // 70s > 60s probation
}

// A live-confirmed device (a presence packet cleared its `cached` flag) gets the full 24 h.
TEST_CASE("DevicesModule: a live-confirmed device survives well past probation (to 24h)") {
    CHECK(aPresentAfter(1000, 23u * 60u * 60u * 1000u, /*reconfirmA=*/true) == true);   // 23h < 24h
}

TEST_CASE("DevicesModule: a live-confirmed device drops once past kStaleMs (24h)") {
    CHECK(aPresentAfter(1000, 25u * 60u * 60u * 1000u, /*reconfirmA=*/true) == false);  // 25h > 24h
}

// A projectMM device advertises BOTH _http._tcp (mm=1) and _wled._tcp, so the WLED
// plugin would relabel it WLED on its _wled hit. Pin that projectMM wins: restore a
// device as projectMM, then confirm the serialized type stays projectMM (the type
// priority lives in upsertDevice, exercised through the public list).
TEST_CASE("DevicesModule: a projectMM device is not downgraded to WLED") {
    ClockGuard guard;
    platform::setTestNowMs(1);
    DevicesModule dev;
    const char* cached =
        "{\"devices\":[{\"name\":\"MM-Bench\",\"ip\":\"192.168.1.30\",\"type\":\"projectMM\"}]}";
    REQUIRE(dev.restoreList(cached, "devices"));
    REQUIRE(dev.listRowCount() == 1);
    // Serialize and confirm it reads back as projectMM, not WLED.
    mm::JsonSink sink;
    dev.writeListRow(sink, 0);
    CHECK(std::strstr(sink.data(), "\"type\":\"projectMM\"") != nullptr);
    CHECK(std::strstr(sink.data(), "\"type\":\"WLED\"") == nullptr);
}

TEST_CASE("DevicesModule: restore tolerates an empty / malformed cache") {
    // Robustness: a malformed array yields an empty list, never a crash.
    ClockGuard guard;   // restores the real clock on return, even if a CHECK fails
    platform::setTestNowMs(1);
    DevicesModule dev;
    CHECK(dev.restoreList("not json", "devices") == false);
    CHECK(dev.listRowCount() == 0);
}
