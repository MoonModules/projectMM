// @module Control
// @also FilesystemModule

// Regression test for the persistence-overlay bug that silently zeroed a control's
// non-zero default when a saved JSON file omitted that control's key.
//
// The bug: applyControlValue() called mm::json::parseInt(json, key), which returns
// 0 for an ABSENT key — indistinguishable from a real 0 — and then wrote that 0
// into the control (Clamp policy). So loading an older/partial NetworkModule.json
// (one written before a control existed, or that simply didn't include it) clobbered
// the control's chip default. On the ESP32-P4 this zeroed eth `ethType` (default 2 =
// IP101) to 0 (= none), so ethInit() dispatched to "no Ethernet" and the board got a
// link but never a DHCP lease. The fix: applyControlValue() skips absent keys
// (mm::json::hasKey guard), leaving the control at its current value.
//
// These tests pin the contract at the unit level so the regression can't recur:
// an absent key must NEVER mutate a control; a present key still applies.

#include "doctest.h"
#include "core/Control.h"
#include "core/JsonUtil.h"

#include <cstdint>
#include <cstring>

// hasKey distinguishes an absent key from one whose value is 0 — the capability the
// fix relies on. parseInt alone can't (returns 0 for both).
TEST_CASE("json::hasKey detects presence independent of value") {
    const char* json = "{\"a\":0,\"b\":5}";
    CHECK(mm::json::hasKey(json, "a"));        // present, value 0
    CHECK(mm::json::hasKey(json, "b"));        // present, value 5
    CHECK_FALSE(mm::json::hasKey(json, "c"));  // absent
    CHECK_FALSE(mm::json::hasKey(json, ""));
    CHECK_FALSE(mm::json::hasKey(nullptr, "a"));
}

// The core regression: a control bound with a non-zero value, overlaid with a JSON
// that does NOT contain its key, must keep its value — not snap to 0.
TEST_CASE("applyControlValue leaves a control untouched when its key is absent") {
    mm::ControlList controls;

    // Mirror the eth controls that triggered the bug: a Select (ethType, default 2)
    // and an Int16 (a pin, default 31), plus a Uint8 and a Bool for coverage.
    uint8_t  ethType = 2;                 // IP101 — the value that got zeroed on P4
    int16_t  mdcGpio = 31;
    uint8_t  small   = 7;
    bool      flag   = true;
    static const char* const opts[] = {"None", "LAN8720", "IP101", "W5500"};
    controls.addSelect("ethType", ethType, opts, 4);
    controls.addInt16("ethMdcGpio", mdcGpio, -1, 48);
    controls.addUint8("small", small, 0, 100);
    controls.addBool("flag", flag);

    // A persisted file that contains an UNRELATED key only — none of our controls.
    const char* partialJson = "{\"ssid\":\"home\"}";

    // Clamp policy is what the persistence overlay uses. Every absent key must be a
    // no-op (Ok, value preserved) — this is exactly what failed before the fix.
    for (uint8_t i = 0; i < controls.count(); i++) {
        auto r = mm::applyControlValue(controls[i], partialJson, controls[i].name,
                                       mm::ApplyPolicy::Clamp);
        CHECK(r == mm::ApplyResult::Ok);
    }
    CHECK(ethType == 2);    // NOT zeroed — the bug would have made this 0
    CHECK(mdcGpio == 31);
    CHECK(small == 7);
    CHECK(flag == true);
}

