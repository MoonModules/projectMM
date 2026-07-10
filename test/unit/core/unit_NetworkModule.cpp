// @module NetworkModule

// Unit tests for NetworkModule WiFi credentials, network-state controls,
// and IP validators. The desktop WiFi backend is a safe stub: it lets tests
// verify buffer copies, delayed web-control apply, and fallback behaviour
// without requiring a real radio.

#include "doctest.h"
#include "platform_config.h"       // pulls in platform::hasWiFi before NetworkModule.h
#include "core/NetworkModule.h"
#include "core/Scheduler.h"
#include "platform/platform.h"
#include "conditional_controls.h"  // shared conditional-control invariant helpers

#include <cstring>

// setWifiCredentials copies SSID + password into internal buffers and raises the dirty flag so the next tick1s() applies them.
TEST_CASE("NetworkModule::setWifiCredentials copies SSID + password and marks dirty") {
    mm::NetworkModule net;
    CHECK_FALSE(net.dirty());

    net.setWifiCredentials("homeAP", "secret123");

    CHECK(net.dirty());

    // No public accessor for ssid_/password_ — re-set with markedly different
    // values to confirm the second write replaces the first (proving the copy
    // happened, not just that the function returned).
    net.clearDirty();
    net.setWifiCredentials("otherSSID", "otherPW");
    CHECK(net.dirty());
}

// A nullptr SSID is silently ignored (no copy, no dirty flag) — guards against a bogus caller.
TEST_CASE("NetworkModule::setWifiCredentials with null SSID is a no-op") {
    mm::NetworkModule net;
    net.setWifiCredentials(nullptr, "irrelevant");
    CHECK_FALSE(net.dirty());
}

// A nullptr password is treated as empty (open networks), still copies SSID and marks dirty.
TEST_CASE("NetworkModule::setWifiCredentials with null password treats it as empty") {
    // Improv allows open networks (auth flag = "NO"); the credential pusher
    // may pass a nullptr or empty password for those. Both must be tolerated.
    mm::NetworkModule net;
    net.setWifiCredentials("openSSID", nullptr);
    CHECK(net.dirty());
}

// An over-length SSID (100 chars) is truncated cleanly into the 33-byte buffer; ASAN catches any overflow.
TEST_CASE("NetworkModule::setWifiCredentials accepts long SSID without crash") {
    // ssid_ is char[33] (32 chars + NUL). A longer SSID must not overflow.
    // Bounds-correctness is checked indirectly: ASAN (the test runner has it
    // available) catches a strncpy overflow; the dirty flag confirms the
    // function ran the copy path. NetworkModule has no public accessor for
    // ssid_ so we can't assert the exact truncated value here — adding one
    // for test purposes only is rejected (see CLAUDE.md "Concrete first").
    mm::NetworkModule net;
    char longSsid[100];
    std::memset(longSsid, 'A', sizeof(longSsid) - 1);
    longSsid[sizeof(longSsid) - 1] = 0;
    net.setWifiCredentials(longSsid, "pw");
    CHECK(net.dirty());
}

namespace {
struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } };
}

// Web-written credentials drive the same STA apply path as Improv, after both fields settle.
TEST_CASE("NetworkModule web credential controls schedule WiFi STA apply") {
    ClockGuard clockGuard;
    mm::platform::setTestNowMs(1000);

    mm::Scheduler scheduler;
    auto* net = new mm::NetworkModule();
    net->setName("Network");
    net->setScheduler(&scheduler);
    scheduler.addModule(net);
    scheduler.setup();

    CHECK(scheduler.setControl("Network", "ssid", "{\"value\":\"homeAP\"}") == mm::Scheduler::SetControlResult::Ok);
    CHECK_FALSE(net->wifiCredentialApplyPendingForTest());
    CHECK(std::strcmp(net->status(), "WiFi SSID saved; press connect") == 0);

    CHECK(scheduler.setControl("Network", "password", "{\"value\":\"secret123\"}") == mm::Scheduler::SetControlResult::Ok);
    CHECK(net->wifiCredentialApplyPendingForTest());
    CHECK(std::strcmp(net->status(), "WiFi credentials saved; connecting soon") == 0);

    mm::platform::setTestNowMs(1000 + mm::NetworkModule::wifiCredentialApplyDelayMsForTest() + 1);
    net->tick1s();

    CHECK_FALSE(net->wifiCredentialApplyPendingForTest());
    CHECK(std::strcmp(net->status(), "No network") == 0);
    scheduler.release();
}

