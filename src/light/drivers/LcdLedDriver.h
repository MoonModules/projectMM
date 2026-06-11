#pragma once

#include "light/drivers/Drivers.h"        // DriverBase, Correction
#include "light/drivers/LcdSlots.h"        // encodeWs2812LcdSlots (host-testable)
#include "light/drivers/LedDriverConfig.h"
#include "light/drivers/PinList.h"         // parsePinList / assignCounts (shared with RmtLedDriver)
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>   // snprintf for status strings
#include <cstring>  // std::strcmp, std::memset

namespace mm {

// WS2812B output over the ESP32-S3 LCD_CAM i80 bus: up to 8 strands clock out
// SIMULTANEOUSLY, one GPIO lane each, fed consecutive slices of the source
// buffer. The S3's scale path — RMT gives it 4 channels, this gives 8 lanes
// for the wall time of one.
//
// Reads as a sibling of RmtLedDriver: same pins/ledsPerPin controls (parsers
// shared via PinList.h), same fused per-light correction+encode, same status
// reporting. The differences: the encode bit-transposes a ROW (the same light
// index across all lanes, LcdSlots.h) instead of one strand linearly, and the
// lifecycle is deliberately FUSED — the platform owns the DMA frame buffer
// (it must be DMA-capable internal RAM with the bus's alignment), and the
// i80 bus's max transfer size is fixed at creation, so re-creating the bus IS
// the buffer resize. The RMT driver's buffer-freed-by-rebuild lesson cannot
// recur here: a failed reinit nulls both dmaBuf_ and inited_, so loop() idles
// instead of writing through a stale pointer.
//
// The whole frame (plus a >=300 µs zeroed latch pad) is pre-encoded off any
// ISR path, then one autonomous GDMA transfer ships it — no refill deadlines,
// so the WiFi-induced bit-slip of refill-based drivers can't occur. Prior
// art: Adafruit's LCD_CAM discovery, hpwit's I2SClockless lineage, FastLED's
// S3 driver — architecture studied, never copied (see LcdLedDriver.md).
class LcdLedDriver : public DriverBase {
public:
    // Bus width this increment: 8 of the peripheral's 16 lanes (constant
    // matches platform::lcdLanes; widening to 16 is a later increment).
    static constexpr uint8_t kMaxLanes = 8;

    // Comma-separated data GPIO list, one lane per pin. Defaults stay clear of
    // the LOLIN S3's octal-PSRAM pins (26-37), native USB (19/20) and
    // strapping pins. The loopback self-test transmits on the FIRST pin.
    char pins[24] = "1,2,4,5,6,7,8,9";

    // Lights per lane, matched to `pins` by position — same semantics as
    // RmtLedDriver (empty = even split, remainder to the last lane).
    char ledsPerPin[48] = "";

    // The i80 peripheral requires its WR (pixel clock) and DC lines on real
    // GPIOs even though WS2812 strands ignore both — two sacrificial pins.
    uint16_t clockPin = 10;
    uint16_t dcPin = 11;

    // Loopback self-test: jumper the FIRST pin in `pins` to `loopbackRxPin`,
    // tick the box — the driver transmits a known pattern on the LCD bus and
    // captures it back with an RMT RX channel (transmitter-agnostic, the
    // increment-1 rig reused). Result lands in the status slot.
    bool     loopbackTest = false;
    uint16_t loopbackRxPin = 12;

    void onBuildControls() override {
        controls_.addText("pins", pins, sizeof(pins));
        controls_.addText("ledsPerPin", ledsPerPin, sizeof(ledsPerPin));
        controls_.addUint16("clockPin", clockPin);
        controls_.addUint16("dcPin", dcPin);
        controls_.addBool("loopbackTest", loopbackTest);
        // Always bound, shown only in test mode — the NetworkModule/RmtLedDriver
        // conditional-control shape.
        controls_.addUint16("loopbackRxPin", loopbackRxPin);
        controls_.setHidden(controls_.count() - 1, !loopbackTest);
    }

    bool controlChangeTriggersBuildState(const char* name) const override {
        return std::strcmp(name, "pins") == 0 || std::strcmp(name, "ledsPerPin") == 0
            || std::strcmp(name, "clockPin") == 0 || std::strcmp(name, "dcPin") == 0;
    }

