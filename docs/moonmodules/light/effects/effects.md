# Effects

Every effect, one block each: its preview, what it does, and what each control means — together. An effect writes per-pixel colour into its [Layer](../Layer.md)'s buffer each tick; [modifiers](../modifiers/modifiers.md) reshape the result and a [driver](../drivers/) sends it out. Effects that name an index colour read the global palette (the `palette` control on [Drivers](../Drivers.md)) via `colorFromPalette`. Each block's emoji are its `tags()` (origin/creator/audio — see the [tag emoji legend](../../../architecture.md#tag-emoji-legend)); **Dim** is its native axes ([Layer](../Layer.md) extrudes a lower-dim effect onto a bigger grid). Effects are grouped into sections by origin, and each block carries that effect's preview, behaviour, and control descriptions together. (For how this page maps to the source/asset folders, see the [folder-structure decision](../../../backlog/folder-structure-proposal.md).)

**Jump to:** [MoonLight](#moonlight-effects) · [MoonModules](#moonmodules-effects) · [WLED](#wled-effects) · [FastLED](#fastled-effects) · [projectMM-native](#projectmm-native-effects)

## MoonLight effects

<a id="distortionwaves"></a>

### DistortionWaves 💫 · 2D

Two interfering sine waves beat against each other into a moiré colour field.

- `freq_x` / `freq_y` — horizontal/vertical wave frequency (1–8).
- `speed` — animation rate (0 = frozen).

[Tests](../../../tests/unit-tests.md#distortionwaveseffect)

<a id="fixedrectangle"></a>

### FixedRectangle 💫 · 3D

A solid colour filling a positioned box within the grid, with an optional alternating-white checker on the box's pixels.

- `red` / `green` / `blue` / `white` — the box colour.
- `X position` / `Y position` / `Z position` — the box's origin corner.
- `Rectangle width` / `Rectangle height` / `Rectangle depth` — the box extent on each axis.
- `alternateWhite` — alternate box pixels to white in a checker pattern.

[Tests](../../../tests/unit-tests.md#fixedrectangleeffect)

<a id="freqsaws"></a>

### FreqSaws 💫📊 · 2D

Audio-reactive sawtooth waves: each column maps to a frequency band whose magnitude drives a per-band oscillator speed, so louder bands sweep their sawtooth up the column faster, with three phase methods.

- `fade` — background decay per frame.
- `increaser` — how fast a band's speed ramps up with its magnitude.
- `decreaser` — how fast a silent band's speed decays.
- `bpmMax` — ceiling on a band's oscillation speed.
- `invert` — flip alternate columns vertically.
- `keepOn` — keep oscillating even when a band is silent.
- `method` — phase model (`Chaos`, `Chaos fix`, `BandPhases`).

[Tests](../../../tests/unit-tests.md#freqsawseffect)

<a id="lavalamp"></a>

### LavaLamp 💫🦅 · 2D

<img src="../../../assets/light/effects/LavaLampEffect.gif" width="300" alt="LavaLamp effect preview">

Three slow blobs through a black→red→orange→yellow→white ramp — atmospheric lava look.

- `bpm` — blob drift speed.
- `radius` — blob influence radius.
- `intensity` — field gain into the black→red→orange→yellow→white ramp.

[Tests](../../../tests/unit-tests.md#spiraleffect)

<a id="lines"></a>

### Lines 💫 · —

<img src="../../../assets/light/effects/LinesEffect.gif" width="300" alt="Lines effect preview">

Sweeps axis-aligned planes in sync; red/green/blue name the X/Y/Z axis — a preview-orientation test pattern.

- `speed` — sweep BPM.
- `axis` — which plane sweeps (`all`, `x (red)`, `y (green)`, `z (blue)`).

<a id="metaballs"></a>

### Metaballs 💫🦅 · 2D

<img src="../../../assets/light/effects/MetaballsEffect.gif" width="300" alt="Metaballs effect preview">

`count` blobs orbit via integer sin/cos; metaball field per pixel — bright HSV merge/split.

- `bpm` — orbit speed.
- `radius` — blob influence radius.
- `count` — number of orbiting balls (1–8).
- `hue_shift` — rotate the palette index.

[Tests](../../../tests/unit-tests.md#metaballseffect)

<a id="particles"></a>

### Particles 💫🦅 · 2D

<img src="../../../assets/light/effects/ParticlesEffect.gif" width="300" alt="Particles effect preview">

A swarm of drifting particles with persistent fading trails.

- `count` — number of particles (1–255).
- `speed` — drift velocity.
- `fade` — trail persistence (higher = longer tails).
- `hue_shift` — rotate every particle's hue.

[Tests](../../../tests/unit-tests.md#particleseffect)

<a id="plasma"></a>

### Plasma 💫🦅 · 2D/3D

<img src="../../../assets/light/effects/PlasmaEffect.gif" width="300" alt="Plasma effect preview">

Summed sine waves on orthogonal + diagonal axes; large rolling blobs (3D on volumetric layouts).

- `bpm` — roll speed.
- `scale_x` / `scale_y` — blob size on each axis (larger = bigger, calmer blobs, lower spatial frequency).
- `hue_shift` — rotate the palette index.

[Tests](../../../tests/unit-tests.md#plasmaeffect)

<a id="praxis"></a>

### Praxis 💫 · 2D

An algorithmic palette pattern driven by two beat oscillators (a macro and a micro mutator) whose frequencies and ranges reshape the hue field over time.

- `macroMutatorFreq` / `macroMutatorMin` / `macroMutatorMax` — the coarse mutator's beat frequency and its oscillation range.
- `microMutatorFreq` / `microMutatorMin` / `microMutatorMax` — the fine mutator's beat frequency and range.

[Tests](../../../tests/unit-tests.md#praxiseffect)

<a id="rainbow"></a>

### Rainbow 💫 · 2D

<img src="../../../assets/light/effects/RainbowEffect.gif" width="300" alt="Rainbow effect preview">

Diagonal animated rainbow — always-visible default/test effect.

- `speed` — animation BPM (one full hue cycle per beat).

[Tests](../../../tests/unit-tests.md#rainboweffect)

<a id="random"></a>

### Random 💫 · 3D

Lights one random light per frame in a random palette colour over a fading background — a sparse, palette-tinted sparkle.

- `fade` — how fast prior sparkles fade to black.

[Tests](../../../tests/unit-tests.md#randomeffect)

<a id="rings"></a>

### Rings 💫🦅 · 2D

<img src="../../../assets/light/effects/RingsEffect.gif" width="300" alt="Rings effect preview">

Expanding concentric rings from random centres, additive overlap (calm defaults).

- `count` — simultaneous rings (1–8 active).
- `speed` — expansion rate.
- `thickness` — ring band width.
- `hue_shift` — rotate every ring's hue.

[Tests](../../../tests/unit-tests.md#spiraleffect)

<a id="ripples"></a>

### Ripples 💫🟦🦅 · 3D

<img src="../../../assets/light/effects/RipplesEffect.gif" width="300" alt="Ripples effect preview">

Distance-from-centre sets a per-column wave phase; the lit surface ripples like water.

- `speed` — wave animation rate (0 = frozen, 99 = fast).
- `interval` — wavefront spacing (low = tight rings, high = wide).

[Tests](../../../tests/unit-tests.md#spiraleffect)

<a id="rubikscube"></a>

### RubiksCube 💫🧊 · 3D

A 3D Rubik's Cube projected onto the volume: it scrambles, then plays its solution back one turn at a time, the six faces in their standard colours.

- `turnsPerSecond` — how fast the cube turns.
- `cubeSize` — the cube order (2×2 up to 8×8).
- `randomTurning` — turn endlessly at random instead of scramble-then-solve.

[Tests](../../../tests/unit-tests.md#rubikscubeeffect)

<a id="solid"></a>

### Solid 💫 · 3D

A flat fill with five colour modes: a plain RGB(W) colour, the active palette spread across the lights, an RMS-averaged single palette colour, or the palette banded along the grid's rows or columns.

- `red` / `green` / `blue` / `white` — the flat colour in `RGB(W)` mode (ignored in the palette modes).
- `brightness` — scales the flat and palette-spread output.
- `colorMode` — `RGB(W)`, `Palette` (spread across the lights), `Palette avg` (RMS mean of the palette), `Palette rows`, `Palette cols` (palette banded along that axis).
- `minRGB` — in the band modes, drops palette entries whose every channel is below this floor.
- `randomColors` — in the band modes, deterministically shuffles the surviving palette entries.

[Tests](../../../tests/unit-tests.md#solideffect)

<a id="spheremove"></a>

### SphereMove 💫🧊 · 3D

A hollow spherical shell that bounces through the 3D volume, its surface coloured from the palette, leaving no trail.

- `speed` — how fast the sphere moves through the volume.

[Tests](../../../tests/unit-tests.md#spheremoveeffect)

<a id="spiral"></a>

### Spiral 💫🦅 · 2D

<img src="../../../assets/light/effects/SpiralEffect.gif" width="300" alt="Spiral effect preview">

Rotating spiral from angle + distance (`atan2_8`/`dist8`).

- `bpm` — rotation speed.
- `twist` — how tightly the arm winds (hue gain per unit of distance).
- `hue_shift` — rotate the palette index.

[Tests](../../../tests/unit-tests.md#spiraleffect)

<a id="starfield"></a>

### StarField 💫 · 2D

A perspective starfield: stars approach the viewer from a vanishing point, brightening as they near, then respawn at depth.

- `speed` — how fast stars approach (frame throttle).
- `numStars` — how many stars are active.
- `blur` — motion-trail fade per frame.
- `usePalette` — colour the stars from the palette instead of white.

[Tests](../../../tests/unit-tests.md#starfieldeffect)

<a id="starsky"></a>

### StarSky 💫 · 3D

Twinkling stars at random light positions, each fading in and out independently over a dark background.

- `speed` — fade rate per frame (how fast each star brightens/dims).
- `star_fill_ratio` — how many stars (as a fraction of the light count).
- `usePalette` — colour the stars from the active palette instead of white.

[Tests](../../../tests/unit-tests.md#starskyeffect)

## MoonModules effects

<a id="ant"></a>

### Ant 💫🌙 · 1D

Ants walking a 1D strip with elastic collisions; optionally they gather food (carrying a coloured pixel back) and either pass through or bounce off each other.

- `antSpeed` — how fast the ants move.
- `nrOfAnts` — ant count (scaled by strip length).
- `antSize` — pixels per ant.
- `blur` — spatial blur applied to the strip.
- `gatherFood` — ants pick up and carry food to the ends.
- `passBy` — ants pass through each other instead of colliding.

[Tests](../../../tests/unit-tests.md#anteffect)

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

<a id="geq"></a>

### GEQ 💫🐙📊 · 2D

A flat graphic equaliser: the 16 audio bands rise as vertical bars from the bottom, with optional smoothing between bars, per-bar palette colouring, and falling peak markers.

- `fadeOut` — how fast bars fade each frame.
- `ripple` — falling-peak marker decay.
- `colorBars` — colour each bar from the palette by band instead of by row.
- `smoothBars` — blend neighbouring bands for smoother bar heights.

[Tests](../../../tests/unit-tests.md#geqeffect)

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

<a id="noise2d"></a>

### Noise2D 💫🌙🐙 · 2D

A smoothly drifting value-noise field: each pixel samples 3D noise (grid position × `scale`, time on the Z axis) and indexes the palette directly, giving an organic plasma wash that morphs over time.

- `speed` — how fast the field morphs (time-flow rate).
- `scale` — noise zoom (higher = finer, more detailed).

[Tests](../../../tests/unit-tests.md#noise2deffect)

<a id="pacman"></a>

### PacMan 💫🌙 · 1D

A 1D Pac-Man chase: Pac-Man eats power dots along the strip, ghosts turn blue and flee when a power dot is eaten, blinking as the effect resets.

- `speed` — movement speed.
- `#powerdots` — number of power dots placed along the strip.
- `blinkDistance` — how close to the start ghosts begin blinking back to normal.
- `blur` — spatial blur applied to the strip.
- `#ghosts` — number of ghosts (2–8).
- `dots` — draw the trail of white dots ahead of Pac-Man.
- `smear` — keep a motion smear instead of clearing each frame.
- `compact` — denser dot spacing.

[Tests](../../../tests/unit-tests.md#pacmaneffect)

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

<a id="tetrix"></a>

### Tetrix 💫🌙 · 2D

Falling Tetris-style blocks: each column drops a brick that lands on the growing stack, fills the column, then clears and restarts.

- `speed` — fall speed (0 = randomised per brick).
- `width` — brick height (0 = randomised).
- `oneColor` — one advancing palette colour for all bricks instead of random per-brick colours.

[Tests](../../../tests/unit-tests.md#tetrixeffect)

## WLED effects

<a id="blurz"></a>

### Blurz 🐙📊 · 2D

Audio-reactive blurred dots: one frequency band per frame lights a dot whose position maps to that band (or to the major-peak frequency), then the whole frame is blurred for soft trails.

- `fadeRate` — background decay per frame.
- `blur` — blur strength applied each frame.
- `freqMap` — place the dot by the major-peak frequency instead of scanning bands.
- `geqScanner` — scan the dot across the strip in a GEQ-like sweep.

[Tests](../../../tests/unit-tests.md#blurzeffect)

<a id="bouncingballs"></a>

### BouncingBalls 🐙 · 2D

A row of balls per column bounce under gravity, each losing energy on impact and relaunching when it stops, palette-coloured by ball index over a fading background.

- `grav` — gravity strength (higher = faster fall, snappier bounce).
- `numBalls` — balls per column (1–16).

[Tests](../../../tests/unit-tests.md#bouncingballseffect)

<a id="freqmatrix"></a>

### FreqMatrix 🐙📊 · 1D

A 1D scrolling frequency display: each frame shifts the strip and injects a new pixel at one end whose hue comes from the dominant frequency and whose brightness from the volume.

- `speed` — scroll rate.
- `fx` — sound-effect intensity (scales the injected brightness).
- `lowBin` / `highBin` — the frequency window mapped across the hue range.
- `sensitivity` — input gain (10–100).
- `audioSpeed` — let the volume modulate the scroll speed.

[Tests](../../../tests/unit-tests.md#freqmatrixeffect)

<a id="lissajous"></a>

### Lissajous 🐙 · 2D

A Lissajous curve traced across the grid from two phase-shifted `sin8`/`cos8` sweeps, palette-coloured along its length, with a fading trail.

- `xFrequency` — the x-axis sweep frequency (sets the curve's lobe count).
- `fadeRate` — trail fade per frame.
- `speed` — how fast the curve's phase advances.

[Tests](../../../tests/unit-tests.md#lissajouseffect)

<a id="noisemeter"></a>

### NoiseMeter 🐙📊 · 3D

An audio VU meter rendered as a noise bar: the volume sets how many rows light from the bottom, each row coloured by drifting Perlin noise, filling the full width and depth.

- `fadeRate` — trail decay per frame (200–254).
- `width` — how strongly the volume drives the bar height.

[Tests](../../../tests/unit-tests.md#noisemetereffect)

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

<img src="../../../assets/light/effects/FireEffect.gif" width="300" alt="Fire effect preview">

Fire2012-style heat field — sparks at the base rise and cool through a black→red→yellow→white ramp; spark count scales with width.

- `cooling` — how fast heat dissipates as it rises (higher = shorter flames).
- `sparking` — chance of a new spark at the base each frame (higher = livelier fire).
- `hue_shift` — rotate the flame's colour ramp.

[Tests](../../../tests/unit-tests.md#fireeffect)

<a id="noise"></a>

### Noise ⚡️ · 2D/3D

<img src="../../../assets/light/effects/NoiseEffect.gif" width="300" alt="Noise effect preview">

Smooth animated value noise; true 3D field on volumetric layouts.

- `scale` — spatial frequency of the field (1–32, higher = finer detail).
- `bpm` — scroll speed (8 noise cells per beat).

[Tests](../../../tests/unit-tests.md#noiseeffect)

## projectMM-native effects

<a id="audiospectrum"></a>

### AudioSpectrum 📊

The 16 mic frequency bands spread across X, each column lit bottom-up by its magnitude.

- `colorMode` — bar colouring: `height` (green base → red top, the VU look) or `per-band` (each column its own hue, the rainbow analyser look).

[Tests](../../../tests/unit-tests.md#audiomodule)

<a id="audiovolume"></a>

### AudioVolume 🔊

A whole-grid VU meter: every light pulses with the mic level, colour indexing the palette by loudness.

- `brightness` — overall brightness ceiling for the VU pulse (1–255).

[Tests](../../../tests/unit-tests.md#audiomodule)

<a id="demoreel"></a>

### DemoReel 🎬 · 3D

A demo reel: plays every other registered effect in turn, auto-advancing on a timer, so one Layer cycles the whole library hands-free. It hosts a single live effect at a time (created from the effect registry, rendered into this Layer) and swaps to the next when the interval elapses — new effects are picked up automatically. The `status` line shows which effect is playing (e.g. `playing: Plasma (3/20)`). It never hosts itself, and it plays effects in sequence rather than compositing them (layering is the [Layer](../Layer.md) stack's job).

- `interval` — seconds each effect plays before advancing (1–120).
- `shuffle` — jump to a random next effect instead of registry order.

[Tests](../../../tests/unit-tests.md#demoreeleffect)

<a id="networkreceive"></a>

### NetworkReceive 📡🌙

Receives lights-over-UDP (Art-Net, E1.31/sACN, DDP) and writes it into the layer — the receive side for Resolume/Madrix/xLights/LedFx.

- `universe_start` — the first incoming universe to map onto the layer (mirrors the sender).
- `channels_per_universe` — bytes each universe maps to (510 = whole RGB lights per universe, the xLights/Falcon convention; 512 for Madrix-style senders that pack pixels across universe boundaries).

[Tests](../../../tests/unit-tests.md#networkreceiveeffect)

**Wire contract:** listens for [Art-Net](https://art-net.org.uk/downloads/art-net.pdf), [E1.31 / sACN](https://tsp.esta.org/tsp/documents/docs/ANSI_E1-31-2018.pdf), and [DDP](http://www.3waylabs.com/ddp/) simultaneously; `universe_start` + `channels_per_universe` map incoming universes onto the layer buffer. The end-to-end pair with [NetworkSendDriver](../drivers/NetworkSendDriver.md).

<a id="sine"></a>

### Sine 🌀 · 3D

R/G/B each follow a sine along one axis at 120° phase offset — a glowing, scrolling colour box.

- `frequency` — spatial frequency, waves across the box (1–20).
- `amplitude` — peak brightness (0–255, 255 = full).
- `bpm` — scroll speed.

[Tests](../../../tests/unit-tests.md#sineeffect)

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
