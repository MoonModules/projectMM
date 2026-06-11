#pragma once

#include "light/drivers/Drivers.h"        // DriverBase, Correction
#include "light/drivers/LedDriverConfig.h"
#include "light/drivers/PinList.h"         // parsePinList / assignCounts (shared with LcdLedDriver)
#include "light/drivers/RmtSymbol.h"       // encodeWs2812Symbols (host-testable)
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>   // snprintf for the loopback status string
#include <cstring>  // std::strcmp in onUpdate / controlChangeTriggersBuildState

namespace mm {

// WS2812B output over the ESP32 RMT peripheral. One or more strands — one GPIO
// and one RMT TX channel per strand — fed consecutive slices of the source
// buffer, 8-bit, GRB.
//
// This is the readable EXAMPLE future LED drivers copy. It reads as a sibling of
// NetworkSendDriver: same DriverBase hooks, same per-light `correction_->apply()`
// guard pattern, same once-allocated owned buffer sized off the hot path. The
// only thing that differs is the emit — ArtNet packs corrected bytes into UDP
// universes; this fuses the correction and the WS2812 symbol-encode into one
// pass over the lights, then hands per-pin slices of the symbols to the
// platform. All channels are started before any is waited on, so a multi-pin
// frame costs the longest strand, not the sum.
//
// Domain code only: the symbol encode lives in RmtSymbol.h; the platform owns
// just the peripheral (platform::rmtWs2812*). On targets without RMT every
// platform call is an inert stub and the driver does nothing — guarded by
// `if constexpr (platform::rmtTxChannels == 0)` so it compiles everywhere.
// The pin/count parsing and buffer slicing run on every platform, which is what
// lets the host unit tests (unit_RmtLedDriver_pins.cpp) pin them.
class RmtLedDriver : public DriverBase {
public:
    // Hard cap on the pin arrays: the largest RMT TX group of any supported
    // chip (8 on classic ESP32; the S3 has 4 — enforced per target via
    // maxPinsForTarget()). A fixed array bounded by a hardware constant, not a
    // dynamic list: the bound can't grow at runtime.
    static constexpr uint8_t kMaxPins = 8;

    // Comma-separated GPIO list, one RMT TX channel per pin ("18,17,16"). Text
    // control so one field holds N pins — per-output (pin, count) rows are the
    // WLED LED-settings pattern. The peripheral validates each pin at init; a
    // parse error or failing pin lands in the status field and the driver idles.
    // 24 bytes fit kMaxPins 2-digit GPIOs plus separators. The loopback
    // self-test transmits on the FIRST pin, so it validates the actual output.
    char pins[24] = "18";

    // Comma-separated lights-per-pin ("100,100,50"), matched to `pins` by
    // position — each pin takes the next consecutive slice of the source
    // buffer. May be empty or shorter than `pins`: the unassigned remainder
    // splits evenly over the remaining pins (last pin takes the rounding
    // remainder), so the empty default just splits the whole buffer evenly.
    char ledsPerPin[48] = "";

    // Loopback self-test (replaces the old standalone test firmware): tick the
    // checkbox to run a one-shot RMT TX→RX round-trip — jumper the FIRST pin in
    // `pins` to `loopbackRxPin`, the test transmits a known WS2812 pattern and
    // captures it back, proving the GPIO emits correct bytes on real silicon.
    // The outcome goes to the MoonModule status slot (setStatus) with the right
    // severity; the checkbox auto-resets.
    bool     loopbackTest = false;  // checkbox: set true to run once
    uint16_t loopbackRxPin = 5;     // jumper this to the first pin for the test

    // Whole-frame stress variant: instead of a 24-bit burst, transmit a real
    // frame the size of the first pin's slice, back to back, and bit-verify the
    // WHOLE capture. This is the one that catches frame-rate corruption and RF
    // interference on the data line (the flicker class of bug) — a 24-bit burst
    // passes through a wire that mangles a sustained frame. Shown only in test
    // mode; the status names the first corrupted light on failure.
    bool     loopbackFrame = false;

