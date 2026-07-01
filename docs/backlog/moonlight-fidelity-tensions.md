# MoonLight migration — fidelity tensions

A running log of places where **strict fidelity to MoonLight's behaviour** (the migration mandate:
end users must see the same effect they always have) collides with a **projectMM principle**
(robustness / no-crash-at-any-grid-size, correctness, hot-path discipline, *common patterns first*).

For each: what MoonLight does, what the principle wants, what was shipped, and the **decision needed**.
The product owner resolves these — the agent records them rather than silently choosing.

Status legend: 🟡 open (needs PO decision) · 🟢 resolved (decision recorded) · ⚪ accepted-as-is (kept faithful, no change wanted)

---

## 1. 🟡 GEQ3D / GEQ — `cols / NUM_BANDS` integer bar width collapses on narrow grids

- **MoonLight:** bar x-position is `linex = i * (cols / NUM_BANDS)` and width `cols / NUM_BANDS`
  (verbatim, verified in E_MoonModules.h). On a grid where `cols < numBands` (e.g. 8 columns,
  16 bands) this integer division is **0**, so every bar collapses to x=0.
- **Principle in tension:** *Effects must run at every grid size* / robustness. The result is
  degenerate (all bars stacked at column 0) though not a crash.
- **Shipped:** kept faithful — reproduces MoonLight's `cols / NUM_BANDS` exactly. No crash (draw
  primitives clip; early-return on 0×0). CodeRabbit flagged it; accepted-with-reason because
  "fixing" the bar math to remapped boundaries would change bar placement on **normal** grids too
  (different rounding), diverging from what users see in MoonLight.
- **Decision needed:** is "16 bands need ≥16 columns, else they pile at x=0, same as MoonLight"
  acceptable? Or do we want a projectMM-only guard (e.g. clamp numBands to cols, or remap bar
  boundaries) accepting a *visible divergence from MoonLight* on normal grids? Fidelity says leave
  it; robustness-purism says guard it. **PO call.**

## 2. 🟡 GEQ3D — projector sweep is frame-counter throttled, not time/BPM based

- **MoonLight:** `if (counter++ % (11 - speed) == 0) projector += dir;` — the vanishing-point sweep
  advances per *loop call*, so its real-world speed depends on the device's frame rate (a fast
  device sweeps faster than a slow one at the same `speed` setting).
- **Principle in tension:** projectMM's convention is time-based motion (BPM / elapsed-ms), so an
  effect looks the same on a 30 FPS and a 280 FPS device. CodeRabbit flagged the frame-dependence.
- **Shipped:** kept faithful — frame-counter throttle, exactly as MoonLight. This means GEQ3D's
  sweep speed is **not** frame-rate-independent (a known projectMM-wide expectation it breaks).
- **Decision needed:** convert the sweep to elapsed-ms/BPM (frame-rate-independent, the projectMM
  way, but **visibly different cadence** from MoonLight on any given device) — or keep MoonLight's
  frame-counter (faithful, but speed varies by device FPS)? This is the cleanest example of the
  fidelity-vs-projectMM-convention tension. **PO call.**

---

## 3. ⚪ SolidEffect / general — `scale8` vs integer `*bri/255` rounding

- **MoonLight:** brightness in several effects is plain integer `channel * brightness / 255`
  (truncating), not FastLED's `scale8` (which has a +1 video-rounding so `scale8(x,255)==x`).
- **Principle in tension:** projectMM's standard channel-scale op is `scale8`. Using it would be the
  "common patterns first" choice, but it rounds ~1 LSB higher than MoonLight at non-255 brightness.
- **Shipped:** kept faithful — used MoonLight's exact `* bri / 255` where the source does (the 3a
  verifier caught and corrected a `scale8` slip in SolidEffect). At brightness=255 (default) both
  agree; the difference is sub-perceptual at other values. Accepted as-is (faithful), recorded so we
  know the rule: **match MoonLight's exact rounding per effect, don't reflexively swap in scale8.**

## 4. 🟡 Audio effects — `volume` normalization scale (0..1 float vs our 0..255 `level`)

