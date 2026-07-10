#pragma once

#include "light/drivers/DriverBase.h"        // DriverBase, Correction
#include "light/drivers/LedDriverConfig.h"
#include "light/drivers/PinList.h"         // parsePinList / assignCounts (shared with LcdLedDriver)
#include "light/drivers/RmtSymbol.h"       // encodeWs2812Symbols (host-testable)
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>   // snprintf for the loopback status string
#include <cstring>  // std::strcmp in onUpdate / controlChangeTriggersBuildState

namespace mm {

/// Output driver: WS2812B-class addressable LEDs over the ESP32 [RMT (Remote Control
/// Transceiver)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/rmt.html)
/// peripheral — one GPIO and one RMT TX channel per strand, fed consecutive slices of the source
/// buffer (8-bit, GRB). The default LED driver for classic-ESP32 and S3 board entries, and the
/// readable EXAMPLE future LED drivers copy: a sibling of NetworkSendDriver (same DriverBase hooks,
/// same per-light `correction_->apply()` guard, same once-allocated owned buffer sized off the hot
/// path); only the emit differs — this fuses the correction + WS2812 symbol-encode into one pass
/// (the encode is `RmtSymbol.h`, host-tested) then hands per-pin slices to the platform.
///
/// **Wire contract — [WS2812B](https://cdn-shop.adafruit.com/datasheets/WS2812B.pdf):** 1-wire NRZ
/// at 800 kHz, no clock line. Each bit is a 1.25 µs cell that starts HIGH then drops LOW; the HIGH
/// duration encodes the bit (`0` = 350 ns high, `1` = 700 ns high), MSB-first per byte. Channel
/// order (GRB, GRBW, …) is applied by `Correction` before the encode, so the encoder is
/// order-agnostic. Frames latch on ≥300 µs idle-LOW. Timings live in `LedDriverConfig`, converted to
/// RMT ticks from the granted resolution (~40 MHz), never hard-coded to one clock. The platform owns
/// only the peripheral (`platform::rmtWs2812*`); on a chip without RMT TX channels those calls are
/// inert stubs, so it compiles everywhere. The peripheral half uses the modern RMT driver (ESP-IDF
/// 5.x "RMT v2": `rmt_new_tx_channel` / a copy encoder / `rmt_transmit`), not the legacy
/// channel-numbered API — not a preference: the legacy driver was removed entirely in ESP-IDF v6
/// (the build IDF), so v2 is the only API that exists. On chips whose RMT has a DMA backend
/// (`SOC_RMT_SUPPORT_DMA` — the P4; the classic ESP32 has none) the whole-frame loopback capture
/// uses it.
///
/// **Flicker on LEDs that should be off** is almost always a data-line signal-integrity problem
/// (3.3 V drive into a 5 V strip), not firmware — the "LED signal integrity" use-case guide has the
/// confirm-firmware-innocent playbook (`loopbackFrame`, the TX-power sweep) and the electrical fixes.
/// @card RmtLedDriver.png
class RmtLedDriver : public DriverBase {
public:
    /// Hard cap on the pin arrays: the largest RMT TX group of any supported chip (8 on
    /// classic ESP32; the S3 has 4 — enforced per target via maxPinsForTarget()). A fixed
    /// array bounded by a hardware constant, not a dynamic list: the bound can't grow at runtime.
    static constexpr uint8_t kMaxPins = 8;

    /// Comma-separated GPIO list, one RMT TX channel per pin ("18,17,16"). Text control so one
    /// field holds N pins — per-output (pin, count) rows are the WLED LED-settings pattern. The
    /// peripheral validates each pin at init; a parse error or failing pin lands in the status
    /// field and the driver idles. 24 bytes fit kMaxPins 2-digit GPIOs plus separators. Defaults
    /// to UNSET: the strand is user-soldered to whatever GPIO the user wired, so a hard-coded pin
    /// would be a guess that could drive a pin committed elsewhere — empty until set, idle
    /// meanwhile (the "default only when it cannot do harm" rule; see lessons.md). Bench pin "18".
    char pins[24] = "";

