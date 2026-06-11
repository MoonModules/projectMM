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

// WS2812B output over the ESP32-P4 Parlio (Parallel IO) TX peripheral: up to 8
// strands clock out SIMULTANEOUSLY, one GPIO lane each, fed consecutive slices
// of the source buffer. The P4's scale path, sibling of LcdLedDriver.
//
// It IS the LcdLedDriver shape — same pins/ledsPerPin controls (PinList.h),
// same per-ROW correction+encode using the SAME encoder (LcdSlots.h; a Parlio
// bus byte and an i80 bus byte are identical — one word per slot, bit L = data
// line L), same fused lifecycle (platform owns the DMA buffer; unit recreation
// = resize; failed reinit idles loop()), same latch pad of zeroed trailing
// bytes, same single-shot autonomous-DMA-transfer-then-wait (no refill deadline
// for WiFi to miss). Two deliberate simplifications vs the i80 driver, because
// Parlio is a simpler peripheral:
//   - NO clockPin/dcPin: Parlio generates the pixel clock itself, so there are
//     no sacrificial WR/DC lines to reserve.
//   - NO exactly-8-pins rule: i80 rejects a partial bus, Parlio doesn't, so the
//     driver runs on 1..8 lanes — whatever `pins` names.
// Prior art: the ESP32-P4 Parlio peripheral, the hpwit/FastLED parallel-WS2812
// lineage — architecture studied, never copied (see ParlioLedDriver.md).
class ParlioLedDriver : public DriverBase {
public:
    // Bus width this increment: 8 of the unit's 16 lanes (matches
    // platform::parlioLanes; widening to 16 is a later constant change).
    static constexpr uint8_t kMaxLanes = 8;

    // Comma-separated data GPIO list, one lane per pin (1..8 — no all-pins
    // rule). The default names 8 strapping-safe pins but `ledsPerPin` puts all
    // the lights on lane 0 (a serpentine 8×8 panel is one 64-LED strand on the
    // first pin); the other 7 lanes get 0 lights and idle LOW — so you can wire
    // more strips later just by reassigning `ledsPerPin`, no need to add pins.
    // Pins avoid the P4-NANO's reserved / dangerous GPIOs: STRAPPING pins
    // (34-38, boot-mode control — never drive these), Ethernet RMII (28-31,
    // 49-52), the ESP32-C6 SDIO (14-19, 54), I2C (7-8). The clear GPIOs are
    // 20-27, 32-33, 39-48. The loopback self-test transmits on the FIRST pin.
    char pins[24] = "20,21,22,23,24,25,26,27";

    // Lights per lane, matched to `pins` by position — same semantics as the
    // RMT/LCD drivers. "64" puts all 64 on lane 0 (the 8×8 panel); lanes 1-7
    // take the 0 remainder and idle. Empty = even split across all 8.
    char ledsPerPin[48] = "64";

    // Loopback self-test controls — wired now, IMPLEMENTED IN ROUND 4 (the
    // platform body is a stub today). The test transmits on the FIRST pin in
    // `pins` and captures on loopbackRxPin. To test without disturbing the strip
    // on lane 0, temporarily set the first pin to a spare line (e.g. 32) and
    // jumper it to loopbackRxPin (33); both are strapping-safe. For now ticking
    // it reports "not implemented".
    bool     loopbackTest = false;
    uint16_t loopbackRxPin = 33;

    void onBuildControls() override {
        controls_.addText("pins", pins, sizeof(pins));
        controls_.addText("ledsPerPin", ledsPerPin, sizeof(ledsPerPin));
        controls_.addBool("loopbackTest", loopbackTest);
        controls_.addUint16("loopbackRxPin", loopbackRxPin);
        controls_.setHidden(controls_.count() - 1, !loopbackTest);
    }

    bool controlChangeTriggersBuildState(const char* name) const override {
        return std::strcmp(name, "pins") == 0 || std::strcmp(name, "ledsPerPin") == 0;
    }

    void onUpdate(const char* name) override {
        const bool isTestControl = std::strcmp(name, "loopbackTest") == 0;
        const bool isPinControl  = std::strcmp(name, "pins") == 0
                                || std::strcmp(name, "loopbackRxPin") == 0;
        if (isTestControl && !loopbackTest) {
            // Toggling off clears the loopback verdict, then re-derives the real
            // driver status (a config/init error must survive a blind clear).
            clearFailBuf();
            clearStatus();
            parseConfig();
            reinit();
        } else if (loopbackTest && (isTestControl || isPinControl)) {
            runLoopbackSelfTest();
        }
    }

