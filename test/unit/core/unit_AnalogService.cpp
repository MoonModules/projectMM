// @module AnalogService
// @also InputMapping, Scheduler

// The analog input path end to end: an ADC reading, through the row's travel mapping and filter,
// into a control. The platform seam is a host stub whose value the test injects
// (platform::setTestAdcValue), so "the pedal is halfway" is expressed exactly as the module sees it
// on a board.

#include "doctest.h"
#include "core/AnalogService.h"
#include "core/Scheduler.h"
#include "core/MoonModule.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

using namespace mm;

namespace {

/// Stands in for the control surface: one fader a row can drive.
struct FakeSurface : public MoonModule {
    uint8_t fader1 = 0;
    bool    on = false;
    void defineControls() override {
        controls_.addControl("fader1", fader1, 0, 255);
        controls_.addControl("on", on);
    }
};

constexpr uint8_t kPin = 4;

struct Rig {
    Scheduler scheduler;
    FakeSurface* surface = new FakeSurface();
    AnalogService* svc = new AnalogService();
    uint32_t rowId = 0;
    Rig() {
        platform::clearTestAdcValue();
        surface->setName("Control");
        svc->setName("Analog");
        scheduler.addModule(surface);
        scheduler.addModule(svc);
        scheduler.setup();
        REQUIRE(svc->addListRow(rowId));
        set("pin", kPin);
        set("target", "\"Control.fader1\"");
        set("kind", "\"set\"");
    }
    ~Rig() { scheduler.release(); platform::clearTestAdcValue(); }

    void set(const char* field, int v) {
        char body[64];
        std::snprintf(body, sizeof(body), "{\"value\":%d}", v);
        REQUIRE(svc->setListRowField(rowId, field, body));
    }
    void set(const char* field, const char* rawJson) {
        char body[96];
        std::snprintf(body, sizeof(body), "{\"value\":%s}", rawJson);
        REQUIRE(svc->setListRowField(rowId, field, body));
    }

    /// Hold a raw count for `ticks` polls, long enough for the filter to settle on it.
    void hold(uint16_t raw, int ticks = 60) {
        platform::setTestAdcValue(kPin, raw);
        for (int i = 0; i < ticks; i++) svc->tick20ms();
    }
};

}  // namespace

TEST_CASE("an analog input drives a control across its travel") {
    // The whole path in one: a pin reading becomes a control value. The ends are what a user
    // actually notices, because a pedal that cannot reach 0 or full is the complaint this module's
    // min/max exists to answer.
    Rig rig;
    const uint16_t full = platform::adcMaxCount();

    rig.hold(0);
    CHECK(rig.surface->fader1 == 0);

    rig.hold(full);
    CHECK(rig.surface->fader1 == 255);

    rig.hold(static_cast<uint16_t>(full / 2));
    // Half travel is half value, within the filter's own resolution.
    CHECK(rig.surface->fader1 > 118);
    CHECK(rig.surface->fader1 < 138);
}

TEST_CASE("a pedal's usable travel is what maps, not the full sweep") {
    // The reason a row carries inMin/inMax: a real pedal rests well above 0 and tops out well below
    // full scale, so a raw mapping would give a control that never reaches either end.
    Rig rig;
    rig.set("inMin", 1000);
    rig.set("inMax", 3000);

    rig.hold(1000);
    CHECK(rig.surface->fader1 == 0);        // the bottom of the TRAVEL is the bottom of the range
    rig.hold(3000);
    CHECK(rig.surface->fader1 == 255);      // and the top is the top

    // Below and above the travel clamp rather than wrapping or running negative.
    rig.hold(200);
    CHECK(rig.surface->fader1 == 0);
    rig.hold(4000);
    CHECK(rig.surface->fader1 == 255);
}

TEST_CASE("an inverted input reads the other way round") {
    // A pot wired the other way is a wiring choice, not a fault, so it is a checkbox rather than a
    // reason to resolder.
    Rig rig;
    rig.set("invert", 1);
    rig.hold(0);
    CHECK(rig.surface->fader1 == 255);
    rig.hold(platform::adcMaxCount());
    CHECK(rig.surface->fader1 == 0);
}

TEST_CASE("a reversed min/max pair means inverted, rather than being an error") {
    // A user calibrating by moving the pedal to each end sets whichever end they reached first.
    // Refusing that would reject a calibration that says exactly what it means.
    Rig rig;
    rig.set("inMin", 3000);
    rig.set("inMax", 1000);
    rig.hold(1000);
    CHECK(rig.surface->fader1 == 255);
    rig.hold(3000);
    CHECK(rig.surface->fader1 == 0);
}

TEST_CASE("a resting input stops writing, so jitter does not flood the control") {
    // The deadband's whole purpose. An ADC wobbles a count or two at rest, and without this the row
    // would write its target fifty times a second forever, which on a persisted control also means
    // a save every time.
    Rig rig;
    rig.hold(2000);
    const uint8_t settled = rig.surface->fader1;

    // Something else moves the control; a resting pedal must not fight it back.
    rig.surface->fader1 = 42;
    platform::setTestAdcValue(kPin, 2001);       // one count of jitter
    for (int i = 0; i < 20; i++) rig.svc->tick20ms();
    CHECK(rig.surface->fader1 == 42);            // not rewritten

    // A real move still gets through.
    rig.hold(3500);
    CHECK(rig.surface->fader1 != 42);
    CHECK(rig.surface->fader1 > settled);
}

TEST_CASE("the first reading is taken whole, so a pedal does not sweep up from zero on boot") {
    // Seeding the filter with 0 would make every input ramp from the bottom at startup, writing its
    // target the whole way: a light that fades up on boot because a pedal is plugged in.
    Rig rig;
    platform::setTestAdcValue(kPin, platform::adcMaxCount());
    rig.svc->tick20ms();                          // ONE poll
    CHECK(rig.surface->fader1 == 255);            // already there, not on its way
}

TEST_CASE("an analog row scales into whatever range its target actually holds") {
    // A pedal is configured once and works on any target: the 0..255 travel is rescaled to the
    // control's own bounds, so a bool gets on/off and a narrower control gets its own maximum.
    Rig rig;
    rig.set("target", "\"Control.on\"");
    rig.hold(0);
    CHECK(rig.surface->on == false);
    rig.hold(platform::adcMaxCount());
    CHECK(rig.surface->on == true);
}

TEST_CASE("an unconfigured or unassigned row does nothing, quietly") {
    // Robustness: a fresh row has no pin and no target, which is a valid state a user passes through
    // rather than a fault to report.
    Scheduler sched;
    auto* surface = new FakeSurface();
    auto* svc = new AnalogService();
    surface->setName("Control");
    svc->setName("Analog");
    sched.addModule(surface);
    sched.addModule(svc);
    sched.setup();

    uint32_t id = 0;
    REQUIRE(svc->addListRow(id));
    for (int i = 0; i < 10; i++) svc->tick20ms();   // no pin named at all
    CHECK(surface->fader1 == 0);

    sched.release();
}