    /// Comma-separated lights-per-pin ("100,100,50"), matched to `pins` by position — each pin
    /// takes the next consecutive slice of the source buffer, in list order (pin 1 = `[0,n₁)`,
    /// pin 2 = `[n₁,n₁+n₂)`, …). May be empty or shorter than `pins`: the unassigned remainder
    /// splits evenly over the remaining pins (last takes the rounding remainder), so the empty
    /// default splits the whole buffer evenly. Each pin is capped at a WS2812 per-pin ceiling
    /// (2048 lights — a 1-wire line clocks ~30 µs/light, so 2048 is already ~16 FPS): a pin over
    /// the ceiling is clamped (output stays lit) with a Warning status, guarding the common
    /// misconfig of a whole grid on one pin. Use the start/count window to drive fewer lights,
    /// not this safety cap.
    char ledsPerPin[48] = "";

    /// On-device loopback self-test — RMT is a transceiver, so the driver verifies its own output
    /// on real silicon (replaces the old standalone test firmware). Tick to run a one-shot RMT
    /// TX→RX round-trip: jumper the first pin (TX) to `loopbackRxPin`, it transmits a known WS2812
    /// pattern, captures it back, decodes, compares → `loopback PASS` / `FAIL: …` / `jumper not
    /// detected` in the status field. A persistent on/off mode (see onUpdate): while on the test
    /// re-runs on every relevant change; turning it off clears the verdict. Hardware lives in
    /// `platform::rmtWs2812Loopback*`.
    bool     loopbackTest = false;  // checkbox: on = run + keep re-running on change
    /// Optional TX override for the test: when set (>= 0), the loopback transmits on THIS pin in
    /// place of pins[0], so the test can run on a dedicated jumper without re-typing the
    /// operational `pins`. Falls back to pins[0] when unset (-1). Test-only — normal output uses
    /// `pins`. int8_t + addPin (not uint16): single-GPIO controls use the standard Pin control,
    /// and -1 = unset lets GPIO 0 be a valid loopback pin (0-as-unset wouldn't).
    int8_t   loopbackTxPin = -1;
    /// Jumper this to the TX pin for the test (unset = -1 by default; bench used pin 5).
    int8_t   loopbackRxPin = -1;

    /// Whole-frame stress variant: instead of a 24-bit burst, transmit a real frame the size of
    /// the first pin's slice, back to back, and bit-verify the WHOLE capture. This is the one that
    /// catches frame-rate corruption and RF interference on the data line (the flicker class of
    /// bug) — a 24-bit burst passes through a wire that mangles a sustained frame. Shown only in
    /// test mode; the status names the first corrupted light on failure. On the classic ESP32,
    /// which has no RMT DMA, the capture is capped to one channel's worth of symbols (~2 RGB lights)
    /// and still clocked back to back; the S3/P4 capture the full frame via DMA.
    bool     loopbackFrame = false;

    // 40 MHz RMT tick clock = 25 ns/tick: t0h 350ns→14, t1h 700ns→28, period
    // 1250ns→50 ticks. The encoder converts ns→ticks via the granted resolution,
    // so this is the requested clock, not a hard-coded tick count.
    static constexpr uint32_t kResolutionHz = 40'000'000;

    // The pin/count list parsing (parsePinList / assignCounts) lives in
    // PinList.h, shared with LcdLedDriver — both drivers slice the source
    // buffer from the same two text controls.