    void setup() override { parseConfig(); reinit(); }
    void teardown() override {
        deinit();
        clearFailBuf();
        clearConfigErr();
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
        if constexpr (platform::parlioLanes == 0) return;  // inert off Parlio chips
        if (!inited_ || !dmaBuf_ || !sourceBuffer_ || !sourceBuffer_->data()
            || !correction_ || laneCount_ == 0 || maxLaneLights_ == 0) return;
        const uint8_t outCh = correction_->outChannels;
        if (outCh == 0 || frameBytes_ > platform::parlioWs2812BufferCapacity(parlio_)) return;

        // Fused per-ROW pass: correct the same light index of every active lane
        // into the wire block, transpose into 3-slot bus bytes. Integer, no heap.
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
        // Latch pad after the rows stays zeroed (set at reinit) → lines idle LOW.
        // Wait only when the transfer started (a failed transmit gives no
        // callback; an unconditional wait would block the full timeout).
        if (platform::parlioWs2812Transmit(parlio_, frameBytes_))
            platform::parlioWs2812Wait(parlio_, 1000 /* ms */);
    }

    // Test-only accessors — pin the lane slicing + frame-size arithmetic on the
    // host (unit_ParlioLedDriver.cpp); the hardware half is proven on device.
    uint8_t laneCount() const { return laneCount_; }
    nrOfLightsType laneLightCount(uint8_t i) const { return i < laneCount_ ? laneCounts_[i] : 0; }
    nrOfLightsType laneStart(uint8_t i) const { return i < laneCount_ ? laneStart_[i] : 0; }
    nrOfLightsType maxLaneLights() const { return maxLaneLights_; }
    size_t frameBytes() const { return frameBytes_; }

private:
    Buffer* sourceBuffer_ = nullptr;
    const Correction* correction_ = nullptr;

    LedDriverConfig cfg_;
    platform::ParlioWs2812Handle parlio_;
    bool inited_ = false;
    uint8_t* dmaBuf_ = nullptr;          // platform-owned; cached for the encode
    uint16_t busPins_[kMaxLanes] = {};   // data pins the live unit was built with
                                         // — a pin change must rebuild the unit
                                         // even when the buffer still fits (no
                                         // WR/DC here, unlike the i80 driver)
    uint16_t laneList_[kMaxLanes] = {};
    nrOfLightsType laneCounts_[kMaxLanes] = {};
    nrOfLightsType laneStart_[kMaxLanes] = {};
    uint8_t laneCount_ = 0;
    nrOfLightsType maxLaneLights_ = 0;
    size_t frameBytes_ = 0;

    const char* configErr_ = nullptr;
    static constexpr size_t kFailBufLen = 48;
    char* failBuf_ = nullptr;
    static constexpr const char* kInitFailMsg = "Parlio init failed — check pins / memory";

    // The WS2812 slot rate (375 ns @ 2.67 MHz) — identical to the LCD driver's;
    // the P4 Parlio's 160 MHz PLL clock divides to it exactly (/60).
    static constexpr uint32_t kPclkHz = 2'666'666;

    static constexpr uint8_t maxLanesForTarget() {
        return (platform::parlioLanes > 0 && platform::parlioLanes < kMaxLanes)
                   ? platform::parlioLanes
                   : kMaxLanes;
    }

    // Frame bytes: longest lane × channels × 24 slot bytes + zeroed latch pad
    // (>=300 µs at 2.67 MHz = 800 B, plus 64 slack), 64-rounded. 0 for an empty
    // grid. Identical to the LCD driver — same encoder, same slot count.
    static size_t frameBytesFor(nrOfLightsType maxLights, uint8_t outCh) {
        if (maxLights == 0 || outCh == 0) return 0;
        const size_t latchPad = 800 + 64;
        const size_t bytes = static_cast<size_t>(maxLights) * outCh * 24 + latchPad;
        return (bytes + 63) & ~static_cast<size_t>(63);
    }

