# AudioSpectrumEffect

The classic equalizer display: the microphone's **16 frequency bands** (bass → treble) spread across the grid's X axis, each column lighting from the bottom up in proportion to its band's magnitude — a bar graph that dances with the music.

On a grid at least 3 rows tall, the **bottom row is an overall level/volume meter** (a horizontal VU bar lit left-to-right in proportion to `level`) and the spectrum bars sit in the rows above it. A shorter grid uses the full height for the spectrum.

Reads the live frame from [MicModule](../../core/MicModule.md)`::latestFrame()`; no microphone or silence → all bands zero → dark.

## Controls

- `colorMode` — `height` (the default: each bar green at its base ramping to red at the top) or `per-band` (each column a distinct hue across the colour wheel, bass red → treble violet — the rainbow-analyser look).

## Scaling to the grid

The 16 bands map onto whatever the grid width is — column `x` shows band `x * 16 / width`:

- a **16-wide** grid is one column per band,
- a **32-wide** grid gives each band two columns,
- an **8-wide** grid samples every other band,
- a **1-row strip** (height 1) collapses the bars to per-column brightness.

So the analyser fills the surface at any size, including a 0×0 grid (it simply draws nothing).

## Source

[AudioSpectrumEffect.h](../../../../src/light/effects/AudioSpectrumEffect.h)
