// @module InfraredService
// @also Scheduler

// Pins the infrared service's mapping path: a learned remote code drives another module's control
// through the shared Scheduler::setControl primitive, clamped to the control's own bounds. A fake
// "Drivers" module stands in for the real one, so the test needs no light-domain modules.
//
// The reception side is a platform stub (irRead returns false on the host), so a code is injected
// the way a decoded frame would arrive. That is the whole point of injectCodeForTest.

#include "doctest.h"
#include "core/InfraredService.h"
#include "core/Scheduler.h"
#include "core/MoonModule.h"
#include "core/JsonSink.h"
#include "core/JsonUtil.h"

#include <cstring>

using namespace mm;

namespace {

// Stands in for Drivers: an `on` Bool, a brightness Uint8 (0-255) and a palette Select (0-3). Named
// "Drivers" so a row targeting "Drivers.on" resolves to it.
struct FakeDrivers : public MoonModule {
    bool on = true;
    uint8_t brightness = 100;
    uint8_t palette = 1;
    void defineControls() override {
        controls_.addControl("on", on);
        controls_.addControl("brightness", brightness, 0, 255);
        // A Select's max is (optionCount - 1); addSelect binds min 0 / max count-1.
        static const char* kPalettes[] = {"A", "B", "C", "D"};
        controls_.addSelect("palette", palette, kPalettes, 4);
    }
};

// Scheduler + FakeDrivers + the service, set up so Scheduler::instance() is live and controls are
// bound. The scheduler owns the heap-allocated modules.
struct Rig {
    Scheduler scheduler;
    FakeDrivers* drivers = new FakeDrivers();
    InfraredService* ir = new InfraredService();
    Rig() {
        drivers->setName("Drivers");
        ir->setName("Ir");
        scheduler.addModule(drivers);
        scheduler.addModule(ir);
        scheduler.setup();   // binds controls + sets Scheduler::instance()
    }
    ~Rig() { scheduler.release(); }

    /// Add a row bound to `target`, and return its id.
    uint32_t addRow(const char* target, const char* kind = "toggle", int value = 0) {
        uint32_t id = 0;
        REQUIRE(ir->addListRow(id));
        char v[64];
        std::snprintf(v, sizeof(v), "{\"value\":\"%s\"}", target);
        REQUIRE(ir->setListRowField(id, "target", v));
        std::snprintf(v, sizeof(v), "{\"value\":\"%s\"}", kind);
        REQUIRE(ir->setListRowField(id, "kind", v));
        std::snprintf(v, sizeof(v), "{\"value\":%d}", value);
        REQUIRE(ir->setListRowField(id, "value", v));
        return id;
    }

    /// Arm a row for learning, then deliver a code: the on-device flow, where the next frame binds.
    void learn(uint32_t id, uint32_t code) {
        REQUIRE(ir->setListRowField(id, "learn", "{\"value\":true}"));
        ir->injectCodeForTest(code);
    }
    void fire(uint32_t code) { ir->injectCodeForTest(code); }

    /// The id of row `n`, read from the row itself rather than assumed: ids are handed out in
    /// sequence and every earlier test in this file consumes some, so a literal would be brittle.
    uint32_t rowId(uint8_t n) const {
        char buf[256];
        JsonSink sink(buf, sizeof(buf));
        ir->writeListRow(sink, n);
        return static_cast<uint32_t>(mm::json::parseInt(buf, "id"));
    }
};

}  // namespace

TEST_CASE("a learned code toggles the control its row targets") {
    Rig rig;
    const uint32_t id = rig.addRow("Drivers.on", "toggle");
    rig.learn(id, 0x40BF);

    CHECK(rig.drivers->on == true);
    rig.fire(0x40BF);
    CHECK(rig.drivers->on == false);   // toggle reads the current value and writes its inverse
    rig.fire(0x40BF);
    CHECK(rig.drivers->on == true);    // and back, which a +1 delta could never do
}

TEST_CASE("a delta row nudges its target and stops at the control's own bounds") {
    Rig rig;
    const uint32_t up = rig.addRow("Drivers.brightness", "delta", 16);
    const uint32_t dn = rig.addRow("Drivers.brightness", "delta", -16);
    rig.learn(up, 0x1111);
    rig.learn(dn, 0x2222);

    rig.fire(0x1111);
    CHECK(rig.drivers->brightness == 116);
    rig.fire(0x2222);
    CHECK(rig.drivers->brightness == 100);

    // The clamp is the CONTROL's, not the row's: the row says +16 and the control says 255, so the
    // control wins. Without this a held key would wrap a uint8 back to 0.
    for (int i = 0; i < 20; i++) rig.fire(0x1111);
    CHECK(rig.drivers->brightness == 255);
    for (int i = 0; i < 40; i++) rig.fire(0x2222);
    CHECK(rig.drivers->brightness == 0);
}

