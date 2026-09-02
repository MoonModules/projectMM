// @module ButtonService
// @also Scheduler

// Pins the button service's two halves: the debounce state machine, and the mapping row that turns
// a settled edge into a control write. The GPIO seam is a host stub whose level a test injects
// (platform::setTestGpioLevel), so a press is expressed as "the pin reads low for long enough",
// which is exactly what the module sees on a board.

#include "doctest.h"
#include "core/ButtonService.h"
#include "core/Scheduler.h"
#include "core/MoonModule.h"
#include "platform/platform.h"
#include "core/JsonSink.h"

#include <cstring>

using namespace mm;

namespace {

// Stands in for Drivers: an `on` Bool and a brightness Uint8, so a row can target either.
struct FakeDrivers : public MoonModule {
    bool on = true;
    uint8_t brightness = 100;
    // A control WIDER than a byte, and one that goes negative: the surface reads both as a clamped
    // byte, which is right for a fader and wrong for arithmetic on the control itself.
    uint16_t rate = 300;
    int16_t offset = -50;
    void defineControls() override {
        controls_.addControl("on", on);
        controls_.addControl("brightness", brightness, 0, 255);
        controls_.addControl("rate", rate, 0, 1000);
        controls_.addControl("offset", offset, -100, 100);
    }
};

// Stands in for a module carrying a pad grid (the control surface's presets). Only the parts a pad
// target touches: rows that publish a `slot`, and an `activate` field that fires one.
struct FakePads : public MoonModule, public ListSource {
    uint8_t fired = 0;       ///< how many times a pad was activated
    uint8_t firedSlot = 255; ///< which one, so a test can tell pad 1 from pad 3

    void defineControls() override { controls_.addList("presets", *this); }

    bool isEditableList() const override { return true; }
    bool listAsPads() const override { return true; }
    uint8_t listRowCount() const override { return 3; }
    void writeListRow(JsonSink& sink, uint8_t row) const override {
        // Slots 0, 2 and 5: deliberately NOT contiguous, so a test that passes by using the row
        // index instead of the slot would fail here.
        static const uint8_t kSlots[] = {0, 2, 5};
        sink.appendf("{\"id\":%u,\"slot\":%u,\"name\":\"p%u\"}",
                     static_cast<unsigned>(row + 1), static_cast<unsigned>(kSlots[row]),
                     static_cast<unsigned>(row));
    }
    bool setListRowField(uint32_t id, const char* field, const char*) override {
        if (std::strcmp(field, "activate") != 0) return false;
        static const uint8_t kSlots[] = {0, 2, 5};
        fired++;
        firedSlot = kSlots[id - 1];
        return true;
    }
};

constexpr uint8_t kPin = 4;

struct Rig {
    Scheduler scheduler;
    FakeDrivers* drivers = new FakeDrivers();
    FakePads* pads = new FakePads();
    ButtonService* buttons = new ButtonService();
    Rig() {
        platform::clearTestGpioLevel();
        drivers->setName("Drivers");
        pads->setName("Control");
        buttons->setName("Button");
        scheduler.addModule(drivers);
        scheduler.addModule(pads);
        scheduler.addModule(buttons);
        scheduler.setup();
    }
    ~Rig() { scheduler.release(); platform::clearTestGpioLevel(); }

    /// A row on `kPin`, active-low (a switch to ground), targeting `target`.
    uint32_t addRow(const char* target, const char* kind = "toggle", int value = 0) {
        uint32_t id = 0;
        REQUIRE(buttons->addListRow(id));
        char v[64];
        std::snprintf(v, sizeof(v), "{\"value\":%d}", static_cast<int>(kPin));
        REQUIRE(buttons->setListRowField(id, "pin", v));
        std::snprintf(v, sizeof(v), "{\"value\":\"%s\"}", target);
        REQUIRE(buttons->setListRowField(id, "target", v));
        std::snprintf(v, sizeof(v), "{\"value\":\"%s\"}", kind);
        REQUIRE(buttons->setListRowField(id, "kind", v));
        std::snprintf(v, sizeof(v), "{\"value\":%d}", value);
        REQUIRE(buttons->setListRowField(id, "value", v));
        return id;
    }