    /// Bind the driver's controls: the window (start/count), the `pins` and
    /// `ledsPerPin` text lists, and the loopback self-test controls (the TX/RX pin
    /// overrides and frame-stress flag are always bound but shown only in test mode).
    void onBuildControls() override {
        addWindowControls();   // start / count — the slice of the shared buffer this driver outputs
        controls_.addText("pins", pins, sizeof(pins));
        controls_.addText("ledsPerPin", ledsPerPin, sizeof(ledsPerPin));
        controls_.addReadOnly("backend", backend_, sizeof(backend_));
        controls_.addReadOnly("frameStats", frameStats_, sizeof(frameStats_));
        controls_.addBool("loopbackTest", loopbackTest);
        // loopbackTxPin / loopbackRxPin are always bound (so persistence can load
        // them any time) but only shown while the test mode is on — same always-
        // add-then-setHidden shape NetworkModule uses for its static-IP fields. The
        // rebuild after every control change (HttpServerModule) re-runs this and
        // flips the flag. txPin is the optional override: -1 (unset) = transmit on
        // pins[0]; a value >= 0 is an explicit GPIO override (0 is now a valid pin).
        controls_.addPin("loopbackTxPin", loopbackTxPin);
        controls_.setHidden(controls_.count() - 1, !loopbackTest);
        controls_.addPin("loopbackRxPin", loopbackRxPin);
        controls_.setHidden(controls_.count() - 1, !loopbackTest);
        controls_.addBool("loopbackFrame", loopbackFrame);
        controls_.setHidden(controls_.count() - 1, !loopbackTest);
    }

    /// Changing the pin list or the per-pin counts re-parses and re-inits the RMT
    /// channels (live, not reboot-to-apply), so the pipeline-wide onBuildState
    /// sweep runs and parseConfig()/reinit() pick up the new lists.
    bool controlChangeTriggersBuildState(const char* name) const override {
        return std::strcmp(name, "pins") == 0 || std::strcmp(name, "ledsPerPin") == 0
            || isWindowControl(name);
    }

    /// React to a control change (runs off the render loop, in the HTTP/API
    /// handler context — a blocking self-test here is fine). loopbackTest is a
    /// persistent on/off mode. While it's ON, the test (re-)runs on every relevant
    /// change — turning it on, OR editing pins / loopbackRxPin — so the pins can be
    /// set in any order and the result always reflects the current pins. Turning it
    /// OFF clears the result.
    void onUpdate(const char* name) override {
        const bool isTestControl = std::strcmp(name, "loopbackTest") == 0;
        const bool isPinControl  = std::strcmp(name, "pins") == 0
                                || std::strcmp(name, "loopbackTxPin") == 0
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
            // A `pins` edit changes pinList_/pinCount_, but onUpdate runs BEFORE the
            // onBuildState() sweep re-parses (and loopbackRxPin/loopbackFrame don't
            // trigger that sweep at all), so refresh here before testing — otherwise
            // the self-test would transmit on the STALE pinList_[0] and show a verdict
            // for the previous pin. Mirrors ParallelLedDriver::onUpdate.
            if (std::strcmp(name, "pins") == 0) { parseConfig(); reinit(); }
            runLoopbackSelfTest();
        }
    }

    /// Parse the config and (re)init the RMT channels. Lifecycle has two
    /// deliberately-separate concerns, so the buffer half stays host-testable and a
    /// hardware-only guard can never strand it:
    ///   - SYMBOL BUFFER (plain heap): resizeSymbols() / freeSymbols(), run on
    ///     every platform.
    ///   - RMT CHANNELS (hardware): reinit() / deinitAll(), RMT-targets-only
    ///     (if constexpr).
    /// The original bug put the buffer free inside the hardware deinit(), which
    /// reinit() (a rebuild) calls — so a rebuild freed the buffer loop() needs.
    /// Keeping the two apart makes that mistake impossible here and lets the host
    /// unit test (unit_RmtLedDriver_lifecycle.cpp) pin it.
    void setup() override { parseConfig(); reinit(); }
    /// Release the RMT channels and free the symbol buffer, then clear the shared
    /// fail/config-error state (DriverBase::teardown()).
    void teardown() override {
        deinitAll();
        freeSymbols();
        DriverBase::teardown();   // clears failBuf_ + configErr_
    }

    /// Topology (light count / channels) or the pins/ledsPerPin controls changed —
    /// re-parse the lists, resize the symbol buffer, and (re)init the channels off
    /// the hot path. loop() never allocates.
    void onBuildState() override {
        parseConfig();
        resizeSymbols();
        reinit();
        MoonModule::onBuildState();
    }

