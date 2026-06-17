#pragma once

// AudioModule — acquires an audio source and publishes an AudioFrame (an overall
// sound level plus a 16-band frequency spectrum and the dominant peak). The frame
// is always available every render tick, but its analysed values are recomputed
// only when a full sample block has accumulated: a 512-sample block at 22 kHz
// takes ~23 ms to arrive (longer than one tick), so a tick that doesn't complete
// a block re-publishes the previous AudioFrame unchanged rather than re-analysing.
// Named for what it does (audio acquisition + analysis), not
// for one source: today the source is a digital I2S MEMS mic (e.g. INMP441, the
// only one wired); the analysis pipeline is source-independent and is meant to
// serve line-in / USB sources behind the platform read seam as they are added.
// It is the PRODUCER in the audio producer/consumer pair; audio-reactive effects
// (AudioVolumeEffect, AudioSpectrumEffect) are the consumers, wired the
// AudioFrame* in main.cpp.
//
// A SystemModule Peripheral child (the role already exists). Chip-agnostic:
// gated on platform::hasI2sMic, inert with a status note on targets without I2S
// and on desktop. The signal math is host-tested domain code (AudioLevel.h,
// AudioBands.h); this module owns the lifecycle, the controls, and the two
// platform seams (the I2S read and the FFT kernel).
//
// Hot path: fixed member scratch buffers (sample block + window + magnitudes),
// one float FFT per loop, no per-loop heap. The mic read is non-blocking enough
// for the tick; a bad init leaves the module idle (zeroed frame), never crashing.

#include "core/MoonModule.h"
#include "core/AudioFrame.h"
#include "core/AudioLevel.h"
#include "core/AudioBands.h"
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>   // snprintf for the read-out strings
#include <cstring>

namespace mm {

class AudioModule : public MoonModule {
public:
    // Block size = FFT size: a power of two. 512 samples at 22050 Hz is ~23 ms of
    // audio per frame — fine resolution (~43 Hz/bin) at a modest per-tick cost.
    static constexpr size_t kBlock = 512;
    static constexpr size_t kMag = kBlock / 2;   // real-FFT magnitude bins

    ModuleRole role() const override { return ModuleRole::Peripheral; }

    // Unlike a zero-cost diagnostic peripheral (BoardModule), this module pays a
    // real per-tick cost (the FFT) that IS the capability, not an optional extra,
    // so it must not run when the user turns it off. We therefore respect `enabled`
    // (the default): the Scheduler skips loop() entirely while disabled, so the FFT,
    // the level, and the read-outs all stop and the cost goes to zero. Enabled runs
    // the full pipeline; removing the module stops it the same way. The read-outs
    // hold their last value while disabled (no consumer reads a disabled module).
    // (respectsEnabled() defaults to true, so we don't override it.)

    // --- controls: three I2S pins, sample rate, the two conditioning knobs, and
    // two read-only read-outs. The pins default to UNSET (-1, the standard Pin-
    // control sentinel — so GPIO 0 stays a usable mic pin): the module is user-added
    // when a board has a mic, and stays idle (no I2S init) until the user enters the
    // real GPIOs, so adding it can't grab arbitrary pins or wedge a board with no
    // mic. The bench INMP441 wiring is WS=4 / SD=5 / SCK=6. ---
    int8_t wsPin = -1;           // word-select / LRCLK (-1 = unset)
    int8_t sdPin = -1;           // serial data in (-1 = unset)
    int8_t sckPin = -1;          // bit clock (-1 = unset)
    // Sample rate is a discrete choice (the standard audio rates), so it's a
    // dropdown over a fixed set, not a free number. sampleRateSel indexes
    // kSampleRates; sampleRate() resolves it to Hz. Default index 2 = 22050.
    uint8_t  sampleRateSel = 2;
    // Two knobs condition the spectrum + level:
    uint8_t  floor = 100;        // noise floor (dB display floor) — bands/level
                                 // below this read as silence. Raise to keep an
                                 // ambient room dark, lower for a quiet room.
    uint8_t  gain = 222;         // sensitivity — HIGHER = more (a narrower dB window
                                 // so a given sound fills more of the bar).