    /// Hold a level for `ms`, in the 20 ms steps the module is polled at. Active-low, so a pressed
    /// switch reads LOW.
    void hold(bool pressed, int ms) {
        platform::setTestGpioLevel(kPin, !pressed);
        for (int t = 0; t < ms; t += 20) buttons->tick20ms();
    }
};

}  // namespace

TEST_CASE("a press toggles the control its row targets, once per press") {
    Rig rig;
    rig.addRow("Drivers.on", "toggle");
    rig.hold(false, 100);                 // settle unpressed first
    CHECK(rig.drivers->on == true);

    rig.hold(true, 100);
    CHECK(rig.drivers->on == false);      // the press toggled it
    rig.hold(true, 200);
    CHECK(rig.drivers->on == false);      // holding does NOT toggle again
    rig.hold(false, 100);
    CHECK(rig.drivers->on == false);      // and neither does the release
    rig.hold(true, 100);
    CHECK(rig.drivers->on == true);       // the next press does
}

TEST_CASE("a bounce shorter than the debounce window is not a press") {
    Rig rig;
    rig.addRow("Drivers.on", "toggle");
    rig.hold(false, 100);

    // 20 ms of contact against a 25 ms window: a real switch does this on every press, and counting
    // it would toggle the lights twice for one push.
    rig.hold(true, 20);
    rig.hold(false, 100);
    CHECK(rig.drivers->on == true);       // untouched

    // Held past the window, it counts.
    rig.hold(true, 100);
    CHECK(rig.drivers->on == false);
}

TEST_CASE("a momentary row writes while held and clears on release, which is what a pedal needs") {
    Rig rig;
    rig.addRow("Drivers.on", "set", 1);
    rig.hold(false, 100);

    rig.hold(true, 100);
    CHECK(rig.drivers->on == true);       // held: written
    rig.hold(false, 100);
    CHECK(rig.drivers->on == false);      // released: cleared, unlike a toggle
}

TEST_CASE("a delta row nudges its target, clamped by the control") {
    Rig rig;
    rig.addRow("Drivers.brightness", "delta", 25);
    rig.hold(false, 100);

    rig.hold(true, 100);
    CHECK(rig.drivers->brightness == 125);
    rig.hold(false, 100);
    rig.hold(true, 100);
    CHECK(rig.drivers->brightness == 150);

    // Enough presses to run past 255, each one settled: the ceiling is the CONTROL's, not the row's,
    // so a row saying +25 cannot push a uint8 past its declared max or wrap it back to 0.
    for (int i = 0; i < 10; i++) { rig.hold(false, 100); rig.hold(true, 100); }
    CHECK(rig.drivers->brightness == 255);
}

TEST_CASE("two buttons on two pins act independently") {
    Rig rig;
    // The rig's helper wires kPin; the second row needs its own, so it is built by hand.
    rig.addRow("Drivers.on", "toggle");
    uint32_t second = 0;
    REQUIRE(rig.buttons->addListRow(second));
    REQUIRE(rig.buttons->setListRowField(second, "pin", "{\"value\":7}"));
    REQUIRE(rig.buttons->setListRowField(second, "target", "{\"value\":\"Drivers.brightness\"}"));
    REQUIRE(rig.buttons->setListRowField(second, "kind", "{\"value\":\"delta\"}"));
    REQUIRE(rig.buttons->setListRowField(second, "value", "{\"value\":10}"));

    platform::setTestGpioLevel(kPin, true);   // both unpressed (active-low)
    platform::setTestGpioLevel(7, true);
    for (int t = 0; t < 100; t += 20) rig.buttons->tick20ms();

    // Press only the second: the first must not fire. Two buttons bouncing independently is why the
    // debounce state is per row rather than shared.
    platform::setTestGpioLevel(7, false);
    for (int t = 0; t < 100; t += 20) rig.buttons->tick20ms();
    CHECK(rig.drivers->brightness == 110);
    CHECK(rig.drivers->on == true);
}

