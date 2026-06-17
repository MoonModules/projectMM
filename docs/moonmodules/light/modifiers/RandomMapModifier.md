# RandomMapModifier

A **modifier** that randomly remaps every light to another light — a true 1:1 permutation (every light goes somewhere, no two lights land on the same place, none are dropped) — and reshuffles to a fresh random permutation on a `bpm` timer. The image scrambles into a new arrangement every beat; the *content* is untouched, only *where each pixel lands* changes.

It is a **dynamic** modifier: where a static modifier (Multiply, Checkerboard) shapes the layer's mapping once, RandomMapModifier re-shapes it on a timer.

## Controls

- `bpm` — reshuffles per minute, `0`–`60`, default `6`. `6` ≈ one new permutation every 10 seconds; `60` is the cap (one per second — faster would strobe, and the per-beat work is bounded for that reason). `0` **freezes** the current permutation: a fixed random remap that never changes.

## How it works

The permutation rides the layer's existing LUT. Like [CheckerboardModifier](CheckerboardModifier.md), it leaves the logical box the same size as the physical box (identity dimensions) and maps each logical light to exactly one physical light — here, the light at `perm[index]`, where `perm` is a [Fisher–Yates](https://en.wikipedia.org/wiki/Fisher%E2%80%93Yates_shuffle)-shuffled array of every light index. On each beat the modifier reshuffles `perm` and asks its parent [Layer](../Layer.md) to rebuild the LUT — the same rebuild a control change triggers, so no new mechanism, just a timer-driven trigger. The shuffle uses an integer LCG seeded from a generation counter, so a given generation is reproducible (and unit-testable) while successive beats differ.

The permutation buffer is sized to the light count and (re)allocated only when the grid resizes — never per frame. If that allocation fails, the modifier degrades to identity passthrough (no remap), the same way the LUT degrades; an empty (0×0×0) grid is a no-op. On a **sparse** layout the permutation is over grid-cell indices, so a real light can be remapped onto a cell with no LED (it goes dark); this is acceptable for the current version.

## Cost

Each beat re-runs the layer's LUT rebuild on the render thread — a transient one-frame cost, like a device scan, not a steady per-frame tax. The `bpm`≤60 cap bounds how often this happens. Steady-state tick/FPS between beats is unchanged.

## Prior art

MoonLight has no direct equivalent; the random-remap idea is common in pixel-mapper tools (e.g. shuffle/scramble transitions). The permutation-rides-the-LUT approach and the bpm-accumulator timer are projectMM's own, reusing the [CheckerboardModifier](CheckerboardModifier.md) 1:1 mapping shape and the effect bpm-timer pattern.

## Source

[RandomMapModifier.h](../../../../src/light/modifiers/RandomMapModifier.h)