    // 40 MHz RMT tick clock = 25 ns/tick: t0h 350ns→14, t1h 700ns→28, period
    // 1250ns→50 ticks. The encoder converts ns→ticks via the granted resolution,
    // so this is the requested clock, not a hard-coded tick count.
    static constexpr uint32_t kResolutionHz = 40'000'000;

    // The pin/count list parsing (parsePinList / assignCounts) lives in
    // PinList.h, shared with LcdLedDriver — both drivers slice the source
    // buffer from the same two text controls.

    void onBuildControls() override {
        controls_.addText("pins", pins, sizeof(pins));
        controls_.addText("ledsPerPin", ledsPerPin, sizeof(ledsPerPin));
        controls_.addBool("loopbackTest", loopbackTest);
        // loopbackRxPin is always bound (so persistence can load it any time) but
        // only shown while the test mode is on — same always-add-then-setHidden
        // shape NetworkModule uses for its static-IP fields. The rebuild after
        // every control change (HttpServerModule) re-runs this and flips the flag.
        controls_.addUint16("loopbackRxPin", loopbackRxPin);
        controls_.setHidden(controls_.count() - 1, !loopbackTest);
        controls_.addBool("loopbackFrame", loopbackFrame);
        controls_.setHidden(controls_.count() - 1, !loopbackTest);
    }

    // Changing the pin list or the per-pin counts re-parses and re-inits the RMT
    // channels (live, not reboot-to-apply), so the pipeline-wide onBuildState
    // sweep runs and parseConfig()/reinit() pick up the new lists.
    bool controlChangeTriggersBuildState(const char* name) const override {
        return std::strcmp(name, "pins") == 0 || std::strcmp(name, "ledsPerPin") == 0;
    }

    // React to a control change (runs off the render loop, in the HTTP/API
    // handler context — a blocking self-test here is fine). loopbackTest is a
    // persistent on/off mode. While it's ON, the test (re-)runs on every relevant
    // change — turning it on, OR editing pins / loopbackRxPin — so the pins can be
    // set in any order and the result always reflects the current pins. Turning it
    // OFF clears the result.
    void onUpdate(const char* name) override {
        const bool isTestControl = std::strcmp(name, "loopbackTest") == 0;
        const bool isPinControl  = std::strcmp(name, "pins") == 0
                                || std::strcmp(name, "loopbackRxPin") == 0
                                || std::strcmp(name, "loopbackFrame") == 0;
        if (isTestControl && !loopbackTest) {
            // Toggling the test off clears the loopback verdict, then re-derives
            // the real driver status — a config/init error must survive (a blind
            // clearStatus() would hide it).
            clearFailBuf();
            clearStatus();
            parseConfig();
            reinit();
        } else if (loopbackTest && (isTestControl || isPinControl)) {
            runLoopbackSelfTest();
        }
    }

    // Lifecycle has two deliberately-separate concerns, so the buffer half stays
    // host-testable and a hardware-only guard can never strand it:
    //   - SYMBOL BUFFER (plain heap): resizeSymbols() / freeSymbols(), run on
    //     every platform.
    //   - RMT CHANNELS (hardware): reinit() / deinitAll(), RMT-targets-only
    //     (if constexpr).
    // The original bug put the buffer free inside the hardware deinit(), which
    // reinit() (a rebuild) calls — so a rebuild freed the buffer loop() needs.
    // Keeping the two apart makes that mistake impossible here and lets the host
    // unit test (unit_RmtLedDriver_lifecycle.cpp) pin it.
    void setup() override { parseConfig(); reinit(); }
    void teardown() override {
        deinitAll();
        freeSymbols();
        clearFailBuf();
        clearConfigErr();
    }

    // Topology (light count / channels) or the pins/ledsPerPin controls changed —
    // re-parse the lists, resize the symbol buffer, and (re)init the channels off
    // the hot path. loop() never allocates.
    void onBuildState() override {
        parseConfig();
        resizeSymbols();
        reinit();
        MoonModule::onBuildState();
    }

    // Preset toggle (RGB↔RGBW) changes outChannels without a structural rebuild —
    // the per-pin symbol offsets scale with outChannels, so re-derive them too.
    void onCorrectionChanged() override { parseConfig(); resizeSymbols(); }