    static constexpr uint16_t kSampleRates[] = {8000, 16000, 22050, 44100};
    static constexpr uint8_t kSampleRateCount = 4;
    uint32_t sampleRate() const { return kSampleRates[sampleRateSel < kSampleRateCount
                                                       ? sampleRateSel : 2]; }

    void onBuildControls() override {
        controls_.addPin("wsPin", wsPin);
        controls_.addPin("sdPin", sdPin);
        controls_.addPin("sckPin", sckPin);
        static constexpr const char* kRateOptions[] = {"8000", "16000", "22050", "44100"};
        controls_.addSelect("sampleRate", sampleRateSel, kRateOptions, kSampleRateCount);
        controls_.addUint8("floor", floor, 0, 255);
        controls_.addUint8("gain", gain, 1, 255);
        // Read-only live read-outs (formatted in loop1s). Derived every second,
        // nothing to persist, so ReadOnly (the display-only type) not a flipped
        // Text — same idiom as SystemModule's uptime/fps.
        controls_.addReadOnly("level", levelStr_, sizeof(levelStr_));
        controls_.addReadOnly("peakHz", peakStr_, sizeof(peakStr_));
        MoonModule::onBuildControls();
    }

    // A pin or rate change rebuilds the I2S channel.
    bool controlChangeTriggersBuildState(const char* name) const override {
        return std::strcmp(name, "wsPin") == 0 || std::strcmp(name, "sdPin") == 0
            || std::strcmp(name, "sckPin") == 0 || std::strcmp(name, "sampleRate") == 0;
    }

    void onBuildState() override { reinit(); MoonModule::onBuildState(); }
    void setup() override { active_ = this; reinit(); }
    void teardown() override {
        deinit();
        if (active_ == this) active_ = nullptr;   // effects fall back to silence
    }

    // The latest analysed frame — what effects read. Always valid (zeroed until
    // the first successful read), so a consumer never dereferences null and a
    // mic-less build just sees silence.
    const AudioFrame* audioFrame() const { return &frame_; }

    // Process-wide accessor for the consumers (audio effects). There is one mic,
    // and an effect can be added/removed via the UI at any time, so it can't rely
    // on a boot-time setter — it asks here. Returns the live mic's frame while one
    // exists, else a static all-silent frame, so an effect added before/without a
    // mic still reads valid silence instead of null. The active instance registers
    // itself in setup() and clears the pointer in teardown(), so add/remove in any
    // order leaves a coherent answer (the robustness rule).
    static const AudioFrame* latestFrame() {
        static const AudioFrame kSilence{};
        return active_ ? &active_->frame_ : &kSilence;
    }

    void loop() override {
        if constexpr (!platform::hasI2sMic) return;   // inert off I2S targets
        if (!inited_) return;                          // bad init → idle (zero frame)

        // Drain whatever the DMA holds this tick (non-blocking) into the free tail
        // of the block accumulator. A full kBlock takes ~23 ms to arrive (longer
        // than one render tick), so each tick contributes a partial; we analyse
        // once the accumulator is full, then reset it.
        const size_t n = platform::audioMicRead(mic_, samples_ + filled_, kBlock - filled_);
        if (n == 0) return;                            // nothing ready this tick
        filled_ += n;
        if (filled_ < kBlock) return;                  // wait for a whole block
        filled_ = 0;                                   // consumed below; refill next

        // DC-blocker high-pass (~40 Hz): removes the constant offset + sub-bass
        // rumble before any analysis, so they can't leak into the low bands. The
        // filter is continuous across blocks (state in dc_).
        dc_.process(samples_, kBlock);

        // Level: overall loudness (RMS), independent of the FFT — it fluctuates
        // with how loud the room is. Uses a gentler floor than the bands (half),
        // so the VU keeps moving with volume instead of being gated hard like the
        // per-band display.
        computeLevel(samples_, kBlock, static_cast<uint8_t>(floor / 2), gain, frame_);

        // Spectrum: window -> FFT -> 16 log bands, same floor/gain mapping.
        uint16_t peakHz = 0, peakMag = 0;
        applyWindow(samples_, kBlock, windowed_);
        platform::audioFft(windowed_, kBlock, mag_);
        magnitudesToBands(mag_, kMag, sampleRate(), floor, gain,
                          frame_.bands, peakHz, peakMag);

        // Peak frequency: the exact-Hz FFT bin, held when there's no real signal so
        // it doesn't wander in silence.
        if (peakMag > 8) { frame_.peakHz = peakHz; frame_.peakMag = peakMag; }
    }

