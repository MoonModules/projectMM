# MicModule

Reads a digital I²S MEMS microphone and publishes one analysed **AudioFrame** per render tick: an overall sound **level**, a 16-band frequency **spectrum**, and the **dominant peak**. It is the producer half of the audio-reactive pipeline — [AudioVolumeEffect](../light/effects/AudioVolumeEffect.md) and [AudioSpectrumEffect](../light/effects/AudioSpectrumEffect.md) are the consumers.

A SystemModule **Peripheral** child, code-wired in `main.cpp`. Chip-agnostic: it is created only where the platform has an I²S peripheral (`platform::hasI2sMic`, true on every current ESP32, false on desktop). On a mic-less build it isn't created and the audio effects read a permanently-silent frame, so they simply stay dark.

## Hardware — INMP441-class digital mic

Built and tested against an **INMP441** (a self-clocked I²S MEMS microphone): standard/Philips framing, 24-bit data left-justified in a 32-bit slot, mono. Three wires plus power:

| Control | Default | Pin |
|---|---|---|
| `wsPin` | 4 | word-select / LRCLK |
| `sdPin` | 5 | serial data out of the mic |
| `sckPin` | 6 | bit clock |

The part is self-clocked from the bit clock — there is no master-clock (MCLK) pin. It drives the one slot its L/R select pin chooses (tie L/R to GND for the left slot, VDD for the right); if `level` stays at the floor with sound present, the mic is filling the other slot — the fix is one wire, not firmware.

## How the AudioFrame is produced

Each `loop()`: read a block of samples → compute the level → window + FFT → map to bands.

- **Level** (`AudioLevel.h`, host-tested): subtract the block's DC mean (a MEMS mic carries a large DC bias that would otherwise dominate), take the RMS, and map it through the log/dB window (`floor` / `gain`). It is the overall loudness, independent of the FFT — the VU value. (It uses a gentler floor than the bands so the meter keeps moving with volume rather than gating hard.)
- **Spectrum** (`AudioBands.h`, host-tested): apply a Hann window (the standard general-purpose FFT window — tapers the block edges so a tone doesn't smear across bins), run the FFT (`platform::audioFft`), then group the magnitude bins into 16 log-spaced bands and pick the loudest bin as the dominant peak.

Only the I²S read and the FFT kernel are platform code (`platform_esp32_i2s.cpp` — IDF's `i2s_std` driver + esp-dsp's float `dsps_fft2r_fc32`); everything else is plain domain math that runs in CI on the desktop's reference DFT.

## Controls

- `wsPin` / `sdPin` / `sckPin` — the three I²S GPIOs (see table above). Changing any re-creates the I²S channel.
- `sampleRate` — a dropdown over the standard rates (8000 / 16000 / 22050 / 44100 Hz), default **22050** (~11 kHz Nyquist covers the range that matters for light). Changing it re-creates the channel.
- `floor` — the noise floor: bands and level below this read as silence, so an ambient room stays dark. Raise it for a noisy room, lower it for a quiet one. Default 100.
- `gain` — sensitivity: higher = more (a narrower dB window, so a given sound fills more of the bar). Default 222.
- `level` — read-only live sound level (updates each second).
- `peakHz` — read-only dominant frequency (updates each second).

## Cross-domain wiring

MicModule produces an `AudioFrame` (`src/light/AudioFrame.h`); the consuming effects reach the live frame through the static **`MicModule::latestFrame()`** — not a boot-time setter — so an effect added through the UI at any time still finds the one mic, and with no mic it gets a static silent frame. The active module registers itself in `setup()` and clears that pointer in `teardown()`, so adding or removing the mic (or an effect) in any order always leaves a coherent answer.

The first ~250 ms after the I²S clock starts are power-on settling garbage; the read is non-blocking (the hot-path rule), so those samples flow through the first few `loop()` reads and the level/bands self-correct within that quarter-second — the frame stays valid (zeroed) until then.

## Prior art

Audio-reactive lighting is a long-standing idea in the LED-controller world; this is projectMM's own implementation, designed from the INMP441 datasheet and standard DSP (RMS level, Hann-windowed FFT, log-spaced bands) rather than any one project's code.

## Tests

- **Level (CI, host):** `test/unit/light/unit_AudioLevel.cpp` — silence → 0, pure DC → 0 (offset stripped), a louder sine reads higher, DC bias doesn't change a sine's level, a high `floor` gates a modest signal to 0, higher `gain` reads higher, null/empty input is silence.
- **Spectrum (CI, host):** `test/unit/light/unit_AudioBands.cpp` — a tone lands in the right band and the reported `peakHz` tracks it (end-to-end through the desktop reference DFT), energy concentrates rather than smears, degenerate input never crashes.
- **Hardware:** on the S3 with an INMP441 — `level` fluctuates with how loud the room is, the spectrum bars track played tones, `peakHz` follows the dominant frequency, and raising `floor` keeps an ambient room dark.

## Source

[MicModule.h](../../../src/core/MicModule.h) · [AudioFrame.h](../../../src/light/AudioFrame.h) · [AudioLevel.h](../../../src/light/AudioLevel.h) · [AudioBands.h](../../../src/light/AudioBands.h) · [platform_esp32_i2s.cpp](../../../src/platform/esp32/platform_esp32_i2s.cpp)