TEST_CASE("a row with no pin, or no target, is a valid state and does nothing") {
    Rig rig;
    uint32_t id = 0;
    REQUIRE(rig.buttons->addListRow(id));    // no pin, no target
    rig.hold(true, 200);                     // must not crash
    CHECK(rig.drivers->on == true);

    // A pin but no target: the button reads, and drives nothing.
    REQUIRE(rig.buttons->setListRowField(id, "pin", "{\"value\":4}"));
    rig.hold(false, 100);
    rig.hold(true, 200);
    CHECK(rig.drivers->on == true);
}

TEST_CASE("rows are added and deleted at runtime") {
    Rig rig;
    CHECK(rig.buttons->listRowCount() == 0);
    const uint32_t a = rig.addRow("Drivers.on", "toggle");
    CHECK(rig.buttons->listRowCount() == 1);
    CHECK(rig.buttons->deleteListRow(a));
    CHECK(rig.buttons->listRowCount() == 0);
    CHECK_FALSE(rig.buttons->deleteListRow(a));

    // A deleted row stops acting: its pin is no longer polled.
    rig.hold(true, 200);
    CHECK(rig.drivers->on == true);
}

TEST_CASE("an active-high row reads the opposite level") {
    Rig rig;
    const uint32_t id = rig.addRow("Drivers.on", "toggle");
    REQUIRE(rig.buttons->setListRowField(id, "activeLow", "{\"value\":false}"));

    // Active-high: the switch feeds 3V3, so HIGH is pressed. Settle low first.
    platform::setTestGpioLevel(kPin, false);
    for (int t = 0; t < 100; t += 20) rig.buttons->tick20ms();
    CHECK(rig.drivers->on == true);

    platform::setTestGpioLevel(kPin, true);
    for (int t = 0; t < 100; t += 20) rig.buttons->tick20ms();
    CHECK(rig.drivers->on == false);
}

TEST_CASE("a target round-trips through the type and number the editor shows") {
    // The stored form is one Module.control string; type and number are how a user edits it. A bug
    // in either direction silently retargets a row, which is invisible until the button does the
    // wrong thing, so both directions are pinned here.
    struct Case { const char* target; uint8_t type; uint8_t nr; };
    const Case cases[] = {
        {"Control.switch1",  1, 1},
        {"Control.encoder3", 2, 3},
        {"Control.fader8",   3, 8},
        {"Control.pad64",    4, 64},
    };
    for (const Case& c : cases) {
        uint8_t type = 0, nr = 0;
        decomposeTarget(c.target, type, nr);
        INFO(c.target);
        CHECK(type == c.type);
        if (targetTypeIsNumbered(type)) CHECK(nr == c.nr);

        char back[32] = {};
        composeTarget(back, sizeof(back), type, nr);
        CHECK(std::strcmp(back, c.target) == 0);
    }

    // An empty target is the unassigned row, not an error.
    uint8_t type = 9, nr = 9;
    decomposeTarget("", type, nr);
    CHECK(type == 0);

    // A target this vocabulary cannot express reads back as unassigned, so the dropdown shows
    // "(none)" while the row keeps working. Rewriting it to fit would lose what the user set. The
    // editor offers the SURFACE only, because switch1 already targets Drivers.on and fader1 targets
    // Drivers.brightness: offering those directly would be two paths to one place.
    decomposeTarget("Audio.gain", type, nr);
    CHECK(type == 0);
    decomposeTarget("Drivers.on", type, nr);
    CHECK(type == 0);
}