    void loop1s() override {
        std::snprintf(levelStr_, sizeof(levelStr_), "%u", static_cast<unsigned>(frame_.level));
        std::snprintf(peakStr_, sizeof(peakStr_), "%u Hz", static_cast<unsigned>(frame_.peakHz));
        MoonModule::loop1s();
    }

private:
    // The mic that latestFrame() hands to effects. One in practice; the last one
    // to setup() wins, teardown() clears it. inline so the header stays standalone.
    static inline AudioModule* active_ = nullptr;

    platform::AudioMicHandle mic_;
    bool inited_ = false;
    size_t filled_ = 0;         // samples accumulated toward the next full block
    DcBlocker dc_;              // ~40 Hz high-pass, continuous across blocks

    // Fixed hot-path scratch — sized once, never reallocated. ~6 KB total
    // (2 KB samples + 2 KB windowed + 1 KB magnitudes), DRAM-resident.
    int32_t samples_[kBlock] = {};
    float windowed_[kBlock] = {};
    float mag_[kMag] = {};

    AudioFrame frame_;

    char levelStr_[12] = {};
    char peakStr_[12] = {};

    static constexpr const char* kInitFailMsg = "mic init failed — check pins / rate";

    void reinit() {
        if constexpr (!platform::hasI2sMic) {
            setStatus("mic: no I2S on this platform", Severity::Warning);
            return;
        }
        deinit();
        // Any pin unset (-1, the default until the user wires a mic): stay idle,
        // don't attempt an I2S init — initialising I2S on unset pins is what hung a
        // mic-less board's boot. GPIO 0 IS a valid mic pin now (the sentinel is -1,
        // not 0), so the guard tests < 0, not == 0.
        if (wsPin < 0 || sdPin < 0 || sckPin < 0) {
            setStatus("mic: set wsPin / sdPin / sckPin", Severity::Status);
            return;
        }
        inited_ = platform::audioMicInit(mic_, static_cast<uint16_t>(wsPin),
                                         static_cast<uint16_t>(sdPin),
                                         static_cast<uint16_t>(sckPin), sampleRate());
        if (!inited_) {
            setStatus(kInitFailMsg, Severity::Error);
            return;
        }
        dc_.reset();   // start the high-pass clean for the new stream
        // The INMP441 emits ~250 ms of power-on settling garbage after the clock
        // starts. The read is non-blocking (hot-path rule), so we can't drain a
        // fixed sample count here at init — the DMA has barely filled. Instead the
        // settling samples flow through the first few loop() reads and the level /
        // bands self-correct within that quarter-second; no separate discard is
        // needed, and the frame stays valid (zeroed) until then.
        // Clear any prior status now the mic is live — not just kInitFailMsg, but
        // also the "set wsPin / sdPin / sckPin" note from the unset-pin path, which
        // would otherwise persist and mislead after the user fills the pins in.
        clearStatus();
    }

    void deinit() {
        if constexpr (!platform::hasI2sMic) return;
        if (inited_) platform::audioMicDeinit(mic_);
        inited_ = false;
        filled_ = 0;
        // Publish silence: latestFrame() hands frame_ to consumers whenever this is
        // the active mic, independent of inited_. Without this, a mic that worked
        // and then lost its bus (a failed reinit after a pin edit, or teardown)
        // would leave the last real frame frozen on the LEDs instead of going dark.
        frame_ = AudioFrame{};
    }
};

} // namespace mm
