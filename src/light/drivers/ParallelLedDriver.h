#pragma once

#include "light/drivers/Drivers.h"        // DriverBase, Correction
#include "light/drivers/LcdSlots.h"        // encodeWs2812LcdSlots (shared encoder)
#include "light/drivers/LedDriverConfig.h"
#include "light/drivers/PinList.h"         // parsePinList / assignCounts (shared)
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>   // snprintf for status strings
#include <cstring>  // std::strcmp, std::memset

namespace mm {

// Shared body for the parallel WS2812 LED drivers: the S3's LCD_CAM i80 bus
// (LcdLedDriver) and the P4's Parlio peripheral (ParlioLedDriver). Both drive up
// to 8 strands that clock out simultaneously, one GPIO lane each, fed consecutive
// slices of the source buffer; both pre-encode the whole frame (a per-ROW fused
// correct+transpose, the SAME LcdSlots.h encoder — a Parlio bus byte and an i80
// bus byte are identical) plus a zeroed >=300 µs latch pad into a platform-owned
// DMA buffer, then ship it as one autonomous transfer. The two were ~250 of ~370
// lines byte-for-byte identical; this is the one copy (the No-duplication rule).
//
// Binding is CRTP (static polymorphism), NOT a virtual second hierarchy: the base
// calls back into the derived through `static_cast<Derived*>(this)->busX()` with
// no vtable and no runtime indirection, so it stays inside the hot-path / data-
// over-objects rules and the "one deliberate class hierarchy is the module tree"
// rule (the only virtual boundary remains MoonModule -> DriverBase). The derived
// supplies just the handful of peripheral-specific pieces:
//   - bus* methods: thin wrappers over its platform::{lcd,parlio}Ws2812* calls
//     (they own the handle, so the base never names the handle type);
//   - static lanesAvailable()  -> the platform::{lcd,parlio}Lanes constant the
//     `if constexpr (... == 0)` inert-on-wrong-chip guards key off;
//   - static kExactLaneCount   -> true for i80 (its layer rejects a partial bus,
//     so the pin list must name exactly 8); false for Parlio (1..8 lanes);
//   - static kInitFailMsg, kClockHz (the slot rate), recordBusPins/extraBusPins-
//     Current() for any extra pins the i80 driver tracks (WR/DC) that Parlio
//     doesn't.
// configErr_/failBuf_ and their clear/ensure helpers come from DriverBase (shared
// with RmtLedDriver too).
template <class Derived>
class ParallelLedDriver : public DriverBase {
public:
    // Bus width this increment: 8 of the peripheral's 16 lanes (matches the
    // platform's lane constant; widening to 16 is a later constant change).
    static constexpr uint8_t kMaxLanes = 8;

    // Comma-separated controls — shared shape with RmtLedDriver (parsers in
    // PinList.h). Defaults live on the derived (chip-specific safe pins), so the
    // derived sets them after construction; the base just declares them.
    char pins[24] = "";
    char ledsPerPin[48] = "";

    bool     loopbackTest = false;
    uint16_t loopbackRxPin = 0;

    void onBuildControls() override {
        controls_.addText("pins", pins, sizeof(pins));
        controls_.addText("ledsPerPin", ledsPerPin, sizeof(ledsPerPin));
        derived()->addBusControls();   // i80 adds clockPin/dcPin here; Parlio none
        controls_.addBool("loopbackTest", loopbackTest);
        // Always bound, shown only in test mode — the conditional-control shape.
        controls_.addUint16("loopbackRxPin", loopbackRxPin);
        controls_.setHidden(controls_.count() - 1, !loopbackTest);
    }

    bool controlChangeTriggersBuildState(const char* name) const override {
        return std::strcmp(name, "pins") == 0 || std::strcmp(name, "ledsPerPin") == 0
            || derived()->busControlTriggersBuild(name);   // clockPin/dcPin on i80
    }

