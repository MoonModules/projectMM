# MoonLight effect inventory (migration reference)

The full set of MoonLight effects to migrate, grouped by **origin library** (the doc-page split: `effects_<library>.md`), with audio/3D markers. Source: [MoonLight effects.md](https://github.com/MoonModules/MoonLight/blob/main/docs/moonlight/effects.md) + the `E_*.h` source files — studied for *behaviour*, reimplemented fresh per the migration plan's *Industry standards, our own code* rule. This reference feeds the [migration plan's](../history/plans/Plan-20260630%20-%20MoonLight%20migration%20(multi-stage).md) Stage-3 batches; it is *what to build*, not a copy of how.

**Markers:** ♫ / ♪ audio-reactive · 🧊 native 3D. **Status:** ✅ already in projectMM · ⬜ to migrate.

## MoonLight library → `effects_moonlight.md`

| Effect | Markers | Status | Notes |
|---|---|---|---|
| Solid | | ⬜ | background/base colour |
| Lines | | ✅ | LinesEffect |
| Frequency Saws | ♫ | ⬜ | audio (Stage 3d) |
| Moon Man | | ⬜ | |
| Particles | 🧊 | ✅ | ParticlesEffect |
| Rainbow | | ✅ | RainbowEffect |
| Random | | ⬜ | |
| Ripples | 🧊 | ✅ | RipplesEffect |
| Rubik's Cube | 🧊 | ⬜ | 3D |
| Scrolling Text | | ⬜ | needs a font/glyph blitter (Stage 3e) |
| Sinus | | ✅ | SineEffect |
| Sphere Move | 🧊 | ⬜ | 3D; pairs with SphereLayout |
| StarField | | ⬜ | |
| Praxis | | ⬜ | |
| Wave | | ✅ | WaveEffect |
| Fixed Rectangle | | ⬜ | |
| Star Sky | | ⬜ | |

## MoonModules library → `effects_moonmodules.md`

| Effect | Markers | Status | Notes |
|---|---|---|---|
| GEQ 3D | ♫ | ⬜ | audio + 3D |
| PaintBrush | ♫ 🧊 | ⬜ | audio + 3D |
| Game Of Life | 🧊 | ✅⚠️ | **re-port** — current version flagged not faithful (migration Stage 1 proof) |

## WLED library → `effects_wled.md`

| Effect | Markers | Status | Notes |
|---|---|---|---|
| Blackhole | | ⬜ | |
| Bouncing Balls | | ⬜ | physics (Stage 3c) |
| Blurz | ♫ | ⬜ | audio |
| Distortion Waves | | ✅ | DistortionWavesEffect |
| Frequency Matrix | ♪ | ⬜ | audio |
| GEQ | ♫ | ⬜ | audio |
| Lissajous | | ⬜ | geometric (Stage 3a) |
| Noise 2D | | ✅ | NoiseEffect |
| Noise Meter | ♪ | ⬜ | audio |
| PopCorn | ♪ | ⬜ | physics + audio |
| Waverly | ♪ | ⬜ | audio |

## Moving-head library → `effects_movingheads.md` (Stage 5, DMX fixtures)

| Effect | Markers | Status |
|---|---|---|
| Troy1 Color / Troy1 Move / Troy2 Color / Troy2 Move | ♫ | ⬜ |
| FreqColors · Wowi Move · Ambient Move | ♫ | ⬜ |

## projectMM-native (no external origin) → `effects_projectmm.md`

Already in projectMM, our own (not from a MoonLight library — kept here so the inventory is complete):
AudioSpectrumEffect ♫, AudioVolumeEffect ♫, FireEffect, GlowParticlesEffect, LavaLampEffect, MetaballsEffect, NetworkReceiveEffect, PlasmaEffect, PlasmaPaletteEffect, RingsEffect, SpiralEffect, CheckerboardEffect.

*(Several have a MoonLight/WLED lineage in their prior-art notes; "origin" here is the page they'll file under — settle per-effect at migration time, per the [folder-structure decision](folder-structure-proposal.md): the page is the primary-steward bucket, the `tags()` emoji carries full lineage.)*

## Tally

47 MoonLight-listed effects. **Already covered: ~7** (Lines, Particles, Rainbow, Ripples, Sinus, Wave, Distortion Waves, Noise 2D — direct equivalents) + GoL (re-port). **To migrate: ~38**, of which ~14 are audio (Stage 3d) and 7 are moving-head (Stage 5). The non-audio, non-moving-head remainder (~17) are the Stage 3a/b/c batches — the bulk of the parity work the [rename gate](rename-to-moonlight.md#must--the-rename-is-a-downgrade-without-these) needs.
