# INMP441 microphone — audio input (volume + FFT 16-band)

Forward-looking plan for the first **audio-reactive lighting** capability. Exempt from the present-tense rule like the rest of `backlog/`. Built **after** the Parlio rx/tx loopback (round 4); this is the increment that follows it.

**Status:** planned and approved (product owner, this session). Scope locked: **volume AND FFT 16-band together** in one increment (the PO reconsidered "volume-only first" with "we'll want FFT very soon"). AGC and pin auto-scan deferred.

## Context

The PO has an **INMP441 I²S MEMS microphone wired to the ESP32-S3** (bench board 192.168.1.156 / USB `…101`), pins **WS=GPIO4, SD=GPIO5, SCK=GPIO6**. The feature is chip-agnostic (gated on `platform::hasI2sMic` = `SOC_I2S_SUPPORTED`); the S3 is the test board and has an FPU for the float FFT. The INMP441 self-clocks from SCK (no MCLK pin/signal). It outputs 24-bit data in a 32-bit Philips-I²S slot, mono (the slot its L/R pin selects: GND=left, VDD=right).

## Key design decisions (locked)

- **Volume + FFT in one increment.** Volume scalar (RMS) AND a 16-band log spectrum + dominant-peak, so effects can react to level or frequency from day one.
- **Pure-math-vs-platform-kernel split** (the key to host-testing the WLED-MM tuning): only the **I²S read** and the **FFT kernel** sit behind the platform boundary (esp-dsp float `dsps_fft2r_fc32` on ESP32; a naive reference DFT on desktop so band tests run end-to-end). Everything else — DC strip, RMS, windowing, the magnitude→16-band log mapping, squelch/gain — is **pure header-only domain code** (`AudioLevel.h`, `AudioBands.h`), the `RmtSymbol.h`/`LcdSlots.h` shape.
- **MicModule** is a SystemModule `Peripheral` child (the role already exists), in `src/core/`. **AudioFrame** snapshot struct in `src/light/` (only effects consume it).
- **Three explicit pin controls** (`wsPin`=4 / `sdPin`=5 / `sckPin`=6) — no auto-scan in the minimum (research: SD-with-fixed-WS/SCK scan is feasible but unreliable in silence / on floating pins; every reference project uses explicit config).
- **sampleRate** a user control, default **22050** (FFT wants the higher rate; ~11 kHz Nyquist).
- **esp-dsp float, not fixed-point** — fixed-point is *slower* on FPU chips (esp-dsp benchmark fact). esp-dsp is a managed component (`espressif/esp-dsp`, added to `idf_component.yml` like `espressif/ip101`).

## Three WLED-MM lessons baked in (studied, never copied; credit MoonModules)

1. **Drop the I²S warm-up samples** — the INMP441 emits ~218 SCK cycles (~first 250 ms) of garbage after enable; discard them.
2. **Subtract the DC offset** — MEMS mics carry a DC bias; RMS without removing the block mean is corrupted. Mandatory.
3. **Squelch / noise gate** — a `squelch` control (noise floor); readings below it clamp to zero so idle hiss doesn't twitch the LEDs. Plus a `gain` multiplier (separate from squelch; AGC is a deferred opt-in).

## Steps (contract → red → green → hardware)

