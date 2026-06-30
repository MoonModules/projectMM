# WaveEffect

An animated **waveform that scrolls across the grid** — the classic oscilloscope look. Each column plots one point of a moving wave; as the wave's phase advances with time and is delayed per column, the lit points trace a curve that travels sideways, leaving a fading trail. Six waveform shapes are selectable.

## Controls

- `bpm` — travel speed (how fast the wave's phase advances), 0–255.
- `fade` — trail length: how much the previous frame is kept each tick (0 = the wave leaves no tail, 255 = a long persistent trail). Applied as `scale8(trail, fade)` over an own z=0-plane trail buffer (sized off the hot path, like [ParticlesEffect](ParticlesEffect.md)).
- `type` — the waveform shape (a [Select](../../core/Control.md)): `Sawtooth`, `Triangle`, `Sine`, `Square`, `Sin3` (three summed sines — a richer rolling wave), `Noise` (1-D value noise — a jittered band). The shape maps a phase to the wave's y-position each column.

It is a [D2 effect](../../core/architecture.md) — it writes the z=0 plane and `Layer::extrude` duplicates it across z on a 3D layout. Runs at every grid size (a 0-height or sub-3-channel grid is a no-op, never an out-of-bounds read).

## Orientation (which axis is which)

This is a **D2** effect: the waveform sets a **y-position per column** (its shape lives on the **height** axis) and the per-column phase delay scrolls it along **width**. So height is the wave's amplitude axis — a grid needs `height > 1` to show any waveform; on a 1-tall grid (`height == 1`) every point collapses to y=0 and only the colour shows, no wave.

This follows the project's [1D-runs-along-Y convention](../../architecture.md#dimensionality): **to drive a one-dimensional output** (a single strip, or a row of [Hue lights](../drivers/HueDriver.md)) **lay it out as `1 × N` (width 1, height N), not `N × 1`** — the lights map onto the height axis where the wave is drawn. An `N × 1` layout plots a flat line.

## How the travel works

Per column `x`, the wave's phase is `t + x·skew`: `t` advances from `bpm` (an integer accumulator, so a sub-millisecond frame time isn't lost), and the `x·skew` term delays each column so the shape appears to move horizontally. The phase runs through the selected waveform to a y in `[0, height)`; that pixel is lit, and for the discontinuous shapes (sawtooth, square) a vertical segment joins it to the previous column so the line stays connected. The wave's colour cycles slowly over time.

**Palette readiness:** the colour comes from a single `waveColor(index)` seam — today an `hsvToRgb` hue sweep. When palettes land, that one function becomes the palette lookup with no other change.

## Prior art

[MoonLight](https://github.com/MoonModules/MoonLight)'s Wave effect (Ewoud Wijma). The *behaviour* is reproduced — the six waveform types, the per-column phase travel, the time-varying colour, the frame fade — written fresh against projectMM's `EffectBase` and integer primitives (the `sin8` LUT, `scale8`), with the colour routed through a palette-ready seam rather than MoonLight's `ColorFromPalette` (palettes aren't in projectMM yet).

## Source

[WaveEffect.h](../../../../src/light/effects/WaveEffect.h)
