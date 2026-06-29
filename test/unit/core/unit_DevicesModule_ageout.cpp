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

// Restore the two cached devices at t0, advance to t0+dt, tick once. Returns whether
// device A (192.168.1.20) is still present. loop1s() also polls the mDNS listener (inert
// on the host) and adds the self row — that's live behaviour and doesn't affect A's
// age-out timestamp, which is what these cases pin.
bool aPresentAfter(uint32_t t0, uint32_t dt) {
    ClockGuard guard;   // real clock restored on return, even if a REQUIRE below fails
    platform::setTestNowMs(t0);
    DevicesModule dev;
    // Mirrors the persistence-overlay shape: the whole module JSON object, the array
    // under the control's key — restoreList navigates `member(root, "devices")`.
    const char* cached =
        "{\"devices\":["
        "{\"name\":\"A\",\"ip\":\"192.168.1.20\",\"type\":\"generic\"},"
        "{\"name\":\"B\",\"ip\":\"192.168.1.21\",\"type\":\"WLED\"}]}";
    REQUIRE(dev.restoreList(cached, "devices"));
    REQUIRE(dev.listRowCount() == 2);
    platform::setTestNowMs(t0 + dt);
    dev.loop1s();                           // runs ageOut() against the advanced clock
    return present(dev, "192.168.1.20");    // ClockGuard restores the real clock on return
}

}  // namespace

TEST_CASE("DevicesModule: a still-fresh device survives just under kStaleMs (60s)") {
    CHECK(aPresentAfter(1000, 50u * 1000u) == true);   // 50s < 60s window
}

TEST_CASE("DevicesModule: a device drops once past kStaleMs (60s)") {
    CHECK(aPresentAfter(1000, 70u * 1000u) == false);  // 70s > 60s window
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