- **MoonLight:** audio effects read `sharedData.volume` as a normalized float (roughly 0.0..1.0+),
  e.g. FreqMatrix's gate `volume > 0.25`. projectMM's `AudioFrame::level` is a small integer
  (~0..255, the VU value).
- **Principle in tension:** fidelity needs the *same trigger points* (a beat that lights MoonLight
  must light ours), but the units differ, so a literal `> 0.25` is wrong against `level`.
- **Shipped (3d batch):** the ports scale the thresholds proportionally (e.g. `0.25` of full-scale →
  `~64` on the 0..255 `level`) and flag each with a comment. This is a **reconstruction**, not a
  verbatim match — the exact trigger point depends on how our AudioModule scales `level` vs how
  MoonLight scales `volume`.
- **Decision needed:** confirm the level-scale mapping on hardware (does our `level` reach ~255 at
  the same loudness MoonLight's `volume` reaches ~1.0?), and whether `volumeRaw` (which we don't
  have separately — NoiseMeter uses it) needs a real raw value added to AudioFrame, or `level` is a
  good-enough stand-in. **PO call after bench.**

## 5. ⚪ Audio effects — `AudioFrame` has no separate `volumeRaw`

- **MoonLight:** NoiseMeter reads `sharedData.volumeRaw` (unsmoothed), distinct from `volume`.
- **projectMM:** `AudioFrame` has only `level` (smoothed RMS). The 3d NoiseMeter port uses `level`
  as a stand-in (commented).
- **Decision needed:** add a `volumeRaw`/unsmoothed field to `AudioFrame` + AudioModule (a small
  producer change) for an exact NoiseMeter, or accept `level` as the stand-in? Low stakes; **PO call.**

## 6. 🟡 Reconstructed logic — effects whose MoonLight source was incomplete (cross-check on bench)

The fetched MoonLight source for some effects was partial, so parts were **reconstructed** from the
WLED algorithm + the documented intent and marked `// RECONSTRUCTED` in the code. These render and
are plausible, but the exact behaviour needs a side-by-side bench check vs MoonLight/WLED:

- **Tetrix** — *no full source was available* (only condensed pseudocode). The fall-speed physics
  (`map(speed,1,255,40000,250)` descending, the per-frame drop, the brick-width formula) are
  reconstructed from the spec. (A real bug was caught + fixed: the speed-map result 250..40000 was
  being truncated into a `uint8_t`, wrapping the physics — now widened.) **Cross-check the fall
  cadence vs WLED 2D Tetrix.**
- **FreqMatrix** — the scroll-throttle gate is reconstructed (WLED uses a `micros()`-based 0..15
  "second hand"; the port reproduces the *intent* with an elapsed()-ms period). A **real D1
  dimensionality bug was caught + fixed** (it was writing along X; D1 writes the x=0 column along Y).
  **Cross-check scroll speed + orientation.**
- **Blurz** — two position branches (the `freqMap` log-frequency placement and the band-scan) were
  reconstructed (source incomplete). **Cross-check dot placement vs WLED Blurz.**
- **FreqSaws** — the `targetSpeed = volume * increaser * 257` overflow/scaling behaviour is
  uncertain (`volume*increaser` already exceeds 16-bit before `*257`); reproduced as-described but
  flagged. **Cross-check band-speed response.**
- **GEQ** — the falling-peak (`ripple`) markers and `fadeOut` are reconstructed to the standard WLED
  2D GEQ (the core band→bar math is verbatim-equivalent). **Cross-check peak-dot fall.**
- **NoiseMeter** — the `aux0/aux1` per-frame noise-scroll increments were unspecified in source;
  reproduced with `beatsin8(5/4,...)` (matches WLED mode). Minor; **cross-check drift speed.**
- **BouncingBalls / PacMan / Ant** — had full source; only standard `map()`/`random` helper bodies
  were reconstructed (FastLED-equivalent). Note: `random16(min,max)` was done as the contract's
  modulo form, NOT FastLED's scaled-draw, so the *seeded random sequence* differs slightly (Ant's
  initial velocities) — visually equivalent, not bit-identical.

These are all in the code with `// RECONSTRUCTED` markers; grep for them to find the exact lines.

## Resolved / accepted

_(⚪ entries above are kept-faithful, no change wanted unless the PO revisits; 🟡 entries await a decision.)_
