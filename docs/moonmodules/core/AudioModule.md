# AudioModule

Acquires an audio source and publishes one analysed **AudioFrame** per render tick: an overall sound **level**, a 16-band frequency **spectrum**, and the **dominant peak**. It is the producer half of the audio-reactive pipeline; [AudioVolumeEffect](../light/effects/AudioVolumeEffect.md) and [AudioSpectrumEffect](../light/effects/AudioSpectrumEffect.md) are the consumers.

It is named for what it does, audio acquisition plus analysis, not for one source: today the source is a digital I²S MEMS microphone (the only one wired), and the same analysis pipeline is built to serve other sources (line-in, USB audio) behind the platform read seam as they are added. Most of the module is the analysis (DC-blocker, RMS level, windowed FFT, band mapping), which is source-independent.

A SystemModule **Peripheral** child, code-wired in `main.cpp`. Chip-agnostic: it is created only where the platform has an I²S peripheral (`platform::hasI2sMic`, true on every current ESP32, false on desktop). On a mic-less build it isn't created and the audio effects read a permanently-silent frame, so they simply stay dark.

## Hardware: INMP441-class digital mic

Built and tested against an **[INMP441](https://invensense.tdk.com/wp-content/uploads/2015/02/INMP441.pdf)** (a self-clocked I²S MEMS microphone): standard/Philips framing, 24-bit data left-justified in a 32-bit slot, mono. Three wires plus power:

| Control | Default | Pin |
|---|---|---|
| `wsPin` | 4 | word-select / LRCLK |
| `sdPin` | 5 | serial data out of the mic |
| `sckPin` | 6 | bit clock |

The part is self-clocked from the bit clock; there is no master-clock (MCLK) pin. It drives the one slot its L/R select pin chooses (tie L/R to GND for the left slot, VDD for the right); if `level` stays at the floor with sound present, the mic is filling the other slot; the fix is one wire, not firmware.

## How the AudioFrame is produced

Each `loop()`: read a block of samples → DC-blocker high-pass → compute the level → window + FFT → map to bands. The high-pass conditions the raw block once, up front, so both the level and the spectrum see the same cleaned signal.

- **DC-blocker high-pass** (`AudioLevel.h::DcBlocker`, host-tested): a one-pole/one-zero IIR high-pass (`y[n] = x[n] − x[n−1] + R·y[n−1]`, `R = 0.99`, ≈ 40 Hz corner at 22 kHz) applied to the whole block before any analysis. It removes the MEMS mic's large constant DC bias *and* sub-bass rumble below ~40 Hz (handling/wind/structural) that would otherwise leak into the lowest band. Its state carries across blocks (it's a continuous filter, not per-block), and it resets when the channel re-inits. This is distinct from, and runs before, the level path's own block-mean subtraction below.
- **Level** (`AudioLevel.h`, host-tested): subtract the block's DC mean (belt-and-braces after the high-pass), take the RMS, and map it through the log/dB window (`floor` / `gain`). It is the overall loudness, independent of the FFT: the VU value. (It uses a gentler floor than the bands so the meter keeps moving with volume rather than gating hard.)
- **Spectrum** (`AudioBands.h`, host-tested): apply a [Hann window](https://en.wikipedia.org/wiki/Hann_function) (the standard general-purpose FFT window, tapers the block edges so a tone doesn't smear across bins), run the FFT (`platform::audioFft`), then group the magnitude bins into 16 log-spaced bands (a plain geometric / equal-ratio bin split) and pick the loudest bin as the dominant peak (argmax). The peak is held when no real signal is present so it doesn't wander in silence.

Only the I²S read and the FFT kernel are platform code (`platform_esp32_i2s.cpp`: IDF's [`i2s_std`](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/i2s.html) driver + [esp-dsp](https://github.com/espressif/esp-dsp)'s float `dsps_fft2r_fc32`); everything else is plain domain math that runs in CI on the desktop's reference DFT.

The DSP choices are the textbook defaults on purpose: a **Hann** window, **RMS** for level, a **geometric** band split, **argmax** for the peak. There is deliberately **no per-frequency correction table**; the INMP441 is flat ±3 dB across the range that matters ([datasheet](https://invensense.tdk.com/wp-content/uploads/2015/02/INMP441.pdf)), so there is no mic-response error to compensate, and a hand-tuned correction curve would add complexity for nothing. The level is overall RMS loudness computed independently of the FFT, not derived from the bands; deriving it from the bands would stop it tracking volume.

## Controls

- `wsPin` / `sdPin` / `sckPin`: the three I²S GPIOs (see table above). Changing any re-creates the I²S channel.
- `sampleRate`: a dropdown over the standard rates (8000 / 16000 / 22050 / 44100 Hz), default **22050** (~11 kHz Nyquist covers the range that matters for light). Changing it re-creates the channel.
- `floor`, the noise floor: bands and level below this read as silence, so an ambient room stays dark. Raise it for a noisy room, lower it for a quiet one. Default 100.
- `gain`, sensitivity: higher = more (a narrower dB window, so a given sound fills more of the bar). Default 222.
- `level`: read-only live sound level (updates each second).
- `peakHz`: read-only dominant frequency (updates each second).

## Cross-domain wiring

AudioModule produces an `AudioFrame` (`src/light/AudioFrame.h`); the consuming effects reach the live frame through the static **`AudioModule::latestFrame()`**, not a boot-time setter, so an effect added through the UI at any time still finds the one mic, and with no mic it gets a static silent frame. The active module registers itself in `setup()` and clears that pointer in `teardown()`, so adding or removing the mic (or an effect) in any order always leaves a coherent answer.

The first ~250 ms after the I²S clock starts are power-on settling garbage; the read is non-blocking (the hot-path rule), so those samples flow through the first few `loop()` reads and the level/bands self-correct within that quarter-second; the frame stays valid (zeroed) until then.

## Prior art

Audio-reactive lighting is a long-standing idea in the LED-controller world (WLED-MM and MoonLight are the closest lineage). This is projectMM's own implementation, designed from the INMP441 datasheet and standard DSP rather than traced from any one project's code or band tables; the rationale for the specific DSP choices is in [How the AudioFrame is produced](#how-the-audioframe-is-produced) above. The history of *what was tried and removed* (notably a self-calibrating auto-gain / noise-floor conditioner, deferred as its own increment) lives in [decisions.md](../../history/decisions.md).

**Frank (softhack007).** [Frank](https://github.com/softhack007) is the main author of the WLED-MM audioreactive usermod, the most-used open-source audio-reactive LED implementation, and a direct ancestor of the ideas this module learns from. projectMM's product owner worked alongside Frank for years on WLED-SR / WLED-MM before starting MoonLight and then projectMM, so the collaboration goes back a long way. We don't trace his code (per the [*Industry standards, our own code*](../../../CLAUDE.md#principles) principle), but we study his thinking with real respect and credit it by name; the *Adaptive noise gate* section below is the first worked example: his concept, our analysis, written fresh against our own architecture.

## Adaptive noise gate: forward-looking

> **Present-tense exception (justified).** Module specs are otherwise present-tense ([CLAUDE.md](../../../CLAUDE.md)); this section is forward-looking by deliberate choice, so the design analysis stays with the module it extends. It describes a concept and our judgement of it, not shipped behaviour. The shipped audio path is everything above.

This concept comes from softhack007 (see [Prior art](#prior-art)), who granted permission to analyse it here. The proposal: replace the borrowed `squelch`/`noiseFloor` knob, described as "a WLED-SR workaround, not a real gate," with a proper adaptive noise gate. The rest of this section is our own assessment.

### The concept

- A **standard [noise gate](https://en.wikipedia.org/wiki/Noise_gate)**: below a threshold the signal is silenced (gate closed), above it the signal passes (gate open).
- **Asymmetric, bang-bang timing:** open **fast**, close **slow**. A bang-bang (hysteresis) controller avoids chatter at the threshold.
- A **new "detect silence" function** drives the gate. This is the explicitly *unfinished* part of the idea.
- **Leave the GEQ / FFT channels untouched.** The gate acts on the time-domain signal, not the bands. (A *per-band* noise threshold is noted as possibly also worth having.)
- The closing pre-condition should be **relative, not an absolute sample count**: a "percentage of average signal," not a fixed number.
- Optionally feed the gate **compressed samples** (sqrt or log) so the threshold behaves perceptually rather than linearly.

Five design constraints come with it, and they are the load-bearing part: (1) samples are **signed**, of **arbitrary magnitude**, and scaling to an effect range is AGC's job, not the gate's; (2) **every `abs()` must be justified** (a rectify discards sign/phase); (3) **prefer relative factors to absolute thresholds**, the one allowed absolute being that changes **< 2** counts are sampling noise; (4) **smooth before thresholding**; (5) **every filter adds delay, and total audio delay must stay < 30 ms.**

### Is this a good idea? Our verdict

**Yes, directionally, and it is squarely industry-standard.** A hysteresis noise gate with a fast-attack/slow-release envelope is the textbook design for exactly this problem (it is how studio gates, two-way-radio squelch, and voice-activity detectors all work), so adopting it moves us *toward* the recognisable solution and *away* from the borrowed `squelch` constant, which is the right direction under the [*Industry standards, our own code*](../../../CLAUDE.md#principles) principle. The relative-threshold insight (constraint 3) is the genuinely valuable core: a gate keyed to a *learned* floor self-calibrates to whatever mic or line source is connected, where an absolute squelch only ever suits one setup. So the idea is sound and worth doing.

**Two cautions keep it from being a drop-in.** First, **timing is tight and must be proven, not assumed.** A 512-sample block at 22050 Hz is already ~23 ms of buffering before analysis begins; that leaves under ~7 ms of the 30 ms budget for everything the gate adds. The block size, not the gate, is the dominant cost, so any smoothing the gate introduces must be cheap (one-pole) and the *open* path especially must not lengthen it. This is measurable on hardware and a hard gate on the design. Second, it overlaps work we have already scoped (the per-band floor, below), so the risk is building a parallel mechanism instead of one coherent one. Both push the same way: **decompose and adopt in steps, do not overhaul.**

### Does our per-band floor already cover part of this?

Partly, and that overlap is the key to sequencing. The backlogged [per-band noise-floor](../../backlog/backlog.md#sensors-and-audio-reactive-input) learns each band's idle baseline and subtracts it, so a *steady single-frequency* tone (our bench's ~258 Hz mains hum) gates to dark while the other bands stay live. The proposed time-domain gate answers a *different* question, "is there any sound at all," across the whole signal. They are complementary halves, not competitors: the per-band floor is the **frequency-domain** noise floor, the gate is the **time-domain** one. The per-band floor is also the smaller, already-planned step, so it is the natural first increment, and it is genuinely "part of this idea," not a thing the gate replaces.

### How to decompose it: cherry-pick, step by step

The whole proposal is more than one increment. Taken apart, most of its value lands early and cheaply, and the riskier parts can wait or be dropped:

1. **Per-band noise floor (already backlogged).** Ship this first. It is the frequency-domain half, the smallest change, and it kills the concrete hum we actually see. Independent of everything below.
2. **Relative thresholds, reusing the RMS we already compute.** The single most valuable idea here is "threshold against a learned floor, not an absolute number." `computeLevel` already produces a per-block **RMS**, which *is* an envelope estimate (and RMS is the one justified `abs()` under constraint 2: it is the energy measure, not a naive rectify). So a learned-floor follower over that RMS, with open/close as **factors** of it, is a small, host-testable addition that needs *no new DSP stage* and *no extra delay* (the RMS is on the critical path already). This is the cherry to pick.
3. **Hysteresis + asymmetric timing.** The fast-open/slow-close behaviour falls out of two time-constants on that follower plus a close-hold, not a separate state machine. Cheap to add once step 2 exists; this is where the < 30 ms budget gets measured for real.
4. **Optional, defer until proven needed:** log/dB-domain thresholds (our `magToByte` already does perceptual compression downstream, so the detector can stay linear at first and move to dB only if the linear factors prove twitchy), and a true soft gate (0..1 gain vs a hard 0/1).

Each step is its own commit, host-tested red-first, and leaves the system working; none requires touching `AudioBands.h` or the effect consumers. Steps 1–2 deliver most of the benefit (a self-calibrating floor in both domains) with almost no timing cost; 3–4 are polish to layer on only if the bench says they earn their place.

**What it eventually retires:** the `floor` knob's role as a hard squelch. `floor` would become the *display* noise-floor only (the dB-window bottom in `magToByte`), while the learned gate decides "is there sound." That is a clean subtraction, but it is the *end* of the path, not the first step. Tracked under [backlog § audio follow-ups](../../backlog/backlog.md#sensors-and-audio-reactive-input).

## Tests

- **Level (CI, host):** `test/unit/light/unit_AudioLevel.cpp`: silence → 0, pure DC → 0 (offset stripped), a louder sine reads higher, DC bias doesn't change a sine's level, a high `floor` gates a modest signal to 0, higher `gain` reads higher, null/empty input is silence.
- **Spectrum (CI, host):** `test/unit/light/unit_AudioBands.cpp`: a tone lands in the right band and the reported `peakHz` tracks it (end-to-end through the desktop reference DFT), energy concentrates rather than smears, degenerate input never crashes.
- **Hardware:** on the S3 with an INMP441, `level` fluctuates with how loud the room is, the spectrum bars track played tones, `peakHz` follows the dominant frequency, and raising `floor` keeps an ambient room dark.

## Source

[AudioModule.h](../../../src/core/AudioModule.h) · [AudioFrame.h](../../../src/light/AudioFrame.h) · [AudioLevel.h](../../../src/light/AudioLevel.h) · [AudioBands.h](../../../src/light/AudioBands.h) · [platform_esp32_i2s.cpp](../../../src/platform/esp32/platform_esp32_i2s.cpp)
