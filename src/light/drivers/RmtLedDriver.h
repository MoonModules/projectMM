#pragma once

#include "light/drivers/Drivers.h"        // DriverBase, Correction
#include "light/drivers/LedDriverConfig.h"
#include "light/drivers/RmtSymbol.h"       // encodeWs2812Symbols (host-testable)
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>   // snprintf for the loopback status string

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
    // Pins are uint16 so the UI renders them as a number field, not a slider —
    // you type a GPIO number, you don't drag to it. (The peripheral validates the
    // pin at init; an out-of-range value just fails to bind.)
    uint16_t gpio = 18;  // data / TX pin (live-editable). 18 is a standard WS2812
                         // data pin. The loopback self-test transmits on THIS pin,
                         // so it validates the actual output.

    // Loopback self-test (replaces the old standalone test firmware): tick the
    // checkbox to run a one-shot RMT TX→RX round-trip — jumper `gpio` to
    // `loopbackRxPin`, the test transmits a known WS2812 pattern and captures it
    // back, proving the GPIO emits correct bytes on real silicon. The outcome
    // goes to the MoonModule status slot (setStatus) with the right severity; the
    // checkbox auto-resets.
    bool     loopbackTest = false;  // checkbox: set true to run once
    uint16_t loopbackRxPin = 5;     // jumper this to `gpio` for the test

    // 40 MHz RMT tick clock = 25 ns/tick: t0h 350ns→14, t1h 700ns→28, period
    // 1250ns→50 ticks. The encoder converts ns→ticks via the granted resolution,
    // so this is the requested clock, not a hard-coded tick count.
    static constexpr uint32_t kResolutionHz = 40'000'000;

    void onBuildControls() override {
        controls_.addUint16("gpio", gpio);
        controls_.addBool("loopbackTest", loopbackTest);
        // loopbackRxPin is always bound (so persistence can load it any time) but
        // only shown while the test mode is on — same always-add-then-setHidden
        // shape NetworkModule uses for its static-IP fields. The rebuild after
        // every control change (HttpServerModule) re-runs this and flips the flag.
        controls_.addUint16("loopbackRxPin", loopbackRxPin);
        controls_.setHidden(controls_.count() - 1, !loopbackTest);
    }

    // Changing the data pin re-inits the RMT channel (live, not reboot-to-apply),
    // so the pipeline-wide onBuildState sweep runs and reinit() picks up the pin.
    bool controlChangeTriggersBuildState(const char* name) const override {
        return std::strcmp(name, "gpio") == 0;
    }

    // React to a control change (runs off the render loop, in the HTTP/API
    // handler context — a blocking self-test here is fine). loopbackTest is a
    // persistent on/off mode. While it's ON, the test (re-)runs on every relevant
    // change — turning it on, OR editing gpio / loopbackRxPin — so the pins can be
    // set in any order and the result always reflects the current pins. Turning it
    // OFF clears the result.
    void onUpdate(const char* name) override {
        const bool isTestControl = std::strcmp(name, "loopbackTest") == 0;
        const bool isPinControl  = std::strcmp(name, "gpio") == 0
                                || std::strcmp(name, "loopbackRxPin") == 0;
        if (isTestControl && !loopbackTest) {
            clearFailBuf();
            clearStatus();
        } else if (loopbackTest && (isTestControl || isPinControl)) {
            runLoopbackSelfTest();
        }
    }

    // Lifecycle has two deliberately-separate concerns, so the buffer half stays
    // host-testable and a hardware-only guard can never strand it:
    //   - SYMBOL BUFFER (plain heap): resizeSymbols() / freeSymbols(), run on
    //     every platform.
    //   - RMT CHANNEL (hardware): reinit() / deinit(), ESP32-only (if constexpr).
    // The original bug put the buffer free inside the hardware deinit(), which
    // reinit() (a rebuild) calls — so a rebuild freed the buffer loop() needs.
    // Keeping the two apart makes that mistake impossible here and lets the host
    // unit test (unit_RmtLedDriver_lifecycle.cpp) pin it.
    void setup() override { reinit(); }
    void teardown() override { deinit(); freeSymbols(); clearFailBuf(); }

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

    // Test-only accessors for the symbol-buffer lifecycle. Mirror ArtNet's
    // correctedBuffer() accessor. Let unit tests pin the invariants a hardware
    // bug already taught us: the buffer is sized in onBuildState (never in
    // loop), it SURVIVES a gpio-change reinit (reinit must not free it), and it
    // is released on teardown (no leak). Not part of any runtime API.
    const uint32_t* symbolBuffer() const { return symbols_; }
    size_t symbolCapacity() const { return symbolCap_; }

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

    // On-demand FAIL status string — only allocated when a loopback FAILs (the
    // one outcome needing the captured hex). nullptr otherwise; freed on teardown
    // and on any non-FAIL result. PASS / jumper / unsupported use flash literals.
    static constexpr size_t kFailBufLen = 48;
    char* failBuf_ = nullptr;

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

    // --- symbol buffer (plain heap; runs on every platform) ---

    // (Re)allocate the symbol buffer for the current source + correction. Off the
    // hot path. Grows only — keeps a big-enough existing allocation.
    void resizeSymbols() {
        if (!sourceBuffer_ || !correction_) return;
        const nrOfLightsType n = sourceBuffer_->count();
        const uint8_t ch = correction_->outChannels;
        if (n == 0 || ch == 0) return;
        const size_t need = symbolsFor(n, ch);
        if (symbols_ && symbolCap_ >= need) return;
        freeSymbols();
        symbols_ = static_cast<uint32_t*>(platform::alloc(need * sizeof(uint32_t)));
        symbolCap_ = symbols_ ? need : 0;
    }

    void freeSymbols() {
        if (symbols_) { platform::free(symbols_); symbols_ = nullptr; symbolCap_ = 0; }
    }

    // --- loopback self-test (control-driven) ---

    // Run the one-shot RMT TX→RX loopback and report via the MoonModule status
    // slot. The slot stores a const char* (no copy), so PASS / jumper-missing /
    // not-supported are flash literals — zero RAM. Only the FAIL case needs the
    // captured hex, so it borrows a buffer allocated ON DEMAND and freed by
    // clearFailBuf() (teardown + every non-FAIL outcome) — no permanent member.
    void runLoopbackSelfTest() {
        if constexpr (!platform::isEsp32) {
            clearFailBuf();
            setStatus("loopback: not supported on this platform", Severity::Warning);
            return;
        }
        // The test reconfigures the data pin (gpio) as TX, so release our normal
        // RMT channel first; reinit() restores it after.
        deinit();
        const auto r = platform::rmtWs2812Loopback(gpio, loopbackRxPin);
        reinit();
        if (!r.jumperDetected) {
            clearFailBuf();
            setStatus("loopback: jumper not detected", Severity::Warning);
        } else if (r.pass) {
            clearFailBuf();
            setStatus("loopback PASS", Severity::Status);
        } else {
            if (!failBuf_) failBuf_ = static_cast<char*>(platform::alloc(kFailBufLen));
            if (failBuf_) {
                std::snprintf(failBuf_, kFailBufLen, "loopback FAIL: sent %02X%02X%02X got %02X%02X%02X",
                              r.sent[0], r.sent[1], r.sent[2], r.got[0], r.got[1], r.got[2]);
                setStatus(failBuf_, Severity::Error);
            } else {
                setStatus("loopback FAIL", Severity::Error);
            }
        }
    }

    // Release the on-demand FAIL string and drop any status pointing into it, so
    // no allocation outlives a non-FAIL result or the module.
    void clearFailBuf() {
        if (failBuf_) {
            if (status() == failBuf_) clearStatus();
            platform::free(failBuf_);
            failBuf_ = nullptr;
        }
    }

    // --- RMT channel (hardware; ESP32-only) ---

    void reinit() {
        if constexpr (!platform::isEsp32) return;
        deinit();
        inited_ = platform::rmtWs2812Init(rmt_, gpio, kResolutionHz, cfg_.invert);
    }

    // Releases only the RMT channel — NOT the symbol buffer (that's freeSymbols(),
    // owned by teardown). reinit() calls this on every rebuild, so freeing the
    // buffer here would strand loop() — the original bug.
    void deinit() {
        if constexpr (!platform::isEsp32) return;
        if (inited_) { platform::rmtWs2812Deinit(rmt_); inited_ = false; }
    }
};

} // namespace mm