    void onUpdate(const char* name) override {
        const bool isTestControl = std::strcmp(name, "loopbackTest") == 0;
        const bool isPinControl  = std::strcmp(name, "pins") == 0
                                || std::strcmp(name, "loopbackRxPin") == 0;
        if (isTestControl && !loopbackTest) {
            // Toggling the test off clears the loopback's own verdict (FAIL
            // buffer or the PASS/jumper string), then re-derives the driver's
            // real status — a config/init error must survive, which a blind
            // clearStatus() would hide.
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
        if constexpr (platform::lcdLanes == 0) return;  // inert off i80 chips
        if (!inited_ || !dmaBuf_ || !sourceBuffer_ || !sourceBuffer_->data()
            || !correction_ || laneCount_ == 0 || maxLaneLights_ == 0) return;
        const uint8_t outCh = correction_->outChannels;
        if (outCh == 0 || frameBytes_ > platform::lcdWs2812BufferCapacity(lcd_)) return;

        // Fused per-ROW pass: correct the same light index of every active lane
        // into an 8x4 wire block, then transpose it into 3-slot bus bytes. No
        // heap, integer math only.
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
        // here, so the transfer ends holding every lane LOW for >=300 µs.
        // Only wait when the transfer actually started: a failed tx_color gives
        // no done-callback, so an unconditional wait would block the full 1000 ms
        // timeout every tick. Drop the frame and retry next tick (self-heals).
        if (platform::lcdWs2812Transmit(lcd_, frameBytes_))
            platform::lcdWs2812Wait(lcd_, 1000 /* ms */);
    }

    // Test-only accessors — pin the lane slicing and frame-size arithmetic on
    // the host (unit_LcdLedDriver.cpp); the hardware half is proven on device.
    uint8_t laneCount() const { return laneCount_; }
    nrOfLightsType laneLightCount(uint8_t i) const { return i < laneCount_ ? laneCounts_[i] : 0; }
    nrOfLightsType laneStart(uint8_t i) const { return i < laneCount_ ? laneStart_[i] : 0; }
    nrOfLightsType maxLaneLights() const { return maxLaneLights_; }
    size_t frameBytes() const { return frameBytes_; }

private:
    Buffer* sourceBuffer_ = nullptr;
    const Correction* correction_ = nullptr;

    LedDriverConfig cfg_;
    platform::LcdWs2812Handle lcd_;
    bool inited_ = false;
    uint8_t* dmaBuf_ = nullptr;          // platform-owned; cached for the encode
    uint16_t busPins_[kMaxLanes + 2] = {};  // pins the live bus was built with
                                            // (data lanes + WR + DC) — a pin
                                            // change must rebuild the bus even
                                            // when the buffer still fits
    uint16_t laneList_[kMaxLanes] = {};
    nrOfLightsType laneCounts_[kMaxLanes] = {};
    nrOfLightsType laneStart_[kMaxLanes] = {};
    uint8_t laneCount_ = 0;
    nrOfLightsType maxLaneLights_ = 0;
    size_t frameBytes_ = 0;

    const char* configErr_ = nullptr;
    static constexpr size_t kFailBufLen = 48;
    char* failBuf_ = nullptr;
    static constexpr const char* kInitFailMsg = "LCD init failed — check pins / memory";

    static constexpr uint8_t maxLanesForTarget() {
        return (platform::lcdLanes > 0 && platform::lcdLanes < kMaxLanes)
                   ? platform::lcdLanes
                   : kMaxLanes;
    }

    // Frame bytes: longest lane × channels × 24 slot bytes, plus a zeroed
    // latch pad of >=300 µs at 2.67 MHz (800 B) with clock-tolerance slack,
    // rounded up to the bus's 64-byte alignment. 0 when there is nothing to
    // send (empty grid) — no pad for an empty frame.
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
        // IDF's i80 layer requires a real GPIO on every data line up to the bus
        // width — a partial bus is rejected at esp_lcd_new_i80_bus(). So the
        // pin list must name all 8 lanes; strands you don't use get 0 in
        // ledsPerPin and idle LOW.
        if (!err && n != kMaxLanes) err = "LCD bus needs exactly 8 pins";
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

    // --- bus + DMA buffer (hardware; i80 targets only). Fused on purpose:
    // max_transfer_bytes is fixed at bus creation, so growing the frame means
    // re-creating the bus and its buffer together. Grow-only; the buffer is
    // re-zeroed on every reinit so a shrunken frame's latch pad can't contain
    // stale row bytes. ---

    void reinit() {
        if constexpr (platform::lcdLanes == 0) return;
        if (laneCount_ == 0 || frameBytes_ == 0) { deinit(); return; }
        if (inited_ && platform::lcdWs2812BufferCapacity(lcd_) >= frameBytes_
            && busPinsCurrent()) {
            // Existing bus + buffer still fit AND the bus sits on the wanted
            // pins — just clear stale bytes so the (possibly relocated) latch
            // pad is zero. The pin check matters: a pins/clockPin/dcPin edit
            // keeps the frame size, and without it the bus would keep clocking
            // out on the OLD GPIOs.
            std::memset(dmaBuf_, 0, platform::lcdWs2812BufferCapacity(lcd_));
            return;
        }
        deinit();
        inited_ = platform::lcdWs2812Init(lcd_, laneList_, laneCount_,
                                          clockPin, dcPin, frameBytes_);
        dmaBuf_ = inited_ ? platform::lcdWs2812Buffer(lcd_) : nullptr;
        if (inited_) {
            for (uint8_t i = 0; i < kMaxLanes; i++) busPins_[i] = laneList_[i];
            busPins_[kMaxLanes] = clockPin;
            busPins_[kMaxLanes + 1] = dcPin;
        }
        if (!inited_) {
            clearFailBuf();
            setStatus(kInitFailMsg, Severity::Error);
        } else if (status() == kInitFailMsg) {
            clearStatus();
        }
    }

    void deinit() {
        if constexpr (platform::lcdLanes == 0) return;
        if (inited_) platform::lcdWs2812Deinit(lcd_);
        inited_ = false;
        dmaBuf_ = nullptr;
    }

    bool busPinsCurrent() const {
        for (uint8_t i = 0; i < laneCount_; i++)
            if (busPins_[i] != laneList_[i]) return false;
        return busPins_[kMaxLanes] == clockPin && busPins_[kMaxLanes + 1] == dcPin;
    }

    // --- loopback self-test (control-driven; same status shapes as RMT) ---

    void runLoopbackSelfTest() {
        if constexpr (platform::lcdLanes == 0) {
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
        // Build the REAL frame with the test pattern in every row on lane 0
        // only — the platform then transmits the genuine transfer (size, DMA
        // chain, latch pad) back to back and verifies every captured bit, so
        // the test covers what the render loop actually sends, not a short
        // synthetic burst. Heap alloc is fine here: control-driven, off the
        // hot path.
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
        deinit();   // the test builds its own full-width bus on the same data pins
                    // (IDF's i80 layer requires all 8 data GPIOs valid, so a
                    // 1-lane private bus is impossible; lane 0 carries the pattern)
        const auto r = platform::lcdWs2812Loopback(laneList_, laneCount_,
                                                   clockPin, dcPin, loopbackRxPin,
                                                   frame, frameBytes_, dataBytes,
                                                   static_cast<uint8_t>(outCh * 8));
        platform::free(frame);
        // Loopback result first, then reinit: if rebuilding the real bus fails
        // afterwards, kInitFailMsg overwrites the loopback verdict — an unusable
        // driver matters more than a passed self-test.
        if (!r.jumperDetected) {
            clearFailBuf();
            setStatus("loopback: jumper not detected", Severity::Warning);
        } else if (r.pass) {
            clearFailBuf();
            setStatus("loopback PASS", Severity::Status);
        } else {
            if (!failBuf_) failBuf_ = static_cast<char*>(platform::alloc(kFailBufLen));
            if (failBuf_) {
                // Name the first corrupted light (the spec promises this): the
                // loopback reports the first mismatching bit; rowBits = outCh*8,
                // so light = firstBadBit / rowBits.
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
