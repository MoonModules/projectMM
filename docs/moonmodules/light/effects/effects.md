# Effects

Every effect, one block each: its preview, what it does, and what each control means — together. An effect writes per-pixel colour into its [Layer](../Layer.md)'s buffer each tick; [modifiers](../modifiers/modifiers.md) reshape the result and a [driver](../drivers/) sends it out. Effects that name an index colour read the global palette (the `palette` control on [Drivers](../Drivers.md)) via `colorFromPalette`. Each block's emoji are its `tags()` (origin/creator/audio — see the [tag emoji legend](../../../architecture.md#tag-emoji-legend)); **Dim** is its native axes ([Layer](../Layer.md) extrudes a lower-dim effect onto a bigger grid). Origin is grouped into sections; the per-library file split is future work — see the [folder-structure decision](../../../backlog/folder-structure-proposal.md).

**Jump to:** [MoonLight](#moonlight-effects) · [MoonModules](#moonmodules-effects) · [WLED](#wled-effects) · [FastLED](#fastled-effects) · [projectMM-native](#projectmm-native-effects)

## MoonLight effects

<a id="rainbow"></a>

### Rainbow 💫 · 2D

<img src="../../../assets/light/effects/RainbowEffect.gif" width="300">

Diagonal animated rainbow — always-visible default/test effect.

- `speed` — animation BPM (one full hue cycle per beat).

[Tests](../../../tests/unit-tests.md#rainboweffect)

<a id="plasma"></a>

### Plasma 💫🦅 · 2D/3D

<img src="../../../assets/light/effects/PlasmaEffect.gif" width="300">

Summed sine waves on orthogonal + diagonal axes; large rolling blobs (3D on volumetric layouts).

- `bpm` — roll speed.
- `scale_x` / `scale_y` — blob size on each axis (larger = bigger, calmer blobs, lower spatial frequency).
- `hue_shift` — rotate the palette index.

[Tests](../../../tests/unit-tests.md#plasmaeffect)

<a id="spiral"></a>

### Spiral 💫🦅 · 2D

<img src="../../../assets/light/effects/SpiralEffect.gif" width="300">

Rotating spiral from angle + distance (`atan2_8`/`dist8`).

- `bpm` — rotation speed.
- `twist` — how tightly the arm winds (hue gain per unit of distance).
- `hue_shift` — rotate the palette index.

[Tests](../../../tests/unit-tests.md#spiraleffect)

<a id="distortionwaves"></a>

### DistortionWaves 💫 · 2D

Two interfering sine waves beat against each other into a moiré colour field.

- `freq_x` / `freq_y` — horizontal/vertical wave frequency (1–8).
- `speed` — animation rate (0 = frozen).

[Tests](../../../tests/unit-tests.md#distortionwaveseffect)

<a id="metaballs"></a>

### Metaballs 💫🦅 · 2D

<img src="../../../assets/light/effects/MetaballsEffect.gif" width="300">

`count` blobs orbit via integer sin/cos; metaball field per pixel — bright HSV merge/split.

- `bpm` — orbit speed.
- `radius` — blob influence radius.
- `count` — number of orbiting balls (1–8).
- `hue_shift` — rotate the palette index.

[Tests](../../../tests/unit-tests.md#metaballseffect)

<a id="lavalamp"></a>

### LavaLamp 💫🦅 · 2D

<img src="../../../assets/light/effects/LavaLampEffect.gif" width="300">

Three slow blobs through a black→red→orange→yellow→white ramp — atmospheric lava look.

- `bpm` — blob drift speed.
- `radius` — blob influence radius.
- `intensity` — field gain into the black→red→orange→yellow→white ramp.

[Tests](../../../tests/unit-tests.md#spiraleffect)

<a id="particles"></a>

### Particles 💫🦅 · 2D

<img src="../../../assets/light/effects/ParticlesEffect.gif" width="300">

A swarm of drifting particles with persistent fading trails.

- `count` — number of particles (1–255).
- `speed` — drift velocity.
- `fade` — trail persistence (higher = longer tails).
- `hue_shift` — rotate every particle's hue.

[Tests](../../../tests/unit-tests.md#particleseffect)

<a id="rings"></a>

### Rings 💫🦅 · 2D

<img src="../../../assets/light/effects/RingsEffect.gif" width="300">

Expanding concentric rings from random centres, additive overlap (calm defaults).

- `count` — simultaneous rings (1–8 active).
- `speed` — expansion rate.
- `thickness` — ring band width.
- `hue_shift` — rotate every ring's hue.

[Tests](../../../tests/unit-tests.md#spiraleffect)

<a id="ripples"></a>

### Ripples 💫🟦🦅 · 3D

<img src="../../../assets/light/effects/RipplesEffect.gif" width="300">

Distance-from-centre sets a per-column wave phase; the lit surface ripples like water.

- `speed` — wave animation rate (0 = frozen, 99 = fast).
- `interval` — wavefront spacing (low = tight rings, high = wide).

[Tests](../../../tests/unit-tests.md#spiraleffect)

<a id="lines"></a>

### Lines 💫 · —

<img src="../../../assets/light/effects/LinesEffect.gif" width="300">

Sweeps axis-aligned planes in sync; red/green/blue name the X/Y/Z axis — a preview-orientation test pattern.

- `speed` — sweep BPM.
- `axis` — which plane sweeps (`all`, `x (red)`, `y (green)`, `z (blue)`).

## MoonModules effects

<a id="gameoflife"></a>

### GameOfLife 💫🌙 · 2D/3D

Conway's cellular automaton generalised to 2D/3D: selectable rulesets (+ custom `B#/S#`), cells that inherit a neighbour's palette colour on birth, optional green→red age colouring, a dead-cell blur fading toward the background colour, toroidal `wrap`, a 1.5 s settle pause, and 3-CRC stasis self-respawn (R-pentomino/glider) when the board goes static.

- `backgroundColorR` / `backgroundColorG` / `backgroundColorB` — the colour dead cells fade toward (0–255 each).
- `ruleset` — the birth/survive rule (Conway, HighLife, InverseLife, Maze, Mazecentric, DrighLife, or Custom).
- `customRuleString` — a custom `B#/S#` rule, read only when `ruleset` = Custom.
- `GameSpeed (FPS)` — generation rate (0–100, 100 = uncapped).
- `startingLifeDensity` — % of cells alive at start (10–90).
- `mutationChance` — % chance a newborn gets a random colour (0–100).
- `wrap` — toroidal edges (cells wrap around).
- `disablePause` — skip the 1.5 s settle pause between boards.
- `colorByAge` — green→red aging instead of inheriting a neighbour's palette colour.
- `infinite` — respawn on stasis (R-pentomino/glider) instead of resetting.
- `blur` — dead-cell fade strength toward the background colour.

[Tests](../../../tests/unit-tests.md#gameoflifeeffect)

<a id="geq3d"></a>

### GEQ3D 💫🌙📊 · 2D

A 3D-perspective graphic equaliser: audio bands rise as bars with faked depth, their side/top lines drawn toward a "projector" vanishing point (sweeping left↔right) and shortened by `depth`. Bands left of the projector are painted right-to-left, bands right of it left-to-right; per-face darkening (side/top/front) and optional `borders`.

- `speed` — projector sweep rate (1–10, higher = faster).
- `frontFill` — bar front-face fill strength (0–255).
- `horizon` — vanishing-point row the projector sits on.
- `depth` — how far the side/top perspective lines reach toward the projector.
- `numBands` — bands shown (2–16, fewer = wider bars).
- `borders` — outline each bar.

[Tests](../../../tests/unit-tests.md#geq3deffect)

<a id="paintbrush"></a>

### PaintBrush 💫🌙📊 · 3D

Audio-reactive brush strokes: lines whose 3D endpoints oscillate on the beat (`beatsin8`, audio-band timebase), each stroke shortened to a band-magnitude length so the moving tip sweeps a curve over the fading field.

- `oscillatorOffset` — phase-spread between the oscillating endpoints (0–16).
- `numLines` — parallel animated strokes (2–255).
- `fadeRate` — background decay per frame (0–128, higher = shorter strokes).
- `minLength` — a stroke draws only if longer than this, so quiet bands stay dark.
- `color_chaos` — per-line random hue vs a per-band gradient.
- `phase_chaos` — random per-frame phase jitter.

[Tests](../../../tests/unit-tests.md#paintbrusheffect)

## WLED effects

<a id="wave"></a>

### Wave 🌊 · 2D

An oscilloscope waveform scrolls across the grid with a fading trail; six selectable shapes.

- `bpm` — travel speed (phase advance per minute).
- `fade` — trail fade per frame (0 = instant clear, 255 = long tail).
- `type` — waveform shape (`Sawtooth`, `Triangle`, `Sine`, `Square`, `Sin3`, `Noise`).

[Tests](../../../tests/unit-tests.md#waveeffect)

## FastLED effects

<a id="fire"></a>

### Fire ⚡️🦅 · 2D

<img src="../../../assets/light/effects/FireEffect.gif" width="300">

Fire2012-style heat field — sparks at the base rise and cool through a black→red→yellow→white ramp; spark count scales with width.

- `cooling` — how fast heat dissipates as it rises (higher = shorter flames).
- `sparking` — chance of a new spark at the base each frame (higher = livelier fire).
- `hue_shift` — rotate the flame's colour ramp.

[Tests](../../../tests/unit-tests.md#fireeffect)

<a id="noise"></a>

### Noise ⚡️ · 2D/3D

<img src="../../../assets/light/effects/NoiseEffect.gif" width="300">

Smooth animated value noise; true 3D field on volumetric layouts.

- `scale` — spatial frequency of the field (1–32, higher = finer detail).
- `bpm` — scroll speed (8 noise cells per beat).

[Tests](../../../tests/unit-tests.md#noiseeffect)

## projectMM-native effects

<a id="sine"></a>

### Sine 🌀 · 3D

R/G/B each follow a sine along one axis at 120° phase offset — a glowing, scrolling colour box.

- `frequency` — spatial frequency, waves across the box (1–20).
- `amplitude` — peak brightness (0–255, 255 = full).
- `bpm` — scroll speed.

[Tests](../../../tests/unit-tests.md#sineeffect)

<a id="audiovolume"></a>

### AudioVolume 🔊

A whole-grid VU meter: every light pulses with the mic level, colour indexing the palette by loudness.

- `brightness` — overall brightness ceiling for the VU pulse (1–255).

[Tests](../../../tests/unit-tests.md#audiomodule)

<a id="audiospectrum"></a>

### AudioSpectrum 📊

The 16 mic frequency bands spread across X, each column lit bottom-up by its magnitude.

- `colorMode` — bar colouring: `height` (green base → red top, the VU look) or `per-band` (each column its own hue, the rainbow analyser look).

[Tests](../../../tests/unit-tests.md#audiomodule)

<a id="networkreceive"></a>

### NetworkReceive 📡🌙

Receives lights-over-UDP (Art-Net, E1.31/sACN, DDP) and writes it into the layer — the receive side for Resolume/Madrix/xLights/LedFx.

- `universe_start` — the first incoming universe to map onto the layer (mirrors the sender).
- `channels_per_universe` — bytes each universe maps to (510 = whole RGB lights per universe, the xLights/Falcon convention; 512 for Madrix-style senders that pack pixels across universe boundaries).

[Tests](../../../tests/unit-tests.md#networkreceiveeffect)

**Wire contract:** listens for [Art-Net](https://art-net.org.uk/downloads/art-net.pdf), [E1.31 / sACN](https://tsp.esta.org/tsp/documents/docs/ANSI_E1-31-2018.pdf), and [DDP](http://www.3waylabs.com/ddp/) simultaneously; `universe_start` + `channels_per_universe` map incoming universes onto the layer buffer. The end-to-end pair with [NetworkSendDriver](../drivers/NetworkSendDriver.md).

## Source

- [AudioSpectrumEffect.h](../../../../src/light/effects/AudioSpectrumEffect.h)
- [AudioVolumeEffect.h](../../../../src/light/effects/AudioVolumeEffect.h)
- [DistortionWavesEffect.h](../../../../src/light/effects/DistortionWavesEffect.h)
- [FireEffect.h](../../../../src/light/effects/FireEffect.h)
- [GEQ3DEffect.h](../../../../src/light/effects/GEQ3DEffect.h)
- [GameOfLifeEffect.h](../../../../src/light/effects/GameOfLifeEffect.h)
- [LavaLampEffect.h](../../../../src/light/effects/LavaLampEffect.h)
- [LinesEffect.h](../../../../src/light/effects/LinesEffect.h)
- [MetaballsEffect.h](../../../../src/light/effects/MetaballsEffect.h)
- [NetworkReceiveEffect.h](../../../../src/light/effects/NetworkReceiveEffect.h)
- [NoiseEffect.h](../../../../src/light/effects/NoiseEffect.h)
- [PaintBrushEffect.h](../../../../src/light/effects/PaintBrushEffect.h)
- [ParticlesEffect.h](../../../../src/light/effects/ParticlesEffect.h)
- [PlasmaEffect.h](../../../../src/light/effects/PlasmaEffect.h)
- [RainbowEffect.h](../../../../src/light/effects/RainbowEffect.h)
- [RingsEffect.h](../../../../src/light/effects/RingsEffect.h)
- [RipplesEffect.h](../../../../src/light/effects/RipplesEffect.h)
- [SineEffect.h](../../../../src/light/effects/SineEffect.h)
- [SpiralEffect.h](../../../../src/light/effects/SpiralEffect.h)
- [WaveEffect.h](../../../../src/light/effects/WaveEffect.h)
