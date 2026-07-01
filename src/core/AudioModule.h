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
#include "core/math8.h"      // beatsin8 / sin8 — the simulated-audio oscillators
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

    // Unlike a zero-cost diagnostic peripheral, this module pays a
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
    // Simulated audio: fill the frame with a synthesized signal so audio-reactive effects are demoable
    // (and testable) without a mic or music. Two patterns:
    //   `music` — a plausible song: multi-sine bands + a swelling volume + a periodic beat + a
    //             sweeping peak. Nice for demos (bars dance, VU breathes, peaks move).
    //   `sweep` — a single band lit, marching bass→treble on a timer, with the peak frequency and a
    //             steady volume tracking it. Deterministic — the clean test pattern to check that each
    //             effect responds across the whole spectrum.
    // A real mic always wins: when `mode` is a fill-in mode it only runs while there's no real signal.
    uint8_t  simulate = 0;       // 0 = off, 1 = music (fill silence), 2 = sweep (fill silence),
                                 // 3 = music (always), 4 = sweep (always)

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
        static constexpr const char* kSimulateOptions[] = {
            "off", "music (silence)", "sweep (silence)", "music (always)", "sweep (always)"};
        controls_.addSelect("simulate", simulate, kSimulateOptions, 5);
        // Read-only live read-outs (formatted in loop1s). Derived every second,
        // nothing to persist, so ReadOnly (the display-only type) not a flipped
        // Text — same idiom as SystemModule's uptime/fps.
        // "level RMS" = the RMS loudness; the DISPLAYED number is its peak over the 1-second window
        // (loop1s publishes levelPeak_, the max of the per-block RMS level, then resets it), so a
        // beat that lands between samples still registers. The live frame_.level the LEDs use is the
        // instantaneous RMS, recomputed every audio block — this read-out is the human-readable
        // summary of it, not a separate statistic.
        controls_.addReadOnly("level RMS", levelStr_, sizeof(levelStr_));
        controls_.addReadOnly("peakHz", peakStr_, sizeof(peakStr_));
        MoonModule::onBuildControls();
    }

    // A pin or rate change rebuilds the I2S channel.
    bool controlChangeTriggersBuildState(const char* name) const override {
        return std::strcmp(name, "wsPin") == 0 || std::strcmp(name, "sdPin") == 0
            || std::strcmp(name, "sckPin") == 0 || std::strcmp(name, "sampleRate") == 0;
    }

    void onBuildState() override { reinit(); MoonModule::onBuildState(); }
    void setup() override {
        if (active_ == nullptr) active_ = this;   // first live mic wins; a 2nd mic is captured but not read
        reinit();
    }
    void teardown() override {
        deinit();
        if (active_ == this) active_ = nullptr;   // vacate; a surviving module re-elects itself in loop()
    }

    // The latest analysed frame — what effects read. Always valid (zeroed until
    // the first successful read), so a consumer never dereferences null and a
    // mic-less build just sees silence.
    const AudioFrame* audioFrame() const { return &frame_; }

    // Process-wide accessor for the consumers (audio effects). There is one mic,
    // and an effect can be added/removed via the UI at any time, so it can't rely
    // on a boot-time setter — it asks here. Returns the live mic's frame while one
    // exists, else a static all-silent frame, so an effect added before/without a
    // mic still reads valid silence instead of null. The FIRST live module claims the seat in setup(),
    // vacates it in teardown(), and any running module re-claims an empty seat in loop() — so a device
    // with two mics reads the first consistently, and removing the active one lets a survivor take over.
    // Add/remove in any order leaves a coherent answer (the robustness rule).
    static const AudioFrame* latestFrame() {
        static const AudioFrame kSilence{};
        return active_ ? &active_->frame_ : &kSilence;
    }

    void loop() override {
        // Self-elect as the active mic if the seat is empty. setup() gives it to the first live module
        // and teardown() vacates it, but removing the active module while a second one is still running
        // would otherwise leave the seat empty (effects go silent). A running module re-claiming an
        // empty seat here keeps latestFrame() pointing at a live frame for ANY add/remove order — the
        // survivor takes over on its next tick (robustness).
        //
        if (active_ == nullptr) active_ = this;

        // Simulated audio (see the `simulate` control): 0=off, 1=music-on-silence, 2=sweep-on-silence,
        // 3=music-always, 4=sweep-always. Real mic input always wins in the "on-silence" modes — the
        // mic path below resets realQuietMs_ whenever a block carries signal, and synthesizeFrame()
        // no-ops while that timer says the mic is live. Off I2S (desktop / mic-less) or on a bad init
        // there's no mic, so the sim is the only possible source: run it and return.
        const bool simSweep = (simulate == 2 || simulate == 4);
        const bool simAlways = (simulate >= 3);
        if constexpr (!platform::hasI2sMic) {
            if (simulate != 0) synthesizeFrame(simSweep);   // the only possible source off I2S
            return;
        } else {
            if (!inited_) { if (simulate != 0) synthesizeFrame(simSweep); return; }
            if (simAlways) { synthesizeFrame(simSweep); return; }   // forced: skip the mic entirely
        }

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

        // Smoothed level: a one-pole exponential moving average of the raw `level`, so effects that
        // want a calm, breathing VU (rather than the raw value's snap-to-transient) read a value that
        // lags and rounds off sudden changes. 3/4 old + 1/4 new is the textbook light smoothing —
        // fast enough to follow the music, slow enough to hide per-block jitter. Integer-only, one
        // block behind, off the per-light path. (WLED's `volume`/`volumeSmth` to our raw `level`.)
        frame_.levelSmoothed = static_cast<uint16_t>((frame_.levelSmoothed * 3 + frame_.level) / 4);

        // Spectrum: window -> FFT -> 16 log bands, same floor/gain mapping.
        uint16_t peakHz = 0, peakMag = 0;
        applyWindow(samples_, kBlock, windowed_);
        platform::audioFft(windowed_, kBlock, mag_);
        magnitudesToBands(mag_, kMag, sampleRate(), floor, gain,
                          frame_.bands, peakHz, peakMag);

        // Peak frequency: the exact-Hz FFT bin, held when there's no real signal so
        // it doesn't wander in silence.
        if (peakMag > 8) { frame_.peakHz = peakHz; frame_.peakMag = peakMag; }

        // Track the PEAK level across the 1 s display window. frame_.level is recomputed every
        // ~23 ms audio block, but the UI string is snapshotted only once a second — sampling the
        // instantaneous value lands in the gaps between beats and reads 0 even while the LEDs (driven
        // live every render tick) move with the music. The window peak is the representative reading.
        // Display-only — the live frame_.level the effects/LEDs use is untouched.
        if (frame_.level > levelPeak_) levelPeak_ = frame_.level;

        // Auto fill-in (simulate 1/2): if the mic has been quiet for a bit, synthesize so the effects
        // still have something to react to. `level` this block re-arms the "real audio" grace period;
        // once it lapses, the sim takes over and yields again the instant real sound returns.
        if (simulate == 1 || simulate == 2) {
            if (frame_.level > kSimRealThreshold) realBlocks_ = kSimRealGraceBlocks;
            else if (realBlocks_ > 0) realBlocks_--;
            if (realBlocks_ == 0) synthesizeFrame(simulate == 2);
        }
    }

    // Fill frame_ with a synthesized signal. sweep=false → a plausible "song" (each band its own
    // oscillator, a swelling volume, a periodic beat, a drifting peak); sweep=true → one band lit
    // marching bass→treble on a timer (the deterministic test pattern). All integer LUT math (sin8),
    // once per tick, off the per-light path. Also runs the same levelSmoothed EMA the mic path does.
    void synthesizeFrame(bool sweep) {
        const uint32_t t = platform::millis();
        if (sweep) {
            // One band lit at a time, stepping bass→treble every ~250 ms and wrapping. The lit band
            // ramps up-and-down (triangle) so it's not a hard on/off, and the peak frequency + volume
            // track the swept band so frequency-mapped and volume effects follow it too.
            const uint8_t pos = static_cast<uint8_t>((t / 250u) % 16u);
            const uint8_t env = triwave8(static_cast<uint8_t>((t % 250u) * 255u / 250u));  // 0..255 within a step
            for (uint8_t b = 0; b < 16; b++) frame_.bands[b] = (b == pos) ? env : 0;
            frame_.level = env;
            frame_.peakHz = static_cast<uint16_t>(80 + pos * 700);   // bass→~10.6 kHz across the 16 steps
            frame_.peakMag = env;
        } else {
            // Musical "song": each band an independent sine at its own rate/phase (bass slow, treble
            // fast), a slow overall volume swell, and a periodic beat pulse that briefly lifts the low
            // bands + volume so beat-reactive effects fire. A drifting peak sweeps the dominant tone.
            const uint8_t beat = (t % 600u < 90u) ? static_cast<uint8_t>(triwave8(static_cast<uint8_t>((t % 600u) * 255u / 90u))) : 0;
            uint16_t sum = 0;
            for (uint8_t b = 0; b < 16; b++) {
                // Per-band oscillator: rate rises with b (treble flickers faster), phase spread by b.
                const uint8_t rate = static_cast<uint8_t>(1 + b);                   // BPM-ish multiplier
                const uint8_t osc = sin8(static_cast<uint8_t>(t * rate / 8u + b * 24u));
                uint16_t v = static_cast<uint16_t>((osc * 3u) / 4u);               // 0..191 base
                if (b < 4) v = static_cast<uint16_t>(v + beat / 2u);               // beat lifts the bass
                frame_.bands[b] = static_cast<uint8_t>(v > 255 ? 255 : v);
                sum = static_cast<uint16_t>(sum + frame_.bands[b]);
            }
            const uint8_t swell = sin8(static_cast<uint8_t>(t / 24u));             // slow volume breath
            uint16_t lvl = static_cast<uint16_t>(swell / 2u + sum / 32u + beat / 2u);
            frame_.level = lvl > 255 ? 255 : lvl;
            // Peak drifts across the spectrum so freq-mapped effects move.
            frame_.peakHz = static_cast<uint16_t>(80 + sin8(static_cast<uint8_t>(t / 40u)) * 40u);
            frame_.peakMag = frame_.level;
        }
        frame_.levelSmoothed = static_cast<uint16_t>((frame_.levelSmoothed * 3 + frame_.level) / 4);
        // Feed the same 1 s peak-hold the mic path uses, so the "level RMS" display tracks the
        // synthesized level instead of reading a stale 0 (the display is peak-over-window, not live).
        if (frame_.level > levelPeak_) levelPeak_ = static_cast<uint8_t>(frame_.level);
    }

    void loop1s() override {
        std::snprintf(levelStr_, sizeof(levelStr_), "%u", static_cast<unsigned>(levelPeak_));
        std::snprintf(peakStr_, sizeof(peakStr_), "%u Hz", static_cast<unsigned>(frame_.peakHz));
        levelPeak_ = 0;   // reset for the next window
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
    uint8_t levelPeak_ = 0;   // peak frame_.level across the current 1 s display window (UI only)

    // Auto fill-in (simulate 1/2): treat the mic as "live" for a grace window after any block above
    // the threshold, so brief gaps between beats don't flip to the sim. ~2 s of blocks (≈86/block·23ms).
    static constexpr uint16_t kSimRealThreshold  = 4;    // a block level above this counts as real sound
    static constexpr uint16_t kSimRealGraceBlocks = 86;  // ~2 s at ~23 ms/block before the sim takes over
    uint16_t realBlocks_ = 0;   // grace countdown: >0 = mic was recently live, hold off the sim

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
        // Bring up the I2S channel FIRST. On a codec board (an analog mic behind an
        // I2S codec, e.g. the S31's ES8311) the I2S peripheral drives MCLK, and the
        // codec won't even answer I2C until that clock runs — so I2S precedes the
        // codec config. The MCLK pin comes from the per-target codec config
        // (platform::audioCodecPins.mclk); −1 on a direct MEMS mic (self-clocked).
        const int16_t mclkPin = platform::audioCodecType == platform::CodecType::None
                              ? -1 : static_cast<int16_t>(platform::audioCodecPins.mclk);
        inited_ = platform::audioMicInit(mic_, static_cast<uint16_t>(wsPin),
                                         static_cast<uint16_t>(sdPin),
                                         static_cast<uint16_t>(sckPin), mclkPin, sampleRate());
        if (!inited_) {
            setStatus(kInitFailMsg, Severity::Error);
            return;
        }
        // Now configure the codec over I2C (MCLK is running). A no-op returning true
        // on direct-mic boards, so the call is uniform. The codec then streams its ADC
        // onto the I2S bus the read above drains.
        if (!platform::audioCodecInit(platform::audioCodecType, platform::audioCodecPins,
                                      sampleRate())) {
            deinit();   // tear the I2S channel back down — we couldn't bring the codec up
            setStatus("mic: codec init failed — check I2C wiring", Severity::Error);
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
        platform::audioCodecDeinit();   // releases the codec + its I2C bus (no-op if none)
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