    /// Preset toggle (RGB↔RGBW) changes outChannels without a structural rebuild —
    /// the per-pin symbol offsets scale with outChannels, so re-derive them too.
    void onCorrectionChanged() override { parseConfig(); resizeSymbols(); }

    /// Point the driver at the source frame buffer; re-parse (counts derive from
    /// its light count) and resize the symbol buffer to match.
    void setSourceBuffer(Buffer* buf) override {
        sourceBuffer_ = buf;
        parseConfig();      // counts derive from the buffer's light count
        resizeSymbols();
    }
    /// Point the driver at the shared correction; re-parse (per-pin symbol offsets
    /// derive from outChannels) and resize the symbol buffer to match.
    void setCorrection(const Correction* c) override {
        correction_ = c;
        parseConfig();      // offsets derive from outChannels
        resizeSymbols();
    }

    /// Per-tick output: fuse the correction and WS2812 symbol-encode in one pass
    /// over this driver's window, then start every pin's transmit before waiting on
    /// any, so the tick costs the longest strand rather than the sum. Inert off RMT
    /// chips and idle until inited with a source buffer + correction.
    void loop() override {
        if constexpr (platform::rmtTxChannels == 0) return;  // inert off RMT chips
        if (!inited_ || !sourceBuffer_ || !sourceBuffer_->data() || !correction_) return;

        // Encode only the lights the pins actually transmit (Σ pinCounts_), NOT the whole source
        // buffer: a strand config of e.g. 64 leds/pin on a 16K-light grid drives 64, so encoding
        // all 16384 would burn ~100× the work the output needs (the rest is never clocked out).
        // Bounded by the buffer too, in case config outruns the current frame.
        // Encode within this driver's window only. winLen_ is the slice length;
        // txLightCount_ (Σ pinCounts_) is what the pins clock out — n is the min,
        // so a window smaller than the configured pin total never reads past it.
        const nrOfLightsType n = txLightCount_ < winLen_ ? txLightCount_ : winLen_;
        const uint8_t outCh = correction_->outChannels;
        // Same defensive guard ArtNet uses: skip rather than overrun if the
        // symbol buffer is stale (e.g. correction swapped without a resize).
        if (n == 0 || outCh == 0 || pinCount_ == 0
            || !symbols_ || symbolCap_ < symbolsFor(n, outCh)) return;

        for (uint8_t i = 0; i < pinCount_; i++) {
            if (platform::rmtWs2812Busy(rmt_[i])) {
                recordSkippedFrame();
                return;
            }
        }

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
            // Read the windowed light: this driver's slice starts at winStart_.
            correction_->apply(src + (winStart_ + i) * srcCh, wire);
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
        // Transmit only up to the n lights actually encoded this frame: pins are laid out
        // contiguously from light 0, so pin i covers lights [pinStart, pinStart+pinCounts_[i]).
        // Normally Σ pinCounts_ == n, but if the buffer shrank since the last parseConfig (a grid
        // resize lands a tick before the config re-parse) n can be below Σ pinCounts_ — cap each
        // pin at the encoded boundary so it never clocks out stale symbols past what we wrote.
        const size_t wordsPerLight = static_cast<size_t>(outCh) * 8;
        for (uint8_t i = 0; i < pinCount_; i++) {
            const nrOfLightsType pinStart = static_cast<nrOfLightsType>(pinOffsets_[i] / wordsPerLight);
            if (pinStart >= n) break;  // contiguous: this pin and all later ones are past the encoded lights
            const nrOfLightsType pinLights =
                (pinStart + pinCounts_[i] > n) ? static_cast<nrOfLightsType>(n - pinStart) : pinCounts_[i];
            if (pinLights == 0) continue;
            if (!platform::rmtWs2812Transmit(rmt_[i], symbols_ + pinOffsets_[i],
                                             static_cast<size_t>(pinLights) * wordsPerLight)) {
                recordSkippedFrame();
            }
        }
    }

