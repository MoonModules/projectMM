// @module NetworkSendDriver
// @also Drivers, Correction

// Pins the no-allocation-in-loop contract for NetworkSendDriver. The framework
// fires onBuildState (topology) and onCorrectionChanged (preset toggle) off
// the hot path; loop() must read the pre-sized buffer and never allocate.
//
// We can't observe a malloc from inside the test without an interceptor, so
// the contract is pinned indirectly: capture the corrected_ buffer's data
// pointer and size after a build-state, then assert they're already correct
// (the resize fired before any loop()) and that the buffer matches the source.

#include "doctest.h"
#include "light/drivers/NetworkSendDriver.h"
#include "light/drivers/Correction.h"
#include "light/drivers/Drivers.h"
#include "light/layers/Buffer.h"

// onBuildState sizes the correction-applied buffer to source-count × out-channels.
// The size matches what loop() needs on its first send. Calling loop()
// after onBuildState must not reallocate — pin the data pointer + shape.
TEST_CASE("NetworkSendDriver sizes corrected_ in onBuildState, not in loop") {
    mm::Buffer source;
    REQUIRE(source.allocate(64, 3));

    mm::Correction correction;
    correction.rebuild(255, mm::LightPreset::RGB);

    mm::NetworkSendDriver driver;
    driver.setSourceBuffer(&source);
    driver.setCorrection(&correction);
    driver.onBuildState();

    // After onBuildState the resized buffer is already in place.
    REQUIRE(driver.correctedBuffer().data() != nullptr);
    CHECK(driver.correctedBuffer().count() == 64);
    CHECK(driver.correctedBuffer().channelsPerLight() == 3);

    // loop() must not reallocate — same backing pointer, same shape — on every
    // protocol path (ArtNet, E1.31, DDP all share the pre-sized buffer and a
    // stack packet). Virtual time advances past the fps limiter between
    // protocols so each send path actually executes.
    const uint8_t* dataBefore = driver.correctedBuffer().data();
    for (uint8_t p = 0; p < mm::NetworkSendDriver::kProtocolCount; p++) {
        mm::platform::setTestNowMs(1000u + 100u * p);
        driver.protocol = p;
        driver.loop();
        CHECK(driver.correctedBuffer().data() == dataBefore);
        CHECK(driver.correctedBuffer().count() == 64);
        CHECK(driver.correctedBuffer().channelsPerLight() == 3);
    }
    mm::platform::setTestNowMs(0);   // restore real-clock behaviour for later cases
}

// A preset toggle from RGB to RGBW grows outChannels from 3 to 4. The grow
// runs in onCorrectionChanged, off the hot path.
TEST_CASE("NetworkSendDriver grows corrected_ in onCorrectionChanged on RGB → RGBW") {
    mm::Buffer source;
    REQUIRE(source.allocate(32, 3));

    mm::Correction correction;
    correction.rebuild(255, mm::LightPreset::RGB);

    mm::NetworkSendDriver driver;
    driver.setSourceBuffer(&source);
    driver.setCorrection(&correction);
    driver.onBuildState();

    REQUIRE(driver.correctedBuffer().channelsPerLight() == 3);

    // Simulate a preset change. Drivers normally drives this; we call directly.
    correction.rebuild(255, mm::LightPreset::RGBW);
    driver.onCorrectionChanged();

    CHECK(driver.correctedBuffer().count() == 32);
    CHECK(driver.correctedBuffer().channelsPerLight() == 4);
}

// A brightness-only change keeps outChannels at 3 — onCorrectionChanged is
// still called, but the resize short-circuits (existing buffer already fits).
TEST_CASE("NetworkSendDriver onCorrectionChanged is a no-op when outChannels unchanged") {
    mm::Buffer source;
    REQUIRE(source.allocate(48, 3));

    mm::Correction correction;
    correction.rebuild(255, mm::LightPreset::RGB);

    mm::NetworkSendDriver driver;
    driver.setSourceBuffer(&source);
    driver.setCorrection(&correction);
    driver.onBuildState();

    const uint8_t* dataBefore = driver.correctedBuffer().data();
    REQUIRE(dataBefore != nullptr);

    // Brightness change: outChannels stays 3, so the existing allocation fits.
    correction.rebuild(128, mm::LightPreset::RGB);
    driver.onCorrectionChanged();

    // Same backing allocation — the resize short-circuited.
    CHECK(driver.correctedBuffer().data() == dataBefore);
    CHECK(driver.correctedBuffer().count() == 48);
    CHECK(driver.correctedBuffer().channelsPerLight() == 3);
}
