# Audio DSP roadmap — source-seam extensions + adaptive noise gate (design study)

> Forward-looking design study (backlog, present-tense-exempt). The *shipped* audio path is
> documented present-tense in `src/core/AudioService.h`'s `///` (and its generated moxygen
> page); this study holds the **prior-art analysis** and the **not-yet-built** extensions the
> module's `///` credit points at.

## Prior art studied (credit by name)

Audio-reactive lighting is a long-standing idea in the LED-controller world (WLED-MM and MoonLight
are the closest lineage). projectMM's audio path is its own implementation, designed from the INMP441
datasheet and standard DSP — not traced from any one project — but three people's thinking is studied
here with respect and credited by name (the *Industry standards, our own code* principle: study hard,
write fresh).

**Frank (softhack007)** — main author of the WLED-MM audioreactive usermod (the most-used open-source
audio-reactive LED implementation), a direct ancestor of the ideas this module learns from. The
product owner worked alongside Frank for years on WLED-SR / WLED-MM before MoonLight and projectMM.
His concept is the worked example in the *Adaptive noise gate* section below: his idea, our analysis,
written fresh against our architecture.

**Troy (troyhacks)** — MoonModules team; keeps a WLED-MM fork where he reworked the audioreactive DSP
onto Espressif's **esp-dsp** library ("stupid fast compared to ArduinoFFT"), very low latency on S3/P4.
His contribution has two parts:
- *esp-dsp FFT.* Troy uses esp-dsp's **radix-4** real FFT (`dsps_fft4r_fc32`) with a Blackman-Harris
  window. This validates the path projectMM is already on — **we use esp-dsp too**, the **radix-2**
  float real FFT (`dsps_fft2r_fc32`) in `platform_esp32_i2s.cpp`. Same library; the one open
  optimisation is **radix-4 vs radix-2** (fewer butterfly stages, log₄N vs log₂N — a measure-then-
  maybe tune-up, not a gap; today the float FFT on the FPU is well inside one tick). Two adjacent,
  *not-yet-adopted* options for the record: (a) esp-dsp's **int16 / fixed-point** path uses the
  **built-in FFT instructions on S3/P4** — the lever for low-power FPU-less chips (C3/S2); we run
  float today because our targets have an FPU; (b) Espressif's standalone **`dl_fft`** component does
  *only* FFT without esp-dsp's shared twiddle tables — the right pick if a future build wants the FFT
  without the rest of esp-dsp (we take the whole esp-dsp because we also want its DSP primitives).
- *Biquad pre-filters.* Before the FFT, Troy runs the samples through **biquad** HP/LP/peaking filters
  via esp-dsp's `dsps_biquad_f32`, coefficients designed in EarLevel's Biquad Calculator v2. Squarely
  industry-standard (the biquad / second-order section is *the* canonical EQ building block; the Audio
  EQ Cookbook is the reference). Our pipeline does one fixed DC-blocker HP (~40 Hz); Troy's work shows
  the next step — a **configurable biquad chain** (HP to kill rumble, LP to tame aliasing, optional
  peaking to lift the mids the FFT under-reports). **Assessment: the biquad pre-filter chain is the
  higher-value idea to adopt** (improves spectral accuracy with off-the-shelf primitives), and it
  composes cleanly with the adaptive gate below (a learned gate on a cleanly-filtered signal beats one
  on a raw signal).

**LedFx** (Python, host-side) — a network LED effect engine whose whole purpose is audio reactivity,
running on a PC and streaming pixels to WLED-class devices. Different architecture to ours (the host
renders, the device receives; we already interoperate through Art-Net / E1.31 / DDP in both
directions), so most of it does not transfer. Two pieces of its *analysis* do, and both are studied
in § Band spacing below: its **mel/bark band spacing** with hand-tuned variants, and its
**per-band asymmetric smoothing**. Worth naming because it reached those two independently of the
WLED lineage above, and its own source comments are unusually candid about which variants work.

**Damian Schneider (DedeHai)** — WLED core dev; WLED's audioreactive usermod carries an integer /
fixed-point FFT path (~1.5 ms on a C3, >10× ArduinoFFT on FPU-less chips). The consensus (Troy + Frank)
is that with esp-dsp FFT + biquads, **fixed-point is not necessary on FPU chips** (S3/P4) — projectMM's
exact position: float on FPU targets, the int16 / `dl_fft` hardware path reserved for low-power chips.
DedeHai's current audio experiment is a PoC MSGEQ7-based path (offloading the spectrum to a dedicated
analyser chip) — a different point in the same space, noted for completeness.

