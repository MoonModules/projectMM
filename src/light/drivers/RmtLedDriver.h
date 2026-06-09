#pragma once

#include "light/drivers/Drivers.h"        // DriverBase, Correction
#include "light/drivers/LedDriverConfig.h"
#include "light/drivers/RmtSymbol.h"       // encodeWs2812Symbols (host-testable)
#include "platform/platform.h"

#include <cstdint>

namespace mm {

// WS2812B output over the ESP32 RMT peripheral. One strand, 8-bit, GRB.
//
// This is the readable EXAMPLE future LED drivers copy. It reads as a sibling of
// ArtNetSendDriver: same DriverBase hooks, same per-light `correction_->apply()`
// guard pattern, same once-allocated owned buffer sized off the hot path. The
// only thing that differs is the emit — ArtNet packs corrected bytes into UDP
// universes; this fuses the correction and the WS2812 symbol-encode into one
// pass over the lights, then hands the symbols to the platform.
//
// Domain code only: the symbol encode lives in RmtSymbol.h; the platform owns
// just the peripheral (platform::rmtWs2812*). On non-ESP32 targets every
// platform call is an inert stub and the driver does nothing — guarded by
// `if constexpr (platform::isEsp32)` so it compiles everywhere.
class RmtLedDriver : public DriverBase {
public:
    uint8_t gpio = 18;  // data pin (live-editable). 18 is a standard WS2812 data
                        // pin and stays clear of GPIO 4/5, which the on-device
                        // loopback test (device_RmtLoopback.cpp) uses — so a strip
                        // on 18 and the loopback jumper on 4->5 coexist.

    // 40 MHz RMT tick clock = 25 ns/tick: t0h 350ns→14, t1h 700ns→28, period
    // 1250ns→50 ticks. The encoder converts ns→ticks via the granted resolution,
    // so this is the requested clock, not a hard-coded tick count.
    static constexpr uint32_t kResolutionHz = 40'000'000;

    void onBuildControls() override {
        controls_.addUint8("gpio", gpio, 0, 48);
    }

    // Changing the data pin re-inits the RMT channel (live, not reboot-to-apply),
    // so the pipeline-wide onBuildState sweep runs and reinit() picks up the pin.
    bool controlChangeTriggersBuildState(const char* name) const override {
        return std::strcmp(name, "gpio") == 0;
    }

    void setup() override { reinit(); }
    void teardown() override { deinit(); }

    // Topology (light count / channels) or the gpio control changed — resize the
    // symbol buffer and (re)init the channel off the hot path. loop() never allocates.
    void onBuildState() override {
        resizeSymbols();
        reinit();
        MoonModule::onBuildState();
    }

    // Preset toggle (RGB↔RGBW) changes outChannels without a structural rebuild.
    void onCorrectionChanged() override { resizeSymbols(); }

    void setSourceBuffer(Buffer* buf) override { sourceBuffer_ = buf; resizeSymbols(); }
    void setCorrection(const Correction* c) override { correction_ = c; resizeSymbols(); }

    void loop() override {
        if constexpr (!platform::isEsp32) return;   // inert off the chip with RMT
        if (!inited_ || !sourceBuffer_ || !sourceBuffer_->data() || !correction_) return;

        const nrOfLightsType n = sourceBuffer_->count();
        const uint8_t outCh = correction_->outChannels;
        // Same defensive guard ArtNet uses: skip rather than overrun if the
        // symbol buffer is stale (e.g. correction swapped without a resize).
        if (n == 0 || outCh == 0 || !symbols_ || symbolCap_ < symbolsFor(n, outCh)) return;

        // Fused single pass: correct one light into wire bytes, encode those
        // bytes straight into the symbol buffer. No second sweep over encoded
        // data, no per-light heap.
        const uint8_t* src = sourceBuffer_->data();
        const uint8_t srcCh = sourceBuffer_->channelsPerLight();
        const uint16_t t0h = nsToTicks(cfg_.t0h_ns);
        const uint16_t t1h = nsToTicks(cfg_.t1h_ns);
        const uint16_t period = nsToTicks(cfg_.period_ns);
        size_t s = 0;
        uint8_t wire[4];
        for (nrOfLightsType i = 0; i < n; i++) {
            correction_->apply(src + i * srcCh, wire);
            encodeWs2812Symbols(wire, outCh, t0h, t1h, period, symbols_ + s);
            s += static_cast<size_t>(outCh) * 8;
        }
        platform::rmtWs2812Show(rmt_, symbols_, s, cfg_.reset_us);
    }

private:
    // Source frame + shared correction — each physical driver owns these (the
    // contract shares Correction::apply(), not the member storage); same shape
    // as ArtNetSendDriver.
    Buffer* sourceBuffer_ = nullptr;
    const Correction* correction_ = nullptr;

    LedDriverConfig cfg_;
    platform::RmtWs2812Handle rmt_;
    bool inited_ = false;
    uint32_t* symbols_ = nullptr;   // owned; one word per WS2812 data bit
    size_t symbolCap_ = 0;          // words allocated

    static size_t symbolsFor(nrOfLightsType lights, uint8_t channels) {
        return static_cast<size_t>(lights) * channels * 8;
    }

    // Convert a ns duration to RMT ticks using the resolution the platform
    // granted. Falls back to the requested clock when not inited (host/desktop).
    uint16_t nsToTicks(uint32_t ns) const {
        uint32_t hz = inited_ ? platform::rmtWs2812Resolution(rmt_) : kResolutionHz;
        if (hz == 0) hz = kResolutionHz;
        return static_cast<uint16_t>((static_cast<uint64_t>(ns) * hz) / 1'000'000'000ull);
    }

    // (Re)allocate the symbol buffer for the current source + correction. Off the
    // hot path. Grows only — keeps a big-enough existing allocation.
    void resizeSymbols() {
        if (!sourceBuffer_ || !correction_) return;
        const nrOfLightsType n = sourceBuffer_->count();
        const uint8_t ch = correction_->outChannels;
        if (n == 0 || ch == 0) return;
        const size_t need = symbolsFor(n, ch);
        if (symbols_ && symbolCap_ >= need) return;
        if (symbols_) { platform::free(symbols_); symbols_ = nullptr; symbolCap_ = 0; }
        symbols_ = static_cast<uint32_t*>(platform::alloc(need * sizeof(uint32_t)));
        symbolCap_ = symbols_ ? need : 0;
    }

    void reinit() {
        if constexpr (!platform::isEsp32) return;
        deinit();
        inited_ = platform::rmtWs2812Init(rmt_, gpio, kResolutionHz, cfg_.invert);
    }

    void deinit() {
        if constexpr (!platform::isEsp32) return;
        if (inited_) { platform::rmtWs2812Deinit(rmt_); inited_ = false; }
    }
};

} // namespace mm