    void setSourceBuffer(Buffer* buf) override {
        sourceBuffer_ = buf;
        parseConfig();      // counts derive from the buffer's light count
        resizeSymbols();
    }
    void setCorrection(const Correction* c) override {
        correction_ = c;
        parseConfig();      // offsets derive from outChannels
        resizeSymbols();
    }

    void loop() override {
        if constexpr (platform::rmtTxChannels == 0) return;  // inert off RMT chips
        if (!inited_ || !sourceBuffer_ || !sourceBuffer_->data() || !correction_) return;

        const nrOfLightsType n = sourceBuffer_->count();
        const uint8_t outCh = correction_->outChannels;
        // Same defensive guard ArtNet uses: skip rather than overrun if the
        // symbol buffer is stale (e.g. correction swapped without a resize).
        if (n == 0 || outCh == 0 || pinCount_ == 0
            || !symbols_ || symbolCap_ < symbolsFor(n, outCh)) return;

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
        // Start every pin's slice before waiting on any — the channels clock out
        // concurrently, so the tick is charged the longest strand, not the sum.
        // The shared reset gap (the WS2812 latch) runs once, after the last wait.
        // Wait ONLY on channels whose transmit started: a failed transmit gives
        // no done-callback, so waiting on it would block the full 1000 ms timeout
        // and a single bad pin would stall the tick (the same guard the LCD /
        // Parlio loops use, here per channel).
        bool started[kMaxPins] = {};
        for (uint8_t i = 0; i < pinCount_; i++) {
            if (pinCounts_[i] == 0) continue;
            started[i] = platform::rmtWs2812Transmit(rmt_[i], symbols_ + pinOffsets_[i],
                                        static_cast<size_t>(pinCounts_[i]) * outCh * 8);
        }
        for (uint8_t i = 0; i < pinCount_; i++) {
            if (started[i]) platform::rmtWs2812Wait(rmt_[i], 1000 /* ms */);
        }
        if (cfg_.reset_us) platform::delayUs(cfg_.reset_us);
    }

    // Test-only accessors. symbolBuffer/symbolCapacity mirror ArtNet's
    // correctedBuffer() and let unit tests pin the buffer-lifecycle invariants a
    // hardware bug already taught us; pinCount/pinLightCount/pinSymbolOffsetWords
    // pin the multi-pin slice arithmetic (unit_RmtLedDriver_pins.cpp). Not part
    // of any runtime API.
    const uint32_t* symbolBuffer() const { return symbols_; }
    size_t symbolCapacity() const { return symbolCap_; }
    uint8_t pinCount() const { return pinCount_; }
    nrOfLightsType pinLightCount(uint8_t i) const { return i < pinCount_ ? pinCounts_[i] : 0; }
    size_t pinSymbolOffsetWords(uint8_t i) const { return i < pinCount_ ? pinOffsets_[i] : 0; }

private:
    // Source frame + shared correction — each physical driver owns these (the
    // contract shares Correction::apply(), not the member storage); same shape
    // as NetworkSendDriver.
    Buffer* sourceBuffer_ = nullptr;
    const Correction* correction_ = nullptr;

    LedDriverConfig cfg_;
    platform::RmtWs2812Handle rmt_[kMaxPins];
    uint16_t       pinList_[kMaxPins] = {};    // parsed pins, list order
    nrOfLightsType pinCounts_[kMaxPins] = {};  // lights per pin (slice lengths)
    size_t         pinOffsets_[kMaxPins] = {}; // slice start in symbols_, words
    uint8_t pinCount_ = 0;                     // 0 = idle (parse error / no pins)
    bool inited_ = false;                      // all-or-nothing across the pins
    uint32_t* symbols_ = nullptr;   // owned; one word per WS2812 data bit
    size_t symbolCap_ = 0;          // words allocated

    // The parse-error literal currently shown in the status slot (nullptr when
    // the config is valid) — tracked so a successful re-parse clears only our
    // own error, never a loopback result.
    const char* configErr_ = nullptr;