    void loop1s() override {
        const uint32_t skipped = skippedFrames_;
        skippedFrames_ = 0;
        std::snprintf(frameStats_, sizeof(frameStats_), "skip %lu/s total %lu",
                      static_cast<unsigned long>(skipped),
                      static_cast<unsigned long>(skippedTotal_));
        if (!configErr_ && !configWarn_) {
            if (skipped > 0) {
                std::snprintf(skipStatus_, sizeof(skipStatus_), "RMT skipped %lu frames/s",
                              static_cast<unsigned long>(skipped));
                setStatus(skipStatus_, Severity::Warning);
            } else if (status() == skipStatus_) {
                clearStatus();
            }
        }
        MoonModule::loop1s();
    }

    /// Test-only accessors. symbolBuffer/symbolCapacity mirror ArtNet's
    /// correctedBuffer() and let unit tests pin the buffer-lifecycle invariants a
    /// hardware bug already taught us; pinCount/pinLightCount/pinSymbolOffsetWords
    /// pin the multi-pin slice arithmetic (unit_RmtLedDriver_pins.cpp). Not part
    /// of any runtime API.
    const uint32_t* symbolBuffer() const { return symbols_; }
    /// Words allocated in the symbol buffer. Test-only.
    size_t symbolCapacity() const { return symbolCap_; }
    /// Number of parsed output pins (0 = idle). Test-only.
    uint8_t pinCount() const { return pinCount_; }
    /// Lights on pin `i` (0 if out of range). Test-only.
    nrOfLightsType pinLightCount(uint8_t i) const { return i < pinCount_ ? pinCounts_[i] : 0; }
    /// Word offset of pin `i`'s slice in the symbol buffer (0 if out of range). Test-only.
    size_t pinSymbolOffsetWords(uint8_t i) const { return i < pinCount_ ? pinOffsets_[i] : 0; }

private:
    // Source frame + shared correction — each physical driver owns these (the
    // contract shares Correction::apply(), not the member storage); same shape
    // as NetworkSendDriver.
    Buffer* sourceBuffer_ = nullptr;
    const Correction* correction_ = nullptr;

    LedDriverConfig cfg_;
    char backend_[16] = "none";
    platform::RmtWs2812Handle rmt_[kMaxPins];
    uint16_t       pinList_[kMaxPins] = {};    // parsed pins, list order
    nrOfLightsType pinCounts_[kMaxPins] = {};  // lights per pin (slice lengths)
    size_t         pinOffsets_[kMaxPins] = {}; // slice start in symbols_, words
    nrOfLightsType txLightCount_ = 0;          // Σ pinCounts_ — lights actually transmitted/encoded
    nrOfLightsType winStart_ = 0;              // first source-buffer light this driver reads (the window)
    nrOfLightsType winLen_ = 0;                // window length (lights), clamped to the buffer
    uint8_t pinCount_ = 0;                     // 0 = idle (parse error / no pins)
    bool inited_ = false;                      // all-or-nothing across the pins
    uint32_t* symbols_ = nullptr;   // owned; one word per WS2812 data bit
    size_t symbolCap_ = 0;          // words allocated
    uint32_t skippedFrames_ = 0;
    uint32_t skippedTotal_ = 0;
    char frameStats_[32] = "skip 0/s total 0";
    char skipStatus_[40] = {};

    // The parse-error literal currently shown in the status slot (nullptr when
    // configErr_, failBuf_, kFailBufLen and the clearConfigErr/clearFailBuf/
    // failBufEnsure/setConfigErr helpers live on DriverBase (shared verbatim with
    // the LCD and Parlio drivers). The on-demand FAIL string (failBuf_) is only
    // formatted when a loopback or channel init FAILs; PASS/jumper/unsupported use
    // flash literals.

    void setBackend(const char* value) {
        std::snprintf(backend_, sizeof(backend_), "%s", value ? value : "none");
    }