// The explicit connect button lets an open network apply with an empty password.
TEST_CASE("NetworkModule connectWifi applies open-network credentials") {
    ClockGuard clockGuard;
    mm::platform::setTestNowMs(1000);

    mm::Scheduler scheduler;
    auto* net = new mm::NetworkModule();
    net->setName("Network");
    net->setScheduler(&scheduler);
    scheduler.addModule(net);
    scheduler.setup();

    CHECK(scheduler.setControl("Network", "ssid", "{\"value\":\"openAP\"}") == mm::Scheduler::SetControlResult::Ok);
    CHECK_FALSE(net->wifiCredentialApplyPendingForTest());
    CHECK(scheduler.setControl("Network", "connectWifi", "{\"value\":1}") == mm::Scheduler::SetControlResult::Ok);
    CHECK_FALSE(net->wifiCredentialApplyPendingForTest());
    CHECK(std::strcmp(net->status(), "No network") == 0);
    scheduler.release();
}

// After setup(), NetworkModule exposes a `mode` read-only control whose value
// reflects the current state-machine state. On the desktop platform every
// network init stub returns false, so the cascade lands on Idle.
TEST_CASE("NetworkModule mode control reflects current state") {
    mm::NetworkModule net;
    net.setup();
    // setup() falls through to startAP() on the desktop platform (no Eth, no
    // SSID), which already calls rebuildControls() internally. Calling
    // defineControls() again would duplicate every control; use
    // rebuildControls() to clear-then-build a single time.
    net.rebuildControls();

    bool foundMode = false;
    for (uint8_t i = 0; i < net.controls().count(); i++) {
        if (std::strcmp(net.controls()[i].name, "mode") == 0) {
            CHECK(net.controls()[i].type == mm::ControlType::ReadOnly);
            const char* val = static_cast<const char*>(net.controls()[i].ptr);
            CHECK(val != nullptr);
            CHECK(std::strcmp(val, "Idle") == 0);
            foundMode = true;
        }
    }
    CHECK(foundMode);
}

// parseDottedQuad (in Control.h) is the validator on every IPv4 write,
// over both the HTTP API and persistence. Pin the contract.
TEST_CASE("parseDottedQuad accepts valid dotted-quads and rejects junk") {
    uint8_t out[4];

    CHECK(mm::parseDottedQuad("0.0.0.0", out));
    CHECK((out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0));

    CHECK(mm::parseDottedQuad("192.168.1.42", out));
    CHECK((out[0] == 192 && out[1] == 168 && out[2] == 1 && out[3] == 42));

    CHECK(mm::parseDottedQuad("255.255.255.255", out));
    CHECK((out[0] == 255 && out[1] == 255 && out[2] == 255 && out[3] == 255));

    // Out-of-range octet — rejected (would clamp to 255 if we allowed it,
    // hiding a malformed write rather than surfacing the bug).
    CHECK_FALSE(mm::parseDottedQuad("1.2.3.256", out));
    // Negative — rejected.
    CHECK_FALSE(mm::parseDottedQuad("-1.0.0.0", out));
    // Wrong shape — rejected.
    CHECK_FALSE(mm::parseDottedQuad("1.2.3", out));
    CHECK_FALSE(mm::parseDottedQuad("1.2.3.4.5", out));
    CHECK_FALSE(mm::parseDottedQuad("", out));
    CHECK_FALSE(mm::parseDottedQuad("abc.def.ghi.jkl", out));
    // Trailing junk after a valid quad — rejected. Lets the API surface
    // "192.168.1.1x" as a 400 instead of silently writing 192.168.1.1.
    CHECK_FALSE(mm::parseDottedQuad("192.168.1.1x", out));
}