    void onUpdate(const char* name) override {
        const bool isTestControl = std::strcmp(name, "loopbackTest") == 0;
        const bool isPinControl  = std::strcmp(name, "pins") == 0
                                || std::strcmp(name, "loopbackRxPin") == 0;
        if (isTestControl && !loopbackTest) {
            // Toggling the test off clears the loopback's own verdict, then
            // re-derives the real driver status — a config/init error must
            // survive, which a blind clearStatus() would hide.
            clearFailBuf();
            clearStatus();
            parseConfig();
            reinit();
        } else if (loopbackTest && (isTestControl || isPinControl)) {
            // A pin edit changes laneList_/laneCount_/frameBytes_, but onUpdate runs
            // BEFORE the onBuildState() sweep (and loopbackRxPin doesn't trigger that
            // sweep at all), so refresh the lane config here before testing it —
            // otherwise the self-test would build its private bus from stale pins.
            if (isPinControl) { parseConfig(); reinit(); }
            runLoopbackSelfTest();
        }
    }

    void setup() override { parseConfig(); reinit(); }
    void teardown() override {
        deinit();
        DriverBase::teardown();   // clears failBuf_ + configErr_
    }

    void onBuildState() override {
        parseConfig();
        reinit();
        MoonModule::onBuildState();
    }

    // RGB<->RGBW changes the bytes-per-light and therefore the frame size.
    void onCorrectionChanged() override { parseConfig(); reinit(); }

    void setSourceBuffer(Buffer* buf) override { sourceBuffer_ = buf; parseConfig(); }
    void setCorrection(const Correction* c) override { correction_ = c; parseConfig(); }

    void loop() override {
        if constexpr (Derived::lanesAvailable() == 0) return;  // inert off this chip
        if (!inited_ || !dmaBuf_ || !sourceBuffer_ || !sourceBuffer_->data()
            || !correction_ || laneCount_ == 0 || maxLaneLights_ == 0) return;
        const uint8_t outCh = correction_->outChannels;
        if (outCh == 0 || frameBytes_ > derived()->busCapacity()) return;

        // Fused per-ROW pass: correct the same light index of every active lane
        // into the wire block, then transpose it into 3-slot bus bytes. No heap,
        // integer math only.
        const uint8_t* src = sourceBuffer_->data();
        const uint8_t srcCh = sourceBuffer_->channelsPerLight();
        uint8_t* out = dmaBuf_;
        uint8_t wire[kMaxLanes * 4];
        for (nrOfLightsType row = 0; row < maxLaneLights_; row++) {
            uint8_t mask = 0;
            for (uint8_t lane = 0; lane < laneCount_; lane++) {
                if (row >= laneCounts_[lane]) continue;   // short strand: idle LOW
                mask |= static_cast<uint8_t>(1u << lane);
                correction_->apply(src + (laneStart_[lane] + row) * srcCh,
                                   wire + lane * 4);
            }
            encodeWs2812LcdSlots(wire, mask, outCh, out);
            out += static_cast<size_t>(outCh) * 8 * 3;
        }
        // The latch pad after the rows is zeroed at reinit and never written
        // here, so the transfer ends holding every lane LOW for >=300 µs. Wait
        // only when the transfer actually started: a failed transmit gives no
        // done-callback, so an unconditional wait would block the full 1000 ms
        // timeout every tick. Drop the frame and retry next tick (self-heals).
        if (derived()->busTransmit(frameBytes_))
            derived()->busWait(1000 /* ms */);
    }

    // Test-only accessors — pin the lane slicing and frame-size arithmetic on the
    // host (unit_{Lcd,Parlio}LedDriver.cpp); the hardware half is proven on device.
    uint8_t laneCount() const { return laneCount_; }
    nrOfLightsType laneLightCount(uint8_t i) const { return i < laneCount_ ? laneCounts_[i] : 0; }
    nrOfLightsType laneStart(uint8_t i) const { return i < laneCount_ ? laneStart_[i] : 0; }
    nrOfLightsType maxLaneLights() const { return maxLaneLights_; }
    size_t frameBytes() const { return frameBytes_; }

protected:
    Derived* derived() { return static_cast<Derived*>(this); }
    const Derived* derived() const { return static_cast<const Derived*>(this); }

    Buffer* sourceBuffer_ = nullptr;
    const Correction* correction_ = nullptr;

    LedDriverConfig cfg_;
    bool inited_ = false;
    uint8_t* dmaBuf_ = nullptr;          // platform-owned; cached for the encode
    uint16_t laneList_[kMaxLanes] = {};
    nrOfLightsType laneCounts_[kMaxLanes] = {};
    nrOfLightsType laneStart_[kMaxLanes] = {};
    uint8_t laneCount_ = 0;
    nrOfLightsType maxLaneLights_ = 0;
    size_t frameBytes_ = 0;
    uint16_t busPins_[kMaxLanes] = {};   // data pins the live bus/unit was built
                                         // with — a pin change must rebuild even
                                         // when the buffer still fits
    uint8_t  busLaneCount_ = 0;          // lane count the live bus was built with —
                                         // a lane-count change (e.g. Parlio 8→4)
                                         // can keep the same frameBytes_ yet needs
                                         // a rebuild, so the fast path checks it too