    void recordSkippedFrame() {
        skippedFrames_++;
        skippedTotal_++;
    }

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
        const char* warn = nullptr;
        const char* err = parsePinList(pins, pinList_, maxPinsForTarget(), n);
        if (!err) {
            // Distribute over the driver's window slice, not the whole buffer, so
            // ledsPerPin's "rest" only fills this driver's [start, start+count).
            // assignCounts clamps each pin to kMaxWs2812LedsPerPin (the driver drives
            // that many rather than choking a whole grid onto one WS2812 line).
            const nrOfLightsType bufN = sourceBuffer_ ? sourceBuffer_->count() : 0;
            windowSlice(bufN, winStart_, winLen_);
            err = assignCounts(ledsPerPin, n, winLen_, pinCounts_, kMaxWs2812LedsPerPin, &warn);
        }
        if (err) {
            setConfigErr(err);
            return false;
        }
        pinCount_ = n;
        const uint8_t outCh = correction_ ? correction_->outChannels : 0;
        size_t off = 0;
        txLightCount_ = 0;
        for (uint8_t i = 0; i < pinCount_; i++) {
            pinOffsets_[i] = off;
            off += static_cast<size_t>(pinCounts_[i]) * outCh * 8;
            txLightCount_ = static_cast<nrOfLightsType>(txLightCount_ + pinCounts_[i]);
        }
        clearConfigErr();
        // assignCounts sets `warn` when it clamped a pin to the WS2812 ceiling — the output
        // still runs (the first 2048/pin), so it's a Warning, not an idling error. Passing it
        // (or null when nothing clamped) to setConfigWarn tracks the live state and retracts a
        // stale warning once the user drops back under the ceiling.
        setConfigWarn(warn);
        return true;
    }

    // --- symbol buffer (plain heap; runs on every platform) ---

    // (Re)allocate the symbol buffer for the current source + correction. Off the
    // hot path. Grows only — keeps a big-enough existing allocation.
    void resizeSymbols() {
        if (!sourceBuffer_ || !correction_) return;
        // Size for this driver's window slice, not the whole source buffer — an
        // onboard-LED slice of 1 reserves 1 light's worth of symbols, not the full
        // grid's. Derive the window length directly (windowSlice is independent of
        // the pin parse, so the buffer sizes correctly even before pins are set).
        nrOfLightsType winStart, n;
        windowSlice(sourceBuffer_->count(), winStart, n);
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
        // The RX pin must be set: the loopback captures TX→RX over a jumper, so an
        // unset rxPin (-1) has nothing to listen on. Guard before the uint8_t cast
        // below, which would otherwise turn -1 into GPIO 255 (a bogus pin).
        if (loopbackRxPin < 0) {
            clearFailBuf();
            setStatus("loopback: set loopbackRxPin (jumper it to the TX pin)", Severity::Status);
            return;
        }
        // The test reconfigures the first data pin as TX, so release ALL our TX
        // channels first — this also guarantees the test's RX channel can always
        // allocate RMT memory, even with every TX channel otherwise claimed;
        // reinit() restores them after.
        deinitAll();
        // TX override: when loopbackTxPin is set, transmit on it instead of the
        // first data pin, so the bench loopback runs on a dedicated jumper without
        // re-typing `pins`. Falls back to pins[0] when unset.
        const uint8_t txPin = loopbackTxPin >= 0
            ? static_cast<uint8_t>(loopbackTxPin)
            : static_cast<uint8_t>(pinList_[0]);
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
            failBufEnsure();
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
            if (failBufEnsure()) {
                std::snprintf(failBuf_, kFailBufLen, "RMT init failed on pin %u",
                              static_cast<unsigned>(pinList_[i]));
                setStatus(failBuf_, Severity::Error);
            } else {
                setStatus(kInitFailMsg, Severity::Error);
            }
            return;
        }
        inited_ = true;
        setBackend(platform::rmtWs2812Backend(rmt_[0]));
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
        setBackend("none");
        inited_ = false;
    }
};

} // namespace mm