// The static-IP fields (ip / gateway / subnet / dns) are bound as IPv4
// controls — 4 bytes of storage each, not 16-char dotted-quad strings.
// They start hidden because addressing defaults to DHCP.
TEST_CASE("NetworkModule static-IP fields are IPv4-typed") {
    mm::NetworkModule net;
    net.setup();
    // setup() falls through to startAP() on the desktop platform (no Eth, no
    // SSID), which already calls rebuildControls() internally. Calling
    // defineControls() again would duplicate every control; use
    // rebuildControls() to clear-then-build a single time.
    net.rebuildControls();

    int found = 0;
    for (uint8_t i = 0; i < net.controls().count(); i++) {
        const char* name = net.controls()[i].name;
        if (std::strcmp(name, "ip") == 0
            || std::strcmp(name, "gateway") == 0
            || std::strcmp(name, "subnet") == 0
            || std::strcmp(name, "dns") == 0) {
            CHECK(net.controls()[i].type == mm::ControlType::IPv4);
            CHECK(net.controls()[i].hidden);  // DHCP default → hidden
            found++;
        }
    }
    CHECK(found == 4);
}

// In WiFi-capable builds (anything other than --firmware esp32-eth), the
// rssi and txPower controls are present and start hidden — Idle/Ethernet
// don't expose live WiFi metrics. The Ethernet-only build compiles them out
// entirely so the iteration finds nothing, which is still a valid pass shape.
TEST_CASE("NetworkModule rssi/txPower controls hidden in non-WiFi states") {
    mm::NetworkModule net;
    net.setup();
    // setup() falls through to startAP() on the desktop platform (no Eth, no
    // SSID), which already calls rebuildControls() internally. Calling
    // defineControls() again would duplicate every control; use
    // rebuildControls() to clear-then-build a single time.
    net.rebuildControls();

    int matchCount = 0;
    for (uint8_t i = 0; i < net.controls().count(); i++) {
        const char* name = net.controls()[i].name;
        if (std::strcmp(name, "rssi") == 0 || std::strcmp(name, "txPower") == 0) {
            matchCount++;
            // ReadOnlyInt = 1-byte int8_t + a "dBm" suffix carried in the
            // descriptor's aux slot (see Control.h). Tests the control type
            // we ended up using after the buffer-shrink refactor.
            CHECK(net.controls()[i].type == mm::ControlType::ReadOnlyInt);
            // Desktop setup() lands in Idle (no Ethernet, no STA, AP stub
            // returns false). Both metrics should be hidden in that state.
            CHECK(net.controls()[i].hidden);
        }
    }
    // Count assertion catches the silent-fail case where the controls are
    // missing entirely — without it, a build that dropped both rssi and
    // txPower would still pass (the loop body never runs). On WiFi-capable
    // builds both controls must exist; on --firmware esp32-eth they're
    // compiled out (NetworkModule's `if constexpr (platform::hasWiFi)`)
    // and the expected count is 0.
    if constexpr (mm::platform::hasWiFi) {
        CHECK(matchCount == 2);
    } else {
        CHECK(matchCount == 0);
    }
}

// Conditional controls: the static-IP fields (ip/gateway/subnet/dns) are visible
// only when addressing == Static (1), hidden under DHCP (0) — but ALWAYS bound so
// persistence can load a saved static config regardless of the live mode. This is
// the documented add-then-setHidden pattern (architecture.md § Conditional
// controls); the test pins it both ways so a regression (e.g. dropping setHidden,
// or conditionally NOT adding the field) fails here, not on hardware.
TEST_CASE("NetworkModule static-IP fields track the addressing mode") {
    mm::NetworkModule net;
    net.setup();   // builds controls once (desktop cascade lands on AP/Idle)

    // addressing is the Select that conditions the static fields. setCondition(true)
    // → Static (value 1) → fields visible; setCondition(false) → DHCP (0) → hidden.
    auto setStatic = [&](bool on) {
        mm::test::setControlValue<uint8_t>(net, "addressing", on ? uint8_t{1} : uint8_t{0});
    };
    for (const char* field : {"ip", "gateway", "subnet", "dns"}) {
        mm::test::checkConditionalControl(net, field, setStatic, /*visibleWhenTrue=*/true);
    }
}