    // On-demand FAIL status string — only allocated when a loopback or channel
    // init FAILs (the outcomes needing a formatted message). nullptr otherwise;
    // freed on teardown and on any non-FAIL result. PASS / jumper / unsupported
    // use flash literals.
    static constexpr size_t kFailBufLen = 48;
    char* failBuf_ = nullptr;

    // The chip's TX-channel cap caps the pin list; on targets without RMT
    // (desktop, where the constant is 0) fall back to kMaxPins so the parsing
    // and slicing stay fully host-testable.
    static constexpr uint8_t maxPinsForTarget() {
        return (platform::rmtTxChannels > 0 && platform::rmtTxChannels < kMaxPins)
                   ? platform::rmtTxChannels
                   : kMaxPins;
    }

    static size_t symbolsFor(nrOfLightsType lights, uint8_t channels) {
        return static_cast<size_t>(lights) * channels * 8;
    }

    // Convert a ns duration to RMT ticks using the resolution the platform
    // granted. Falls back to the requested clock when not inited (host/desktop).
    uint16_t nsToTicks(uint32_t ns) const {
        uint32_t hz = inited_ ? platform::rmtWs2812Resolution(rmt_[0]) : kResolutionHz;
        if (hz == 0) hz = kResolutionHz;
        return static_cast<uint16_t>((static_cast<uint64_t>(ns) * hz) / 1'000'000'000ull);
    }

    // --- pin/count config (plain parsing; runs on every platform) ---

    // Re-derive pinList_/pinCounts_/pinOffsets_ from the two text controls and
    // the current buffer/correction. On error: pinCount_ = 0 (loop() idles) and
    // the static error literal goes to the status slot; a later successful parse
    // clears it. Off the hot path.
    bool parseConfig() {
        pinCount_ = 0;
        uint8_t n = 0;
        const char* err = parsePinList(pins, pinList_, maxPinsForTarget(), n);
        if (!err) {
            const nrOfLightsType total = sourceBuffer_ ? sourceBuffer_->count() : 0;
            err = assignCounts(ledsPerPin, n, total, pinCounts_);
        }
        if (err) {
            configErr_ = err;
            setStatus(err, Severity::Error);
            return false;
        }
        pinCount_ = n;
        const uint8_t outCh = correction_ ? correction_->outChannels : 0;
        size_t off = 0;
        for (uint8_t i = 0; i < pinCount_; i++) {
            pinOffsets_[i] = off;
            off += static_cast<size_t>(pinCounts_[i]) * outCh * 8;
        }
        clearConfigErr();
        return true;
    }

    // Drop our parse-error status (and only ours — a loopback result stays).
    void clearConfigErr() {
        if (configErr_) {
            if (status() == configErr_) clearStatus();
            configErr_ = nullptr;
        }
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

    // Run the one-shot RMT TX→RX loopback on the FIRST pin and report via the
    // MoonModule status slot. The slot stores a const char* (no copy), so PASS /
    // jumper-missing / not-supported are flash literals — zero RAM. Only the
    // FAIL case needs the captured hex, so it borrows a buffer allocated ON
    // DEMAND and freed by clearFailBuf() (teardown + every non-FAIL outcome) —
    // no permanent member.
    void runLoopbackSelfTest() {
        if constexpr (platform::rmtTxChannels == 0) {
            clearFailBuf();
            setStatus("loopback: not supported on this platform", Severity::Warning);
            return;
        }
        if (pinCount_ == 0) {
            clearFailBuf();
            setStatus("loopback: no valid pins", Severity::Warning);
            return;
        }
        // The test reconfigures the first data pin as TX, so release ALL our TX
        // channels first — this also guarantees the test's RX channel can always
        // allocate RMT memory, even with every TX channel otherwise claimed;
        // reinit() restores them after.
        deinitAll();
        const uint8_t txPin = static_cast<uint8_t>(pinList_[0]);
        const uint8_t rxPin = static_cast<uint8_t>(loopbackRxPin);
        platform::RmtLoopbackResult r;
        if (loopbackFrame) {
            // Whole-frame stress test on the first pin's slice (or 64 lights if
            // no buffer is wired yet) — the size that actually exposes
            // frame-rate / RF corruption.
            const uint16_t lights = pinCounts_[0] > 0
                ? static_cast<uint16_t>(pinCounts_[0]) : 64;
            const uint8_t ch = correction_ ? correction_->outChannels : 3;
            r = platform::rmtWs2812LoopbackFrame(txPin, rxPin, lights, ch);
        } else {
            r = platform::rmtWs2812Loopback(txPin, rxPin);
        }
        reinit();
        if (!r.jumperDetected) {
            clearFailBuf();
            setStatus("loopback: jumper not detected", Severity::Warning);
        } else if (r.pass) {
            // PASS is a static literal (no failBuf_ alloc): setStatus holds the
            // pointer, not a copy, so the string must outlive the call, and
            // failBuf_ is by invariant a FAIL-only buffer (see clearFailBuf).
            // The whole-frame bit count is in the serial log; the status slot
            // doesn't need it badly enough to break either rule.
            clearFailBuf();
            setStatus("loopback PASS", Severity::Status);
        } else {
            if (!failBuf_) failBuf_ = static_cast<char*>(platform::alloc(kFailBufLen));
            if (failBuf_ && loopbackFrame) {
                // bits per light = outChannels × 8 (24 for RGB, 32 for RGBW) —
                // the same channel count the frame was built with, not a
                // hardcoded /24, so the light index is right for RGBW too.
                const unsigned bitsPerLight =
                    (correction_ ? correction_->outChannels : 3u) * 8u;
                std::snprintf(failBuf_, kFailBufLen,
                              "loopback FAIL: bad bit %u/%u (light %u)",
                              static_cast<unsigned>(r.firstBadBit),
                              static_cast<unsigned>(r.bitsChecked),
                              static_cast<unsigned>(r.firstBadBit / bitsPerLight));
                setStatus(failBuf_, Severity::Error);
            } else if (failBuf_) {
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

    // --- RMT channels (hardware; RMT targets only) ---

    static constexpr const char* kInitFailMsg = "RMT init failed — check the pins";

    // All-or-nothing: a failing pin deinits everything and reports which pin,
    // so loop()'s guard stays a single bool and the user sees one clear error
    // instead of some strands dark, some lit.
    void reinit() {
        if constexpr (platform::rmtTxChannels == 0) return;
        deinitAll();
        if (pinCount_ == 0) return;   // parse error — already in the status slot
        for (uint8_t i = 0; i < pinCount_; i++) {
            if (platform::rmtWs2812Init(rmt_[i], static_cast<uint8_t>(pinList_[i]),
                                        kResolutionHz, cfg_.invert)) continue;
            // Surface which pin failed instead of silently no-op'ing in loop() —
            // the status tells the user why output is dark (usually a bad pin),
            // rather than leaving them to wonder why nothing lights.
            deinitAll();
            clearFailBuf();
            failBuf_ = static_cast<char*>(platform::alloc(kFailBufLen));
            if (failBuf_) {
                std::snprintf(failBuf_, kFailBufLen, "RMT init failed on pin %u",
                              static_cast<unsigned>(pinList_[i]));
                setStatus(failBuf_, Severity::Error);
            } else {
                setStatus(kInitFailMsg, Severity::Error);
            }
            return;
        }
        inited_ = true;
        // A prior init failure recovered (e.g. a pin fixed) — drop the stale error.
        if (failBuf_ && status() == failBuf_) clearFailBuf();
        if (status() == kInitFailMsg) clearStatus();
    }

    // Releases only the RMT channels — NOT the symbol buffer (that's
    // freeSymbols(), owned by teardown). reinit() calls this on every rebuild,
    // so freeing the buffer here would strand loop() — the original bug.
    void deinitAll() {
        if constexpr (platform::rmtTxChannels == 0) return;
        for (uint8_t i = 0; i < kMaxPins; i++) {
            if (rmt_[i].impl) platform::rmtWs2812Deinit(rmt_[i]);
        }
        inited_ = false;
    }
};

} // namespace mm