    static constexpr uint8_t maxLanesForTarget() {
        return (Derived::lanesAvailable() > 0 && Derived::lanesAvailable() < kMaxLanes)
                   ? Derived::lanesAvailable()
                   : kMaxLanes;
    }

    // Frame bytes: longest lane × channels × 24 slot bytes, plus a zeroed latch
    // pad of >=300 µs at the slot rate (800 B) with clock-tolerance slack (64),
    // rounded up to the bus's 64-byte alignment. 0 when there's nothing to send.
    static size_t frameBytesFor(nrOfLightsType maxLights, uint8_t outCh) {
        if (maxLights == 0 || outCh == 0) return 0;
        const size_t latchPad = 800 + 64;
        const size_t bytes = static_cast<size_t>(maxLights) * outCh * 24 + latchPad;
        return (bytes + 63) & ~static_cast<size_t>(63);
    }

    // Re-derive lanes/counts/starts/frame size from the controls and the wired
    // buffer/correction. Off the hot path; on error the driver idles with the
    // parse literal in the status slot (same shape as RmtLedDriver).
    bool parseConfig() {
        laneCount_ = 0;
        maxLaneLights_ = 0;
        frameBytes_ = 0;
        uint8_t n = 0;
        const char* err = parsePinList(pins, laneList_, maxLanesForTarget(), n);
        // i80 (kExactLaneCount) requires a real GPIO on every data line up to the
        // bus width — a partial bus is rejected at esp_lcd_new_i80_bus(). Parlio
        // accepts 1..8, so it sets kExactLaneCount=false and skips this.
        if constexpr (Derived::kExactLaneCount) {
            if (!err && n != kMaxLanes) err = "LCD bus needs exactly 8 pins";
        }
        if (!err) {
            const nrOfLightsType total = sourceBuffer_ ? sourceBuffer_->count() : 0;
            err = assignCounts(ledsPerPin, n, total, laneCounts_);
        }
        if (err) {
            setConfigErr(err);
            return false;
        }
        laneCount_ = n;
        nrOfLightsType start = 0;
        for (uint8_t i = 0; i < laneCount_; i++) {
            laneStart_[i] = start;
            start = static_cast<nrOfLightsType>(start + laneCounts_[i]);
            if (laneCounts_[i] > maxLaneLights_) maxLaneLights_ = laneCounts_[i];
        }
        const uint8_t outCh = correction_ ? correction_->outChannels : 0;
        frameBytes_ = frameBytesFor(maxLaneLights_, outCh);
        clearConfigErr();
        return true;
    }

    // --- bus + DMA buffer (hardware; this peripheral's targets only). Fused on
    // purpose: max_transfer_bytes is fixed at bus creation, so growing the frame
    // means re-creating the bus and its buffer together. Grow-only; the buffer is
    // re-zeroed on every reinit so a shrunken frame's latch pad can't hold stale
    // row bytes. ---

    void reinit() {
        if constexpr (Derived::lanesAvailable() == 0) return;
        if (laneCount_ == 0 || frameBytes_ == 0) { deinit(); return; }
        if (inited_ && derived()->busCapacity() >= frameBytes_ && busPinsCurrent()
            && busLaneCount_ == laneCount_) {
            // Existing bus + buffer still fit AND sit on the wanted pins AND drive
            // the same lane count — just clear stale bytes so the (possibly
            // relocated) latch pad is zero. The pin check matters: a pin edit keeps
            // the frame size, and without it the bus would keep clocking out on the
            // OLD GPIOs. The lane-count check matters for Parlio: 8→4 lanes can keep
            // frameBytes_ and the surviving pins, but the bus was built for 8.
            std::memset(dmaBuf_, 0, derived()->busCapacity());
            return;
        }
        deinit();
        inited_ = derived()->busInit(frameBytes_);
        dmaBuf_ = inited_ ? derived()->busBuffer() : nullptr;
        if (inited_) {
            for (uint8_t i = 0; i < kMaxLanes; i++) busPins_[i] = laneList_[i];
            busLaneCount_ = laneCount_;
            derived()->recordBusPins();   // i80 also stores WR/DC; Parlio no-op
        }
        if (!inited_) {
            clearFailBuf();
            setStatus(Derived::kInitFailMsg, Severity::Error);
        } else if (status() == Derived::kInitFailMsg) {
            clearStatus();
        }
    }