// A present key still applies (the fix must not break the normal load path).
TEST_CASE("applyControlValue still applies a present key") {
    mm::ControlList controls;
    uint8_t ethType = 2;
    int16_t mdcGpio = 31;
    static const char* const opts[] = {"None", "LAN8720", "IP101", "W5500"};
    controls.addSelect("ethType", ethType, opts, 4);
    controls.addInt16("ethMdcGpio", mdcGpio, -1, 48);

    // Saved file carries new values for both.
    const char* json = "{\"ethType\":3,\"ethMdcGpio\":23}";
    CHECK(mm::applyControlValue(controls[0], json, "ethType", mm::ApplyPolicy::Clamp)
          == mm::ApplyResult::Ok);
    CHECK(mm::applyControlValue(controls[1], json, "ethMdcGpio", mm::ApplyPolicy::Clamp)
          == mm::ApplyResult::Ok);
    CHECK(ethType == 3);    // applied
    CHECK(mdcGpio == 23);   // applied
}

// A present key whose value IS 0 must apply the 0 (don't confuse "present 0" with
// "absent"). Guards against an over-eager fix that skipped on value rather than key.
TEST_CASE("applyControlValue applies an explicit zero when the key is present") {
    mm::ControlList controls;
    uint8_t ethType = 2;
    static const char* const opts[] = {"None", "LAN8720", "IP101", "W5500"};
    controls.addSelect("ethType", ethType, opts, 4);

    const char* json = "{\"ethType\":0}";   // explicitly set to None
    CHECK(mm::applyControlValue(controls[0], json, "ethType", mm::ApplyPolicy::Clamp)
          == mm::ApplyResult::Ok);
    CHECK(ethType == 0);    // explicit 0 IS applied
}

// A per-control validator (ControlDescriptor::validate) runs on EVERY write path —
// the backend home for input rules that used to live in a bespoke per-transport RPC
// (e.g. deviceModel's printable-ASCII check, formerly the SET_DEVICE_MODEL Improv RPC).
// A reject returns Malformed and leaves the stored value untouched (no partial write);
// any transport (HTTP, APPLY_OP over serial, persistence) gets the check for free.
static bool acceptPrintableAscii(const char* v) {
    if (!v) return false;
    size_t n = std::strlen(v);
    if (n == 0 || n >= 32) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char b = static_cast<unsigned char>(v[i]);
        if (b < 0x20 || b > 0x7E) return false;
    }
    return true;
}

TEST_CASE("a per-control validator accepts a valid value and rejects bad input") {
    mm::ControlList controls;
    char deviceModel[32] = "initial";
    controls.addText("deviceModel", deviceModel, sizeof(deviceModel), acceptPrintableAscii);

    // Valid printable-ASCII → applied.
    CHECK(mm::applyControlValue(controls[0], "{\"deviceModel\":\"LOLIN D32\"}",
                                "deviceModel", mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(std::strcmp(deviceModel, "LOLIN D32") == 0);

    // A raw non-printable byte embedded in the value (0x01) — parseString copies bytes
    // verbatim (it only un-escapes \" and \\), so a wire-untrusted control byte reaches
    // the validator, which rejects it → Malformed, prior value preserved (no partial write).
    const char bad[] = {'{','"','d','e','v','i','c','e','M','o','d','e','l','"',':','"',
                        'b','a','d', 0x01, 'x','"','}', 0};
    CHECK(mm::applyControlValue(controls[0], bad,
                                "deviceModel", mm::ApplyPolicy::Clamp) == mm::ApplyResult::Malformed);
    CHECK(std::strcmp(deviceModel, "LOLIN D32") == 0);   // unchanged

    // Empty string → Malformed (the validator rejects 0-length), prior value preserved.
    CHECK(mm::applyControlValue(controls[0], "{\"deviceModel\":\"\"}",
                                "deviceModel", mm::ApplyPolicy::Clamp) == mm::ApplyResult::Malformed);
    CHECK(std::strcmp(deviceModel, "LOLIN D32") == 0);
}

TEST_CASE("a Text control with no validator accepts anything that fits") {
    mm::ControlList controls;
    char label[16] = {};
    controls.addText("label", label, sizeof(label));   // no validator

    CHECK(mm::applyControlValue(controls[0], "{\"label\":\"hi\"}",
                                "label", mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(std::strcmp(label, "hi") == 0);
}