TEST_CASE("a delta row steps a select and clamps at both ends") {
    Rig rig;
    const uint32_t next = rig.addRow("Drivers.palette", "delta", 1);
    const uint32_t prev = rig.addRow("Drivers.palette", "delta", -1);
    rig.learn(next, 0xAAAA);
    rig.learn(prev, 0xBBBB);

    CHECK(rig.drivers->palette == 1);
    rig.fire(0xAAAA);
    CHECK(rig.drivers->palette == 2);
    // A 4-option select's max is 3, so stepping past it holds rather than wrapping into a
    // nonexistent option.
    rig.fire(0xAAAA); rig.fire(0xAAAA); rig.fire(0xAAAA);
    CHECK(rig.drivers->palette == 3);
    for (int i = 0; i < 6; i++) rig.fire(0xBBBB);
    CHECK(rig.drivers->palette == 0);
}

TEST_CASE("learning binds the next code to the armed row, and only that row") {
    Rig rig;
    const uint32_t a = rig.addRow("Drivers.on", "toggle");
    const uint32_t b = rig.addRow("Drivers.brightness", "delta", 10);

    rig.learn(a, 0x1234);
    rig.learn(b, 0x5678);

    rig.fire(0x1234);
    CHECK(rig.drivers->on == false);            // a's code drove a
    CHECK(rig.drivers->brightness == 100);      // and left b alone
    rig.fire(0x5678);
    CHECK(rig.drivers->brightness == 110);
}

TEST_CASE("arming a row disarms any other, so one code cannot bind twice") {
    Rig rig;
    const uint32_t a = rig.addRow("Drivers.on", "toggle");
    const uint32_t b = rig.addRow("Drivers.brightness", "delta", 10);

    // Arm a, then arm b without delivering a code: only b should be waiting. Otherwise the next
    // frame binds to whichever row the scan reached first, which is not a user's intent.
    REQUIRE(rig.ir->setListRowField(a, "learn", "{\"value\":true}"));
    REQUIRE(rig.ir->setListRowField(b, "learn", "{\"value\":true}"));
    rig.fire(0x9999);

    rig.fire(0x9999);
    CHECK(rig.drivers->brightness == 110);   // b learned it
    CHECK(rig.drivers->on == true);          // a did not
}

TEST_CASE("an unlearned code is reported and changes nothing") {
    Rig rig;
    const uint32_t id = rig.addRow("Drivers.on", "toggle");
    rig.learn(id, 0x1111);

    rig.fire(0xDEAD);
    CHECK(rig.drivers->on == true);                      // untouched
    CHECK(std::strstr(rig.ir->status(), "unassigned") != nullptr);
    CHECK(rig.ir->latestCode() == 0xDEAD);               // still reported, so a user can bind it
}

TEST_CASE("a row whose target module is gone is a no-op, not a crash") {
    Rig rig;
    const uint32_t id = rig.addRow("Nope.on", "toggle");
    rig.learn(id, 0x4321);
    rig.fire(0x4321);                                    // must not crash
    CHECK(rig.drivers->on == true);
}

TEST_CASE("a row with no target does nothing at all") {
    Rig rig;
    uint32_t id = 0;
    REQUIRE(rig.ir->addListRow(id));
    rig.learn(id, 0x7777);
    rig.fire(0x7777);                                    // an unassigned row is a valid state
    CHECK(rig.drivers->on == true);
}

TEST_CASE("rows are added and deleted at runtime, which is what a fixed action table could not do") {
    Rig rig;
    CHECK(rig.ir->listRowCount() == 0);
    const uint32_t a = rig.addRow("Drivers.on", "toggle");
    const uint32_t b = rig.addRow("Drivers.brightness", "delta", 5);
    CHECK(rig.ir->listRowCount() == 2);

    CHECK(rig.ir->deleteListRow(a));
    CHECK(rig.ir->listRowCount() == 1);
    CHECK_FALSE(rig.ir->deleteListRow(a));   // already gone

    // b survives its sibling's removal, and keeps working: ids are stable, not positions.
    rig.learn(b, 0x0F0F);
    rig.fire(0x0F0F);
    CHECK(rig.drivers->brightness == 105);
}

