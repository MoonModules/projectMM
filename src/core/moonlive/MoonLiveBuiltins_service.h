#pragma once

#include "core/moonlive/MoonLive.h"
#include "core/moonlive/MoonLiveBuiltins.h"
#include "core/Scheduler.h"
#include "platform/platform.h"   // gpioInputBegin / gpioRead / gpioWrite
#include "core/moonlive/MoonLiveBuiltins_common.h"   // the neutral half: math, waveforms, print
// addControl still comes from the light header: its sink machinery lives there, and moving that is
// a bigger job than moving a pure function. Everything else a service borrowed is now in core.
#include "light/moonlive/MoonLiveBuiltins_light.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

// The SERVICE vocabulary: what a script can do when its job is reading hardware and driving
// controls, rather than painting lights.
//
// Deliberately its own table rather than an extension of the light one. A service has no canvas, no
// light buffer and no per-light coordinate, so `setRGB`, `noise` and `xPos` would either fail at
// runtime or quietly do nothing; a table that offers them promises something it cannot keep. What a
// service gets instead is the two things that make it a service: it can READ a pin and it can WRITE
// a control.
//
// It lives in core because an input is domain-neutral: a button on a GPIO is not a light-domain
// idea, and a service script must run on a device with no lights configured at all.

namespace mm::moonlive {

/// The moment a service runs: the 50 Hz poll, not the render frame.
///
/// A contact closes for tens of milliseconds and a sensor answers at its own rate, so the render
/// tick would sample either thousands of times a second to learn the same thing. This is the same
/// reason `ButtonService` polls on tick20ms, and it means a slow script cannot stutter the lights at
/// the render rate: it costs its own tick instead.
inline constexpr const char* kEntryTick20ms = "tick20ms";

// --- The host functions -------------------------------------------------------------------------

/// gpioRead(pin) -> 0 or 1.
///
/// Opens the pin on first use with a pull-up, the wiring a switch to ground needs and the same
/// default `ButtonService::beginPin` uses for an active-low row. A script that wants the other
/// arrangement drives the pin itself and reads the level; a script that wants debouncing writes it,
/// because a time constant belongs to whoever knows what is wired (platform.h, gpioRead).
extern "C" inline uint32_t mm_service_gpioRead(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t pin = static_cast<uint32_t>(args[0]);
    if (pin > 48) return 0;                      // out of range on every supported chip
    // Opened ONCE per pin, not on every read. gpioInputBegin sets the resting level from the pull
    // (platform_desktop.cpp), so calling it per read holds the pin at its idle state and a script
    // could never see a press at all; on hardware it is also a peripheral reconfiguration per tick
    // for no reason. A script still needs no begin/read pair: the first read opens it.
    static bool opened[49] = {};
    if (!opened[pin]) {
        // Cached only on SUCCESS: the seam refuses an invalid pin and one wired to flash, PSRAM or
        // USB, and marking a refused pin as open meant reading it forever and reporting whatever a
        // floating read returned as if it were a button.
        if (!platform::gpioInputBegin(static_cast<uint8_t>(pin), platform::GpioPull::Up)) return 0;
        opened[pin] = true;
    }
    return platform::gpioRead(static_cast<uint8_t>(pin)) ? 1u : 0u;
}

/// gpioWrite(pin, on) -> whether the write took.
///
/// Returns 0 rather than failing silently when the pin has no output driver (a classic ESP32's
/// 34-39), which is the one thing a script cannot discover for itself.
extern "C" inline uint32_t mm_service_gpioWrite(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t pin = static_cast<uint32_t>(args[0]);
    if (pin > 48) return 0;
    return platform::gpioWrite(static_cast<uint8_t>(pin), args[1] != 0) ? 1u : 0u;
}

/// adcRead(pin) -> the raw ADC count, or 0 where the pin has no ADC.
///
/// RAW counts (0..4095 on ESP32), the same numbers `AnalogService` maps: a script reading a pedal or
/// a sense divider does its own arithmetic anyway, and a scaled value would hide which end of the
/// travel the reading came from. `adcMax()` reports the full scale so a script can normalize without
/// knowing the chip.
///
/// Opens nothing: the seam configures the pin on first use, so a script just reads it.
extern "C" inline uint32_t mm_service_adcRead(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t pin = static_cast<uint32_t>(args[0]);
    if (pin > 48) return 0;
    uint16_t raw = 0;
    if (!platform::adcRead(static_cast<uint8_t>(pin), raw)) return 0;
    return raw;
}

/// adcMv(pin) -> the pin's voltage in MILLIVOLTS, or 0 where the chip carries no calibration.
///
/// What a sensor reporting a VOLTAGE needs: a divider on a supply rail, a shunt amplifier reporting
/// current. The raw count is not a fixed fraction of full scale (every converter is nonlinear in its
/// own measured way), so scaling one by hand gives a number that looks right and is not. A pedal
/// keeps using adcRead: it maps a travel to a range and would convert a calibrated figure straight
/// back out again.
extern "C" inline uint32_t mm_service_adcMv(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t pin = static_cast<uint32_t>(args[0]);
    if (pin > 48) return 0;
    uint16_t mv = 0;
    if (!platform::adcReadMv(static_cast<uint8_t>(pin), mv)) return 0;
    return mv;
}

/// adcMax() -> the full-scale count adcRead reports on this platform.
///
/// So a script scales against the chip it is on rather than a number typed into the source: the same
/// script normalizes correctly on a 12-bit ESP32 and on whatever a later platform reports.
extern "C" inline uint32_t mm_service_adcMax(const uintptr_t*, uint32_t, const uint8_t*) {
    return platform::adcMaxCount();
}

/// setControl(name, value) -> whether the write took.
///
/// Writes a control on the CONTROL MODULE only, which is the decision recorded in the plan's step 0:
/// a script drives the surface, and the surface drives everything else. That is the same two-step
/// model the button and infrared mapping rows use, so a script and a mapping row reach a driver by
/// one path rather than two, and a script cannot rewrite a driver's pin list or a network setting by
/// naming it.
///
/// The name is a pointer into the compiled program's string pool, which outlives the run: the same
/// way `addControl` receives its name (MoonLiveBuiltins_light.h, addControlDecl).
extern "C" inline uint32_t mm_service_setControl(const uintptr_t* args, uint32_t, const uint8_t*) {
    const char* name = reinterpret_cast<const char*>(args[0]);
    if (!name || !name[0]) return 0;
    Scheduler* sched = Scheduler::instance();
    if (!sched) return 0;
    // A JSON object read for its "value" key, which is what the primitive documents and what every
    // other caller passes: a bare number would not parse.
    char valueJson[32];
    std::snprintf(valueJson, sizeof(valueJson), "{\"value\":%d}",
                  static_cast<int>(static_cast<int32_t>(args[1])));
    // Through the one generic control-set primitive every transport uses, so a script write is
    // indistinguishable from an OSC message or a remote press to whatever it drives.
    return sched->setControl("Control", name, valueJson) == Scheduler::SetControlResult::Ok ? 1u : 0u;
}

// --- The tables ---------------------------------------------------------------------------------

/// A service script's system variables: elapsed milliseconds, and nothing else.
///
/// No `width`/`height`/`depth`: a service is not attached to a grid, and a variable that always
/// reads zero is a trap rather than a convenience. `t` rides an argument register, so it costs no
/// instruction and no arena byte.
inline SysVarTable serviceSysVars() {
    SysVarTable t;
    t.add({"t", SysVarKind::Arg, kArg3});
    return t;
}

/// The service built-in table the binding injects into the compiler.
inline const BuiltinTable& serviceBuiltins() {
    // Built ONCE and reused: the table is ~2 KB by value and never changes after registration, and
    // it was being rebuilt on every prepare sweep, including the sweeps whose hash check returns
    // immediately because nothing was edited.
    static const BuiltinTable table = [] {
        BuiltinTable t;
    // The neutral half: a service gets sin, noise, beat, print and the rest, because none of that
    // is about light. It could not before, which is why a sweep had to be spelled as integer
    // arithmetic while an effect one line away could call sin().
    addCommonBuiltins(t);
    // gpioRead(pin)          -> 0/1. The input half: any switch, PIR, or level a pin can carry.
    t.add({"gpioRead", 1, /*returns*/ true, BuiltinKind::Call, &mm_service_gpioRead, {}});
    // gpioWrite(pin, on)     -> 0/1. The output half: a relay, an indicator, a chip's enable line.
    t.add({"gpioWrite", 2, /*returns*/ true, BuiltinKind::Call, &mm_service_gpioWrite, {}});
    // adcRead(pin)           -> raw counts. The analog half: a pedal, a pot, a sense divider.
    t.add({"adcRead", 1, /*returns*/ true, BuiltinKind::Call, &mm_service_adcRead, {}});
    // adcMv(pin)             -> millivolts, chip-calibrated: the form a voltage or current sensor
    // needs, where a raw count would be a plausible-looking wrong answer.
    t.add({"adcMv", 1, /*returns*/ true, BuiltinKind::Call, &mm_service_adcMv, {}});
    // adcMax()               -> this platform's full scale, so a script normalizes without a magic
    // number. Zero arguments, like a system variable would be, but a call because it asks the seam.
    t.add({"adcMax", 0, /*returns*/ true, BuiltinKind::Call, &mm_service_adcMax, {}});
    // setControl(name, v)    -> 0/1. The surface write: "switch1", "fader3", "pad7".
    // byStr 0x1: the FIRST argument is a name in quotes, which the compiler passes as a pointer into
    // the string pool. Without the mask it is parsed as a number and the call is rejected.
    t.add({"setControl", 2, /*returns*/ true, BuiltinKind::Call, &mm_service_setControl, {},
           /*byRef*/ 0, /*byStr*/ 0x1});
    // addControl(name, member, min, max) -> declare a setting, exactly as an effect script does: the
    // member is passed BY REFERENCE (its arena offset) and the name as a string, which is what the
    // byRef/byStr masks say. A service without this could be configured only by editing its source.
    t.add({"addControl", 4, /*returns*/ false, BuiltinKind::Call, &mm_light_addControl, {},
           /*byRef*/ 0x2, /*byStr*/ 0x1});
        return t;
    }();
    return table;
}

}  // namespace mm::moonlive
