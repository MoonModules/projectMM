// @module AudioModule

#include "doctest.h"
#include "core/AudioModule.h"

#include <cstring>

// The MoonModule lifecycle for the audio peripheral: setup/teardown is
// repeatable and leaves no residue, the static latestFrame() accessor stays
// coherent through any add/remove order (the robustness rule), and a module
// that is never configured stays idle. The signal math (level, bands, FFT) is
// covered by unit_AudioLevel.cpp / unit_AudioBands.cpp; this file owns the
// module's own state machine, which is what the classic-ESP32 boot-loop showed
// was the risky part. Host-side: hasI2sMic is false on desktop, so reinit()
// settles on the platform-inert path (no I2S init) — exactly the state a
// mic-less board boots into — and these pin that that state is clean, not a
// crash or a hang (the bug class behind the boot-loop).

// active_ is process-wide static and shared with the cases in unit_AudioLevel.cpp;
// each case here brackets its own setup() with a teardown() so it never leaks a
// live mic pointer into another test (the residue the robustness rule forbids).

TEST_CASE("AudioModule: a fresh, unconfigured module is idle (pins default unset)") {
    mm::AudioModule a;
    a.onBuildControls();
    // Pins default to -1 (unset, the standard Pin-control sentinel): the module is
    // user-added when a board has a mic and waits for the real GPIOs. It must never
    // have inited a mic by merely existing — the auto-init-on-boot path is what hung
    // a mic-less classic. (-1, not 0, so GPIO 0 stays a usable mic pin.)
    CHECK(a.wsPin == -1);
    CHECK(a.sdPin == -1);
    CHECK(a.sckPin == -1);
    a.setup();           // settles the status; no I2S touched on host or unset pins
    a.loop();            // must be a quiet no-op, not a crash, with nothing inited
    a.teardown();
    CHECK(true);
}

TEST_CASE("AudioModule: setup/teardown is repeatable with no residual state") {
    mm::AudioModule a;
    a.onBuildControls();
    const char* afterFirst = nullptr;
    for (int cycle = 0; cycle < 4; cycle++) {
        a.setup();
        a.onBuildState();   // reinit, as the Scheduler does
        a.loop();           // a tick: no heap churn, no crash (ASAN across cycles)
        a.teardown();
        // The status settles to a stable value, not a growing/changing string —
        // a leak or churn would show up as a different pointer each cycle. On host
        // (hasI2sMic false) that value is the platform-inert note; on a mic target
        // with unset pins it's the "set pins" note. Either way: same every cycle.
        if (cycle == 0) afterFirst = a.status();
        else CHECK(a.status() == afterFirst);
    }
}

TEST_CASE("AudioModule: teardown clears the active mic (latestFrame falls back to silence)") {
    {
        mm::AudioModule a;
        a.onBuildControls();
        a.setup();                                  // registers itself as active_
        REQUIRE(mm::AudioModule::latestFrame() == a.audioFrame());
        a.teardown();                               // must release the registration
    }
    // After teardown (and destruction) no mic is active: the accessor returns the
    // static silent frame, never a dangling pointer into the destroyed module.
    const mm::AudioFrame* f = mm::AudioModule::latestFrame();
    REQUIRE(f != nullptr);
    CHECK(f->level == 0);
    CHECK(f->peakHz == 0);
}

TEST_CASE("AudioModule: two mics — first wins, survivor re-elects, any order stays coherent") {
    // The robustness rule for a device with TWO AudioModules (two mics). Regression for a real bug:
    // removing the ACTIVE mic used to leave active_ null while the other mic kept running, so every
    // audio effect went silent (latestFrame() returned the static silence). The fix: first live module
    // wins the seat, teardown() vacates it, and any running module re-claims an empty seat in loop().
    mm::AudioModule a, b;
    a.onBuildControls();
    b.onBuildControls();

    a.setup();
    CHECK(mm::AudioModule::latestFrame() == a.audioFrame());
    b.setup();                                          // second mic captured, but a keeps the seat
    CHECK(mm::AudioModule::latestFrame() == a.audioFrame());   // FIRST wins, not last

    a.teardown();                                       // tearing down the ACTIVE one vacates the seat
    // Before the survivor's next tick the seat is empty; b re-claims it in loop() (self-election).
    b.loop();                                           // b (still live) takes over
    CHECK(mm::AudioModule::latestFrame() == b.audioFrame());   // effects keep reading a live frame
    b.teardown();
    // Both gone: back to the static silence, no dangling pointer.
    CHECK(mm::AudioModule::latestFrame()->level == 0);
}