TEST_CASE("the pin state decides what the service reports about itself") {
    Rig rig;
    rig.ir->prepare();
    // No pin: a warning, because a receiver with no GPIO can never see a code, and saying "ready"
    // there would be a lie a user cannot see through.
    CHECK(std::strstr(rig.ir->status(), "set pin") != nullptr);
}

TEST_CASE("a code that is not a number is refused rather than binding something else") {
    // A typed code is parsed, and a bad one has to be REFUSED: silently keeping whatever prefix
    // parsed would bind a different code entirely, and the user would press the remote, see nothing
    // happen, and find a number in the row they never typed.
    Rig rig;
    uint32_t id = 0;
    REQUIRE(rig.ir->addListRow(id));

    REQUIRE(rig.ir->setListRowField(id, "code", "{\"value\":\"0x40BF\"}"));    // hex
    REQUIRE(rig.ir->setListRowField(id, "code", "{\"value\":\"16575\"}"));     // decimal

    CHECK_FALSE(rig.ir->setListRowField(id, "code", "{\"value\":\"40BF!\"}"));       // trailing junk
    CHECK_FALSE(rig.ir->setListRowField(id, "code", "{\"value\":\"\"}"));            // nothing typed
    CHECK_FALSE(rig.ir->setListRowField(id, "code", "{\"value\":\"nonsense\"}"));    // not a number
    // Past 32 bits: not a frame this receiver can ever decode, so it is a typo rather than a code.
    CHECK_FALSE(rig.ir->setListRowField(id, "code", "{\"value\":\"4294967296\"}"));
    // Longer than the field holds: truncating would parse a DIFFERENT valid number.
    CHECK_FALSE(rig.ir->setListRowField(id, "code", "{\"value\":\"0x00000000000000000040BF\"}"));

    // The last GOOD value survived every refusal: a rejected edit changes nothing.
    rig.fire(16575);
    CHECK(std::strstr(rig.ir->status(), "unassigned") == nullptr);
}

TEST_CASE("a set row is refused on a remote, which has no release to clear it") {
    // `set` means "write while held, clear on release". A remote code is a single event with no
    // release, so running one would latch the control with nothing able to undo it: the row would
    // look like it worked once and then break the control it targeted.
    Rig rig;
    const uint32_t id = rig.addRow("Drivers.brightness", "set", 200);
    rig.learn(id, 0x5150);

    rig.fire(0x5150);
    CHECK(rig.drivers->brightness == 100);          // untouched: not latched at 200
    CHECK(std::strstr(rig.ir->status(), "release") != nullptr);

    // Toggle and delta are unaffected: both are complete in one event.
    const uint32_t d = rig.addRow("Drivers.brightness", "delta", 5);
    rig.learn(d, 0x5151);
    rig.fire(0x5151);
    CHECK(rig.drivers->brightness == 105);
}

TEST_CASE("one remote key binds to one row, so a re-learned key moves rather than duplicates") {
    // Dispatch fires the FIRST row holding a code and stops, so a duplicate is a row that can never
    // run: it reads as bound in the list while the key does another row's action.
    Rig rig;
    const uint32_t a = rig.addRow("Drivers.on", "toggle");
    const uint32_t b = rig.addRow("Drivers.brightness", "delta", 10);

    rig.learn(a, 0x1234);
    rig.learn(b, 0x1234);          // the SAME key, onto the second row

    rig.fire(0x1234);
    CHECK(rig.drivers->brightness == 110);   // the newest binding won
    CHECK(rig.drivers->on == true);          // and the first row no longer holds the code
}

TEST_CASE("an explicit false disarms a row, so a learn can be canceled") {
    // The UI's button sends {"value":""} and means "arm". The API can also send a real boolean, and
    // `false` has to mean disarm: parseString reads only quoted strings, so a JSON boolean left the
    // buffer empty and took the same path as the button, making a row impossible to un-arm.
    Rig rig;
    uint32_t id = 0;
    REQUIRE(rig.ir->addListRow(id));

    REQUIRE(rig.ir->setListRowField(id, "learn", "{\"value\":true}"));
    REQUIRE(rig.ir->setListRowField(id, "learn", "{\"value\":false}"));
    // Disarmed: the next code is NOT captured, so the row stays unbound and reports the code as
    // unassigned rather than silently learning it.
    rig.fire(0x2468);
    CHECK(std::strstr(rig.ir->status(), "unassigned") != nullptr);
}
