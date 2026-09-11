# Plan: MoonLive palettes (`.mlp`)

## What this is

A palette that is CODE rather than data. A `.mlp` script fills the 16 active palette entries once
per frame, so a palette can respond to audio, drift over time, or compute from an algorithm.
A gradient stop list cannot express any of that: it is frozen the moment it is saved.

The idea is MoonLight's, and it is a good one. Their implementation is not the part to copy.

## Why our engine makes this cheap, where MoonLight's does not

MoonLight runs a scripted palette through hpwit's LiveScript in a **separate FreeRTOS task**, at
8 KB of stack per script plus an on-device compiler, which is why their live scripts are documented
as needing a PSRAM-class board. Changing sixteen colors should not cost that.

We already JIT to native code and run every binding INLINE on the render thread. A `.mlp` is the
fifth binding beside effect (`.mle`), layout (`.mll`), modifier (`.mlm`) and service (`.mls`): the
same `MoonLiveScript` member, the same `sync()` on prepare, the same file picker. The pattern is
established four times over, so this adds a role rather than a mechanism.

## Cost, which is what makes the design work

A palette is **16 entries, once per frame**. Not per light. A 16x16 grid runs an effect body 256
times a frame; a `.mlp` runs 16 iterations of a loop, whatever the rig size. So the hot-path cost is
independent of light count, which is the property that makes running it in the render path
reasonable at all.

Measure it anyway on the classic ESP32, where the margin is thinnest: the acceptance number is that
a scripted palette costs less than a scripted EFFECT on the same board, since the effect is the
thing already considered affordable.

## The shape

```c
class Fire {
  byte speed = 40;

  void defineControls() {
    addControl("speed", speed, 1, 120); // how fast the fire breathes
  }

  void tick() {
    for (int i = 0; i < 16; i = i + 1) {
      int heat = beatsin(speed, t + i * 16, 255);
      setPalEntryHSV(i, div(heat, 8), 255, heat);
    }
  }
}
```

- **Entry point**: `tick`, the same name an effect uses. A palette and an effect are both
  per-frame producers, so the name means the same thing in both, and a script author moving between
  them learns nothing new.
- **Two new builtins**: `setPalEntry(i, r, g, b)` and `setPalEntryHSV(i, h, s, v)`. Both are
  MoonLight's names, kept deliberately: a MoonLight user's palette script should read as familiar.
  Index bounded to 0..15, out of range ignored, so a script cannot write past the palette.
- **Everything else it already has**: `beat`, `beatsin`, `noise`, `sin`, `random16`, and crucially
  `audioBand()` / `audioBeat()`. The audio vocabulary is where "a palette that reacts" actually
  lands, and it costs nothing to expose because the common builtins are already shared.
- **`addControl` works**, as it does in every other binding, so a scripted palette is configurable
  from its card without editing the source.

## Where it plugs in

`Palettes::active_` is already a single 48-byte global (`Palette`, 16 x RGB) that every effect
samples through `colorFromPalette()`. `setActiveDirect()` already exists. So a `.mlp` fills those
48 bytes where `fromBuiltin()` otherwise would: **no effect changes, and the sampling path does not
change at all.**

The `palette` control on `Drivers` gains the discovered `.mlp` files after the built-ins, exactly as
the effect list already appends scripted effects. One selector, one mental model, and a scripted
palette is picked the same way a built-in is.

## The three decisions, and their answers

**1. Tearing.** Today `Palettes::setActive()` runs on a control change, so it never overlaps a
frame. A per-frame script does. The script therefore fills a SCRATCH `Palette` and the 48 bytes are
assigned once at the end, so an effect samples either the old palette or the new one, never half of
each. Cheap, and it removes the whole class of problem rather than narrowing the window.

**2. Where the per-frame call sits.** Ahead of the effect pass, so every effect in the frame sees
the same palette. `Drivers::tick()` owns the palette control today, which makes it the honest owner
of the per-frame refresh too.

**3. Ordering against the perceptual curve.** A `.mlp` writes entries in LINEAR light; the CIE curve
is applied downstream in the driver's output LUT. This is already correct and needs no code, but it
belongs in the docs: a palette author who pre-compensates would double-correct, which is the same
trap the curve work just documented one layer down.

## Steps

1. **The role**: `.mlp` extension, `kPaletteExt`, `kPalettePick`, a template, and the catalog glob.
   Four globs need it (CMakeLists, `catalog_scripts.py` x2, `catalog_scripts.cmake`), which is the
   step that was missed when `.mls` was added, so it is called out here rather than discovered.
2. **The builtins**: `setPalEntry` / `setPalEntryHSV` in the light table, writing through a sink
   installed for exactly one `run()`, the same bracket `MoonLiveModifier` uses for its coordinate
   sink. A script cannot reach the palette outside its own tick.
3. **The binding**: `MoonLivePalette`, holding a `MoonLiveScript` and filling a scratch `Palette`.
4. **The wiring**: the `palette` select gains the `.mlp` files; `Drivers::tick()` runs the active
   one per frame; the scratch is assigned into `Palettes::active_`.
5. **Two shipped scripts**, because a feature with no example is a feature nobody finds: one
   algorithmic (a drifting fire) and one AUDIO-REACTIVE, which is the case that justifies the whole
   design over a stop list.

## Tests

- Unit: a `.mlp` fills all 16 entries; an out-of-range index writes nothing; a script with no `tick`
  leaves the palette untouched; a broken script leaves the LAST GOOD palette rather than black
  (the same degrade-visibly rule the other bindings follow).
- Scenario: a scripted palette drives a real effect end to end, which is the integration the unit
  tests cannot reach.
- Bench: the audio-reactive palette on a board with a mic, which is the only way to judge whether
  the idea actually looks good.
- Cost: the per-frame tick measured on a classic ESP32 against a scripted effect on the same rig.

## Backlogged from the build

**A swatch for every picker row, not just palettes.** The shared picker learned an optional swatch
column for palettes (a row carrying `colors` paints a gradient; every other list is untouched
because it carries none). The same column could preview an EFFECT or a script, which would make the
picker readable at a glance rather than a list of names.

What to draw is the open question, and it is why this is backlogged rather than built: an effect has
no single color, and a still frame of a moving effect may be its least representative moment. The
candidates worth trying are a strip of the effect's first rendered row, its palette usage, or a
tiny animated preview once the picker can afford one. Worth prototyping against real effects before
committing to any of them, because the wrong preview is worse than no preview.

## Not in this plan

- **Crossfading between palettes.** WLED blends over a transition and it is the visible quality
  difference, but it is orthogonal: it improves built-in palette CHANGES, scripted or not, and
  belongs in its own change.
- **A stop-list editor.** WLED's `cpal.htm` is a good visual editor for FROZEN palettes. It solves a
  different problem from this one and is worth its own decision later.