    void deinit() {
        if constexpr (Derived::lanesAvailable() == 0) return;
        if (inited_) derived()->busDeinit();
        inited_ = false;
        dmaBuf_ = nullptr;
        busLaneCount_ = 0;
    }

    bool busPinsCurrent() const {
        for (uint8_t i = 0; i < laneCount_; i++)
            if (busPins_[i] != laneList_[i]) return false;
        return derived()->extraBusPinsCurrent();   // i80 also checks WR/DC
    }

    // --- loopback self-test (control-driven; same status shapes as RMT). Builds
    // the REAL frame with the test pattern on lane 0, deinits the live bus, and
    // hands the platform a private TX path + RMT-RX capture that transmits the
    // genuine frame back to back and verifies every bit. ---

    void runLoopbackSelfTest() {
        if constexpr (Derived::lanesAvailable() == 0) {
            clearFailBuf();
            setStatus("loopback: not supported on this platform", Severity::Warning);
            return;
        }
        if (laneCount_ == 0) {
            clearFailBuf();
            setStatus("loopback: no valid pins", Severity::Warning);
            return;
        }
        const uint8_t outCh = correction_ ? correction_->outChannels : 0;
        if (frameBytes_ == 0 || maxLaneLights_ == 0 || outCh == 0) {
            clearFailBuf();
            setStatus("loopback: no lights to encode", Severity::Warning);
            return;
        }
        // Build the REAL frame with the test pattern in every row on lane 0 only;
        // the platform transmits the genuine transfer (size, DMA chain, latch pad)
        // back to back and verifies every captured bit, so the test covers what
        // the render loop actually sends. Heap alloc is fine: control-driven, off
        // the hot path.
        auto* frame = static_cast<uint8_t*>(platform::alloc(frameBytes_));
        if (!frame) {
            clearFailBuf();
            setStatus("loopback: out of memory", Severity::Error);
            return;
        }
        std::memset(frame, 0, frameBytes_);
        uint8_t wire[kMaxLanes * 4] = {};
        wire[0] = 0xA5; wire[1] = 0x00; wire[2] = 0xFF;   // wire[3] stays 0 (RGBW)
        uint8_t* out = frame;
        for (nrOfLightsType row = 0; row < maxLaneLights_; row++) {
            encodeWs2812LcdSlots(wire, 0x01, outCh, out);
            out += static_cast<size_t>(outCh) * 8 * 3;
        }
        const size_t dataBytes = static_cast<size_t>(maxLaneLights_) * outCh * 24;
        deinit();   // free the live bus; the test builds its own on the data pins
        const auto r = derived()->busLoopback(frame, frameBytes_, dataBytes,
                                              static_cast<uint8_t>(outCh * 8));
        platform::free(frame);
        // Loopback result first, then reinit: if rebuilding the real bus fails
        // afterwards, kInitFailMsg overwrites the verdict — an unusable driver
        // matters more than a passed self-test.
        if (!r.jumperDetected) {
            clearFailBuf();
            setStatus("loopback: jumper not detected", Severity::Warning);
        } else if (r.pass) {
            clearFailBuf();
            setStatus("loopback PASS", Severity::Status);
        } else if (failBufEnsure()) {
            // Name the first corrupted light: the loopback reports the first
            // mismatching bit; rowBits = outCh*8, so light = firstBadBit / rowBits.
            const unsigned rowBits = static_cast<unsigned>(outCh) * 8u;
            const unsigned badLight = rowBits ? r.firstBadBit / rowBits : 0u;
            std::snprintf(failBuf_, kFailBufLen,
                          "loopback FAIL: bad bit %u/%u (light %u)",
                          static_cast<unsigned>(r.firstBadBit),
                          static_cast<unsigned>(r.bitsChecked), badLight);
            setStatus(failBuf_, Severity::Error);
        } else {
            setStatus("loopback FAIL", Severity::Error);
        }
        reinit();
    }
};

} // namespace mm