1. **Snapshot struct + platform API.** `src/light/AudioFrame.h`: POD `{ uint16_t volume; uint16_t volumeRaw; uint16_t majorPeakHz; uint16_t majorPeakMag; uint8_t bands[16]; uint32_t sampleCount; }`. `platform.h`: `AudioMicHandle` + `audioMicInit(ws,sd,sck,rate)` / `audioMicRead(int32_t*,n)→size_t` / `audioMicDeinit()`, and the FFT kernel seam `audioFft(const float* windowed, size_t n, float* outMag)`. `hasI2sMic` SOC flag in both platform_config.h. Desktop stubs: read returns 0; **`audioFft` does a naive DFT** so host tests exercise the real magnitude→band path.
2. **Pure math + tests FIRST (red).** `src/light/AudioLevel.h` — `computeVolume(samples,n,squelch,gain,AudioFrame&)` (24-in-32 shift, DC subtract, RMS, squelch clamp, gain, saturate). `src/light/AudioBands.h` — `applyWindow` (Flat-Top, per WLED-MM, for amplitude accuracy) + `magnitudesToBands(mag,nMag,rate,bands[16],peakHz,peakMag)` (log-spaced 16-band grouping, hand-tuned edges, top-band trim — re-derived not copied, + peak pick). Tests `unit_AudioLevel.cpp` + `unit_AudioBands.cpp`: silence→0, DC-only→0, known sine→expected RMS + single dominant band + correct peakHz (end-to-end through the desktop DFT), squelch clamps, gain scales, band-edge mapping, zero/odd no-crash.
3. **MicModule (green).** `src/core/MicModule.h`. Controls: `wsPin`/`sdPin`/`sckPin`, `sampleRate` (22050), `squelch` (10), `gain` (40), read-only `volume` + `majorPeakHz`. Lifecycle: `setup()`→`audioMicInit` (discard warm-up); `loop()`→read 512-sample block→`computeVolume` + `applyWindow`→`audioFft`→`magnitudesToBands`→store latest `AudioFrame_`. Robust: bad pins/init fail→status error, idles, never crashes; teardown releases the channel. Hot path: fixed member buffers, one float FFT/loop, no per-loop heap.
4. **Two effects (green).** `AudioVolumeEffect.h` (VU/pulse, brightness ∝ volume) + `AudioSpectrumEffect.h` (16 bands across grid X, magnitude→column). Both read the `const AudioFrame*` wired in `main.cpp`; both degrade to dark/idle with no mic.
5. **Wire + platform impl.** `platform_esp32_i2s.cpp` (`#if SOC_I2S_SUPPORTED` else inert): `i2s_new_channel` + `i2s_channel_init_std_mode` (Philips, 32-bit slot/24-bit data, mono slot matching L/R), enable, read, warm-up discard; the FFT kernel (`dsps_fft2r_fc32` + init twiddle tables once). `idf_component.yml`: `espressif/esp-dsp`. `main.cpp`: register MicModule + both effects, wire MicModule under `if constexpr (platform::hasI2sMic)` + markWiredByCode, pass `AudioFrame*` to effects. `esp32/main/CMakeLists.txt`: add the platform file.
6. **Pin auto-scan — deferred** (recorded under backlog.md audio section).
7. **Docs.** `docs/moonmodules/core/MicModule.md` + the two effect specs; `architecture.md` audio producer/consumer + the pure-math-vs-kernel split; `decisions.md` the volume+FFT decision, the host-testable-FFT split, esp-dsp-float-not-fixed, the WLED-MM lessons.

## Verification

Host gates (incl. `unit_AudioLevel` + `unit_AudioBands` with the sine→band end-to-end via the desktop DFT). ESP32 builds esp32 + **esp32s3-n16r8** + esp32p4-eth (esp-dsp links). Hardware on the S3: MicModule clean init; `volumeRaw` rises on noise / floors in silence; `squelch` clamps idle to 0; `majorPeakHz` tracks a played tone, `bands[]` light the matching spectrum region; AudioVolumeEffect brightens with sound, AudioSpectrumEffect columns track the spectrum. KPI — measure the per-tick FFT cost; if the tick balloons, FFT-every-N-ticks / smaller block / lower rate are the levers (tuning, not a blocker).

## Risks

INMP441 L/R slot (read the empty slot → flip it; first bench suspect) · warm-up discard timing (~250 ms) · FFT tick cost (one 512-pt float FFT/loop — measure) · esp-dsp twiddle-table init once at module init, not per loop.

## Out of scope (expand later)

AGC (normal/vivid/lazy, after squelch); beat/onset beyond raw peak; pin auto-scan as a first-class feature; 2D/palette frequency-reactive effects; per-band smoothing/decay tuning.