TEST_CASE("editing the type or the number re-composes the target, so they cannot disagree") {
    Rig rig;
    const uint32_t id = rig.addRow("Drivers.on", "toggle");

    // Type 1 is "switch": the row keeps whatever number it had, which is 1 for a fresh row.
    REQUIRE(rig.buttons->setListRowField(id, "target", "{\"value\":1}"));
    REQUIRE(rig.buttons->setListRowField(id, "number", "{\"value\":5}"));

    char buf[256];
    JsonSink sink(buf, sizeof(buf));
    rig.buttons->writeListRow(sink, 0);
    CHECK(std::strstr(buf, "\"target\":\"Control.switch5\"") != nullptr);

    // The API's string form still reaches ANY control, which is the escape hatch for anything the
    // surface does not carry: the editor is a convenience, not the only way in.
    REQUIRE(rig.buttons->setListRowField(id, "target", "{\"value\":\"Drivers.palette\"}"));
    JsonSink sink2(buf, sizeof(buf));
    rig.buttons->writeListRow(sink2, 0);
    CHECK(std::strstr(buf, "\"target\":\"Drivers.palette\"") != nullptr);
}

TEST_CASE("a button fires the pad in that grid position, so a preset has a physical key") {
    // The point of the two-step model: a pad is a ROW on a grid, not a control, so a target naming
    // one has to be resolved through the list. Without this a row reading `Control.pad3` looks for a
    // control called pad3, finds nothing, and silently never works.
    Rig rig;
    rig.addRow("Control.pad3", "toggle");     // pad 3 = the third grid position, slot 2
    rig.hold(false, 100);

    rig.hold(true, 100);
    CHECK(rig.pads->fired == 1);
    CHECK(rig.pads->firedSlot == 2);          // by SLOT, not by row index

    // Held, then released: a preset applies ONCE per press. Re-firing on release would re-apply the
    // same look, and would make the pad flicker under a foot pedal.
    rig.hold(true, 200);
    rig.hold(false, 200);
    CHECK(rig.pads->fired == 1);
    rig.hold(true, 100);
    CHECK(rig.pads->fired == 2);
}

TEST_CASE("a button bound to an empty pad reports it rather than firing something else") {
    Rig rig;
    rig.addRow("Control.pad2", "toggle");     // slot 1 holds nothing: the grid has 0, 2 and 5
    rig.hold(false, 100);

    rig.hold(true, 100);
    CHECK(rig.pads->fired == 0);              // nothing near it fired
    CHECK(std::strstr(rig.buttons->status(), "empty") != nullptr);
}

TEST_CASE("a delta on a wide control counts from its real value, not a clamped byte") {
    // The surface reads every control as a BYTE, which is right for a fader's 8 bits of travel and
    // wrong here: a Uint16 holding 300 reads back 255, so `+10` wrote 265 instead of 310, and a
    // negative Int16 clamps to 0, so a delta could never move one down at all. A mapping nudges the
    // CONTROL, so it has to read in the control's own units.
    Rig rig;
    rig.addRow("Drivers.rate", "delta", 10);
    rig.hold(false, 100);

    rig.hold(true, 100);
    CHECK(rig.drivers->rate == 310);        // 300 + 10, not 255 + 10
}

TEST_CASE("a delta moves a negative control down, which a clamped read could not") {
    Rig rig;
    const uint32_t id = rig.addRow("Drivers.offset", "delta", -10);
    (void)id;
    rig.hold(false, 100);

    rig.hold(true, 100);
    CHECK(rig.drivers->offset == -60);      // -50 - 10, where a clamp-to-0 read would give -10

    // And the control's own floor still holds: the bounds are the CONTROL's, not the row's.
    for (int i = 0; i < 10; i++) { rig.hold(false, 100); rig.hold(true, 100); }
    CHECK(rig.drivers->offset == -100);
}