## Source-seam extensions (widen what feeds the pipeline)

All of the following widen the **source seam** — what feeds the pipeline — leaving the DC-blocker / RMS
/ FFT / band analysis untouched. In roughly increasing hardware complexity:

- **I²S with MCLK, for line-in — largely SHIPPED.** The INMP441 is self-clocked (no MCLK); a line-in
  codec needs a master clock the ESP32 drives. The module already carries an `mclkPin` control (the
  I²S peripheral drives MCLK for a line-in ADC; a codec board uses the codec's own MCLK). Remaining
  source-seam work is the other three types below.
- **PDM mics** — a different I²S sub-mode (the IDF `i2s_pdm` driver), a variant behind the same
  platform read.
- **Analog line-in.** DedeHai got analog input working on the S3; Troy got it working in ParrotRadio.
  Troy's testing-confidence nuance worth recording: he considers his ParrotRadio analog path
  better-exercised (he actually recorded + played back through it), whereas an unlistened-to analog
  path "may not be as accurate as it looks." **If projectMM adopts analog line-in, validate by
  listening**, not just by watching the level meter.
- **I²C-configured codecs (e.g. ES8311).** Do **not** hand-roll each codec's register config — pull in
  Espressif's **`esp_codec_dev`** component (carries option tables for many codecs), supporting "a
  bunch more codecs for free." The *Industry standards, our own code* call applied to codec bring-up.

Troy also has **DSP boards** (I²S front-ends "way beyond regular codecs") — recorded so the line-in /
codec work leaves room for that class of source.

## Band spacing: the geometric split wastes a quarter of the display

**The defect, measured.** `magnitudesToBands` splits the FFT bins geometrically:
`edge[e] = nMag^(e/16)`, equal frequency RATIO per band. That is the textbook way to map linear bins
onto pitch, and at our shipped shape (512-sample FFT, 22050 Hz, so 256 bins of 43.1 Hz) it produces
this:

| band | bins | count | Hz |
|---|---|---:|---|
| 0 | 1-1 | **0** | 43-43 |
| 1 | 1-2 | 1 | 43-86 |
| 2 | 2-2 | **0** | 86-86 |
| 3 | 2-4 | 2 | 86-172 |
| 4 | 4-5 | 1 | 172-215 |
| 5 | 5-8 | 3 | 215-345 |
| ... | | | |
| 15 | 181-256 | **75** | 7795-11025 |

**Two bands have no bins at all and two more have a single bin**, while the top band averages 75.
A quarter of a 16-band display is dead or nearly dead, and it is the quarter where music has its
energy. On a spectrum effect that reads as bass bars that barely move while the treble end is a
smear of everything above 8 kHz.

The cause is that a geometric split is scale-free but the FFT is not: below `binHz * 2` there are
simply no distinct bins to hand out, so the low bands collide. Raising the FFT size fixes it by
brute force (1024 samples halves `binHz`) at 2x the compute and 2x the latency, which is the wrong
trade on an MCU.

**What LedFx does instead.** Its melbank offers six spacings and its own comments rank them: standard
HTK mel is "weak on the bass and high end"; `bark` "needs tinkering"; plain `triangle` "kinda sucks".
The two it ships as defaults are hand-tuned log variants, `scott_mel` (base 9) and `matt_mel`
(base 12, the default), written explicitly to "spread out the low range and compress the highs". It
also splits into THREE banks (lows 20-350, mids 350-2000, highs 2000-15000) so an effect that wants
bass gets a bank resolved for bass rather than picking bins out of one spread.

**What to take.** Not the three banks (memory, and our 16-band frame is a published contract), and not
mel as an unexamined default: mel is a perceptual scale for SPEECH, and its low end is coarser than
ours, which is the opposite of the fix. What transfers is the principle behind `matt_mel`: **choose
the band edges so every band owns bins, then bias what is left toward the low end.** A concrete shape:
start the edges at `binHz * 2` rather than bin 1, enforce a minimum of one bin per band by
construction, and distribute the remainder on a log curve whose base is a control rather than a
constant. Cost is zero in the hot path (17 edges, computed once per rate change), so this is a table
change with a golden test, not an architecture change.

**How to judge it.** A swept sine from 40 Hz to 10 kHz should light each band in turn, one at a time,
with no band staying dark and no band lit twice. That is a unit test, and the current split fails it
by construction at bands 0 and 2.

## Per-band smoothing: the ballistic belongs on the band, not the effect

`smoothFollow` exists (`math16.h`) and `SpectrumEffect` uses it, together with `peakHold`. So the
mechanism is built; what is missing is WHERE it is applied and whether it is asymmetric.

**Today** each effect smooths for itself, symmetrically, if it remembers to. That has three costs: a
new audio effect starts jittery until its author rediscovers the problem, two effects on the same
signal disagree about what the spectrum is doing, and a symmetric filter is the wrong ballistic for a
meter, making the attack as sluggish as the decay and rounding off exactly the drum hits an effect
exists to show.

**LedFx applies an asymmetric exponential filter per band** inside the melbank (rise 0.99, decay 0.7:
fast up, slow down), so every effect downstream sees an already-ballistic spectrum. WLED and FastLED
converged on the same asymmetric form independently, which is recorded in
[power-functions-analysis-bottom-up](power-functions-analysis-bottom-up.md) G4.

**What to take.** Move the ballistic into `AudioFrame`, beside `level` and `levelSmoothed`, as a
`bandsSmoothed[16]` computed once per block with separate rise and decay constants. `bands` stays raw
for anything that wants the instantaneous value, exactly as `level` and `levelSmoothed` already
coexist: the pattern is established, this extends it to the spectrum. Cost is 16 bytes of frame and
32 multiply-adds per block, which is nothing against the FFT.

**How to judge it.** A step input should reach 90% within one block and fall to 50% over roughly ten,
and a kick drum should be visibly sharper on a spectrum effect. Both are testable on the host with a
synthetic signal.

## Per-band conditioning: the generic form the two above are special cases of

The two sections above fix band EDGES and band BALLISTICS. The third axis is band LEVEL, and once it
is per band the other two get easier. This section is the generic design; the industry names are used
throughout, because every part of this is a standard piece of audio equipment.

### The problem, measured

`floor` and `gain` are single global values applied identically to all 16 bands (`AudioBands.h`,
`magToByte`). A band reports the PEAK magnitude of its bins (so a single tone lights one band rather
than smearing), and pink noise (equal energy per octave, the standard reference for typical program
material) has a per-bin magnitude falling as 1/sqrt(f). So under pink noise, with one gain, the
bands read:

| band | lowest Hz | relative reading |
|---:|---:|---:|
| 0 | 43 | 1.00 |
| 4 | 215 | 0.45 |
| 8 | 689 | 0.25 |
| 12 | 2756 | 0.12 |
| 15 | 7795 | 0.07 |

A 14x spread across the display from a signal that is, by definition, spectrally balanced. No single
gain serves both ends: set it for the bass and the treble is a flicker, set it for the treble and the
bass pins at full. Every microphone, line-in and genre shifts the curve further (a MEMS mic's own
response, a room's bass build-up), which is why a hand-tuned table is tuned for exactly one setup.

(An earlier draft of this section had the spread the other way round, from a bin-count argument
that applies to a SUM per band. The aggregation is a max, so the treble is the starved end.)

### What the industry calls the parts

Naming them correctly is what makes this recognisable rather than bespoke:

- **Band gain / makeup gain** — the per-band multiplier. A graphic equaliser's slider.
- **AGC (automatic gain control)** — deriving that gain from the signal rather than from a control.
  Its two time constants are **attack** and **release**; a **hold** delays the release after a peak.
- **Noise floor** — the level a band reads with no program material. Learned per band, since mic
  self-noise and mains hum at 50/100 Hz are not flat.
- **Ballistics** — how fast a meter rises and falls. **Peak** ballistics rise instantly; **VU** is
  slow and averaged; **PPM** (peak programme meter, IEC 60268-10) rises fast and falls slowly, which
  is the standard broadcast choice and exactly the asymmetric filter § Per-band smoothing describes.
  Naming ours PPM-like says more than "asymmetric smoothing" does.
- **Weighting curve** — a fixed per-band offset applied before anything else. **A-weighting** (IEC
  61672) matches human hearing at low levels; **C-weighting** is flatter; **Z** is none. The
  "loudness" button on a hi-fi is a bass-and-treble lift at low volume, from the equal-loudness
  contours (ISO 226, the Fletcher-Munson curves).

### The design: two tiers, because full per-band normalization is the wrong instrument

The multiband dynamics literature names the trap before we can fall into it. A compressor that
normalizes every band independently "throws off the entire spectral balance" (Recording Magazine on
multiband dynamics; the same problem is the subject of US patents 8,634,578 and 9,673,770, "multiband
dynamics compressor with spectral balance compensation"). Music with more lows than highs is SUPPOSED
to show more lows than highs; a display that levels every band to the same height has erased the one
thing it was meant to show. The product owner's instinct here was exactly this objection.

The standard resolution, from those patents and from hearing-aid WDRC (2 to 24 channels of exactly
this, on DSPs far smaller than an ESP32, so cost is settled): **two tiers with different speeds.**

- **Slow, applied EQUALLY to all bands**: overall loudness. This preserves the spectral balance by
  construction, because every band moves by the same amount. It is the global `gain` we have,
  made automatic.
- **Slow, per band, converging to a STATIC curve**: calibration. Mic response, room, bin width. It
  learns the fixed shape of this rig and this room, and then holds it; it is not chasing the music.
  This is where `ratio` lives: 1:1 leaves the raw balance, higher ratios flatten the rig's own
  coloration.
- **Fast, per band**: ballistics only (the PPM rise and fall). Never gain. A fast per-band gain is
  what causes cross-spectral pumping, and it is the one thing all three sources say not to do.

The sequencing is a dependency, not a preference. WLED's own docs state it for their global AGC:
squelch must be set first "so AGC knows what silence looks like". The floor converges BEFORE the gain
loop starts, or the gain learns the noise.

Three per-band arrays, all computed once per block, feeding the existing `magToByte`:

1. **`bandFloor[16]`** — a running minimum with a slow release, so it tracks silence but recovers if
   the room gets quieter. This is the frequency-domain half of the adaptive noise gate below; the two
   share a mechanism and should share code.
2. **`bandGain[16]`** — a running maximum with attack/release/hold, normalising each band to its own
   recent peak. This is the piece that makes band edges much less critical: a band with few bins
   simply learns a higher gain. It does NOT fix a band with zero bins, which is why § Band spacing is
   still a correctness fix rather than a taste one.
3. **`bandsSmoothed[16]`** — the PPM ballistic from § Per-band smoothing, applied after the other two.
   In scope with this work, not separate: gain decides how much, ballistics decide how fast, and
   tuning either without the other chases its own tail.

### Weighting presets, on top rather than instead

A learned gain answers "make every band visible". It does not answer "which bands SHOULD dominate",
which is taste and genre. So a **weighting curve** rides on top as a fixed per-band offset in dB:

- **Flat (Z)** — what the FFT saw. The reference.
- **A-weighting** — perceptual, de-emphasises the extremes. Good for a level meter.
- **Loudness** — the equal-loudness lift: bass and treble up, mids down. What people mean by "make
  it punchy".
- **Pink** — compensates the bin-count spread so pink noise reads flat. The right default for a
  spectrum display, and computable from the band edges rather than tabled.

Four 16-entry int8 tables, applied as an offset before the gain. A user picks a name, not sixteen
sliders.

### Measurement modes, and what they cost

The product owner's three modes are the right shape, and the cost settles which is the default:

- **Keep** — learn nothing; use the stored table. Deterministic, and what an installation wants once
  it is tuned. Also the answer for a show that must look identical every night.
- **Period** — learn during an explicit window (a "calibrate" button, or the first N seconds), then
  freeze. One honest use: point it at the room before doors open.
- **Continuous** — always adapting, with slow constants.

**Continuous is affordable.** The analysis runs once per 512-sample block (~23 ms), never per light
and never per tick, and the whole audio path measures **136 us per block on an ESP32-S3**, which is
0.59% of the block period. The three additions are 16 compares, 16 compares plus 16 multiplies, and
16 multiply-adds: about **0.6 us, or +0.4% on the audio path**, and nothing at all on the render hot
path. Continuous can be the default; the modes exist for determinism, not for cost.

The real argument against continuous is musical rather than computational: **AGC breathes**. A quiet
passage gets amplified until it looks loud, which is wrong when the quiet is the point.

The guards are the standard compressor controls, and there are two distinct things a user means by
"how aggressive" which must not be collapsed into one slider:

- **`ratio`** — how much of the difference the learner removes, the compressor's depth control. 1:1
  is off (each band keeps its raw level) and higher ratios push the bands toward equal. This is
  "how much".
- **`attack`** and **`release`** (ms and seconds) — how fast the learned gain follows a rise and
  recovers afterwards. **Release is what breathing is**: a fast release re-levels a quiet passage
  until it looks loud. This is "how fast".
- **`hold`** — a delay before release begins, so the gain does not pump between beats. The standard
  companion to release, and the cheap fix for the most audible artefact.
- A **maximum gain** in dB, so a silent band cannot be amplified into its own noise. A limit rather
  than a ballistic, and the guard that makes the rest safe.

Defaults worth starting from: a low ratio, a fast attack, a release in seconds, and a hold of about
a beat. Those are conservative on purpose, the settings a broadcast limiter would use, so the
learner is invisible until someone goes looking for it.

### What replaces `floor` and `gain`

The global pair goes. Keeping them as an offset on top of a learned per-band pair would be two
mechanisms doing one job, kept only to avoid a break, which is the debt
[ADR-0013](../adr/0013-no-migration-code-robust-persistence-plus-documented-breaks.md) exists to
refuse: document the break, do not carry the old shape forward.

What a user needs from them survives in a better form. `gain` becomes **`ratio`**, how far the
learner is allowed to push the bands toward equal, which is the compressor's own name for that
control and the one someone actually reaches for.
`floor` becomes the learned per-band floor, which is what they were approximating by hand. The
break gets its MIGRATING entry, and a restored config loses two values that the learner re-derives
within seconds of hearing audio.

### The UI: one read-out row, not sixteen

Nothing per band is something a user SETS. Every knob in this design is global on purpose (`ratio`,
`attack`, `release`, `hold`, the maximum gain, the measurement mode, the weighting preset), and the
per-band floor and gain are learned observations. So a sixteen-row list, considered here first
because `ListSource` makes it cheap to build, would be a diagnostic costing a 1 Hz push of sixteen
rows for values nobody edits. And the live per-band picture already exists: the AudioSpectrum
effect in the preview IS the spectrum analyser.

What earns its place is one row: a compact read-out of the sixteen learned gains (or floors,
toggled), sixteen small numbers in one string, updated at the same 1 Hz cadence and in the same
shape as the `level RMS` and `peakHz` read-outs beside it. About fifty bytes, no new traffic
pattern, and it answers the one diagnostic question ("is a band stuck, or pinned?") at a glance. A
per-band manual trim is not built: `ratio` and the weighting preset cover taste, and sixteen sliders
are real estate for a control nobody reaches for.

Still worth doing FIRST, before the learning: the read-out row showing the raw band levels is a
diagnostic for § Band spacing on its own, and it makes an empty band visible to a user rather than
only to a unit test.

### How to judge it

- **Pink noise in, flat display out** with the pink weighting: the direct test of the bin-count fix.
- **A swept sine lights each band in turn**, none dark, none twice: the band-edge test.
- **Silence converges to zero** within the learning window on a real mic, including 50/100 Hz hum.
- **A kick drum is visibly sharper** than with symmetric smoothing: the ballistics test.
- **Switching genre re-levels within seconds** without visible pumping on sustained material.

## Onset and tempo: reactive first, predictive after

`BeatPhase` is a clock, not a detector: the user sets a BPM and effects follow it. Nothing in the
tree hears a beat. Two standard pieces would, and they are different in kind, which matters because
the usual worry ("the beat has happened before you know it was one") applies to one and is answered
by the other.

**Onset detection is reactive, cheap, and nearly free here.** The standard onset detection function
is **spectral flux** (Bello et al. 2005, Dixon 2006): the sum over bands of the POSITIVE difference
between this block's magnitude and the last. A rise across the spectrum is a hit; a fall is not. We
already compute the 16 band magnitudes once per block, so flux is 16 subtractions and 16 compares on
top, with the block's own ~23 ms latency, which is under what a viewer can attribute. An `onset`
field in `AudioFrame` (0 or a strength, 0..255) is the smallest useful shape, and a per-band flux is
the same loop kept per band for an effect that wants to know WHERE the hit was. Thresholding the
flux against its own recent mean is what separates a hit from a swell, and it reuses the learned
floor machinery from § Per-band conditioning.

**Tempo tracking is predictive, and it is the answer to "too late".** A tracker does not react to a
beat, it locks onto the periodicity of the onset stream and predicts the NEXT one, so an effect can
fire on the beat or ahead of it. The reference form is Ellis, "Beat Tracking by Dynamic Programming"
(JNMR 2007): estimate the dominant period of the onset function by autocorrelation, then choose beat
times that agree with both the period and the onset strength. Real-time variants exist (IBT, OBTAIN)
and run hundreds of times faster than real time on a desktop. On an MCU the cost is the
autocorrelation over a few seconds of onset history: about 260 values at block rate over the lags
that span 40 to 180 BPM (14 to 65 blocks), roughly 13,000 multiply-adds. Run every block that is
50 to 100 us on an S3, most of the audio budget again; run ONCE PER SECOND, which is all a tempo
estimate needs, it amortises to under 1% of one block. So the tracker is scheduled at its own
cadence, not the block's, and like everything in this file it is in the audio path, never the
render path. Unmeasured until built.
What it delivers is a `bpm` estimate and a `beatPhase` prediction, which is exactly what `BeatPhase`
consumes today from a control: the clock stays, the number feeding it becomes measured.

**Sequence.** Onset first: it is the cheaper piece, it has immediate value (a flash on the hit), and
it is the input the tracker needs anyway. Tempo tracking second, judged on the bench against a
metronome and against real music with a known BPM, because a tracker that locks onto the half-time
or double-time pulse looks confidently wrong.

**How to judge it.** A click track produces one onset per click and no others; a sustained tone
produces none after its attack; a track with a known BPM yields that BPM within 2% after a few
seconds and holds it, and the predicted beats land within one block of the true ones.

## Adaptive noise gate (softhack007's concept, our analysis)

Replace the borrowed `squelch`/`noiseFloor` knob ("a WLED-SR workaround, not a real gate") with a
proper adaptive noise gate. From softhack007 (granted permission to analyse); the assessment is ours.

**The concept:** a standard [noise gate](https://en.wikipedia.org/wiki/Noise_gate) (below a threshold
the signal is silenced, above it passes), **asymmetric bang-bang timing** (open fast, close slow;
hysteresis avoids threshold chatter), driven by a **new "detect silence" function** (the explicitly
unfinished part). Leave the GEQ/FFT bands untouched (the gate acts on the time-domain signal). The
closing pre-condition should be **relative** ("percentage of average signal"), not an absolute count.
Optionally feed the gate **compressed samples** (sqrt/log) so the threshold behaves perceptually.

**Five design constraints (the load-bearing part):** (1) samples are signed, arbitrary magnitude —
scaling to an effect range is AGC's job, not the gate's; (2) every `abs()` must be justified (a
rectify discards sign/phase); (3) prefer relative factors to absolute thresholds (the one allowed
absolute: changes < 2 counts are sampling noise); (4) smooth before thresholding; (5) every filter
adds delay — total audio delay must stay < 30 ms.

**Verdict: yes, directionally — it's squarely industry-standard** (a hysteresis gate with
fast-attack/slow-release is how studio gates, radio squelch, and voice-activity detectors all work),
and moves us off the borrowed `squelch` constant. The relative-threshold insight (constraint 3) is the
valuable core: a gate keyed to a *learned* floor self-calibrates to whatever source is connected.
**Two cautions:** (1) timing is tight — a 512-sample block at 22050 Hz is already ~23 ms, leaving
< ~7 ms of the 30 ms budget; any smoothing must be cheap (one-pole) and proven on hardware; (2) it
overlaps the already-scoped per-band floor, so decompose and adopt in steps, don't overhaul.

**Per-band floor overlap:** the backlogged per-band noise floor learns each band's idle baseline
(frequency-domain floor — kills a steady tone like the bench's ~258 Hz hum); the proposed gate answers
"is there any sound at all" (time-domain floor). Complementary halves, not competitors.

**Decomposition (cherry-pick, most value early):**
1. **Per-band noise floor (already backlogged)** — ship first; frequency-domain half, smallest change,
   kills the concrete hum. Independent.
2. **Relative thresholds reusing the RMS we already compute** — `computeLevel` already produces a
   per-block RMS (an envelope estimate, and the one justified `abs()` under constraint 2). A
   learned-floor follower over that RMS with open/close as *factors* of it is a small, host-testable
   addition needing no new DSP stage and no extra delay. **The cherry to pick.**
3. **Hysteresis + asymmetric timing** — two time-constants on that follower plus a close-hold; where
   the < 30 ms budget gets measured for real.
4. **Defer until proven:** log/dB-domain thresholds (downstream `magToByte` already compresses
   perceptually), a true soft gate (0..1 gain vs hard 0/1).

**Eventually retires:** the `floor` knob's hard-squelch role — `floor` becomes the *display*
noise-floor only (the dB-window bottom in `magToByte`), while the learned gate decides "is there
sound." A clean subtraction, but the *end* of the path, not the first step.