    // Re-derive lanes/counts/starts/frame size. Off the hot path; on error the
    // driver idles with the parse literal in the status slot. NO exactly-8-pins
    // rule (the i80 driver's, not Parlio's): 1..8 lanes are all valid.
    bool parseConfig() {
        laneCount_ = 0;
        maxLaneLights_ = 0;
        frameBytes_ = 0;
        uint8_t n = 0;
        const char* err = parsePinList(pins, laneList_, maxLanesForTarget(), n);
        if (!err) {
            const nrOfLightsType total = sourceBuffer_ ? sourceBuffer_->count() : 0;
            err = assignCounts(ledsPerPin, n, total, laneCounts_);
        }
        if (err) {
            configErr_ = err;
            setStatus(err, Severity::Error);
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

    void clearConfigErr() {
        if (configErr_) {
            if (status() == configErr_) clearStatus();
            configErr_ = nullptr;
        }
    }

    // --- TX unit + DMA buffer (hardware; Parlio targets only). Fused: the
    // max transfer size is fixed at unit creation, so growing the frame means
    // re-creating the unit + buffer. Grow-only; the buffer is re-zeroed on every
    // reinit so a shrunken frame's latch pad can't hold stale row bytes. ---

    void reinit() {
        if constexpr (platform::parlioLanes == 0) return;
        if (laneCount_ == 0 || frameBytes_ == 0) { deinit(); return; }
        if (inited_ && platform::parlioWs2812BufferCapacity(parlio_) >= frameBytes_
            && busPinsCurrent()) {
            // Existing unit + buffer still fit AND sit on the wanted pins — just
            // clear stale bytes. The pin check matters: a `pins` edit keeps the
            // frame size, and without it the unit would clock out on OLD GPIOs.
            std::memset(dmaBuf_, 0, platform::parlioWs2812BufferCapacity(parlio_));
            return;
        }
        deinit();
        inited_ = platform::parlioWs2812Init(parlio_, laneList_, laneCount_,
                                             kPclkHz, frameBytes_);
        dmaBuf_ = inited_ ? platform::parlioWs2812Buffer(parlio_) : nullptr;
        if (inited_) {
            for (uint8_t i = 0; i < kMaxLanes; i++) busPins_[i] = laneList_[i];
        }
        if (!inited_) {
            clearFailBuf();
            setStatus(kInitFailMsg, Severity::Error);
        } else if (status() == kInitFailMsg) {
            clearStatus();
        }
    }

    void deinit() {
        if constexpr (platform::parlioLanes == 0) return;
        if (inited_) platform::parlioWs2812Deinit(parlio_);
        inited_ = false;
        dmaBuf_ = nullptr;
    }

    bool busPinsCurrent() const {
        for (uint8_t i = 0; i < laneCount_; i++)
            if (busPins_[i] != laneList_[i]) return false;
        return true;
    }

    // --- loopback self-test (control-driven). Builds the REAL frame with the
    // test pattern on lane 0, deinits the live unit, and hands the platform a
    // private Parlio TX unit + RMT-RX capture that transmits the genuine frame
    // back to back and verifies every bit. Same shape as the LCD/RMT rigs, minus
    // the i80 full-bus workaround (Parlio runs on 1 lane, so lane 0 alone is a
    // valid private unit). ---

    void runLoopbackSelfTest() {
        if constexpr (platform::parlioLanes == 0) {
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
        // Build the REAL frame, test pattern in every row on lane 0 only — the
        // platform transmits the genuine transfer (size, DMA, latch pad) back to
        // back and verifies every captured bit. Heap alloc is fine: control-
        // driven, off the hot path.
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
        deinit();   // free the live unit; the test rebuilds its own on the data pins
        const auto r = platform::parlioWs2812Loopback(laneList_, laneCount_,
                                                      loopbackRxPin, frame,
                                                      frameBytes_, dataBytes,
                                                      static_cast<uint8_t>(outCh * 8));
        platform::free(frame);
        // Loopback verdict first, then reinit: if rebuilding the real unit fails
        // afterwards, kInitFailMsg overwrites the verdict — an unusable driver
        // matters more than a passed self-test.
        if (!r.jumperDetected) {
            clearFailBuf();
            setStatus("loopback: jumper not detected", Severity::Warning);
        } else if (r.pass) {
            clearFailBuf();
            setStatus("loopback PASS", Severity::Status);
        } else {
            if (!failBuf_) failBuf_ = static_cast<char*>(platform::alloc(kFailBufLen));
            if (failBuf_) {
                // Name the first corrupted light: rowBits = outCh*8, so
                // light = firstBadBit / rowBits.
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
        }
        reinit();
    }

    void clearFailBuf() {
        if (failBuf_) {
            if (status() == failBuf_) clearStatus();
            platform::free(failBuf_);
            failBuf_ = nullptr;
        }
    }
};

} // namespace mm
