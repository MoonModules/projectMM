# MoonLiveModifier

A **modifier written as a live script**: the coordinate transform that decides where each light sits in the pattern, authored as text on a running device instead of compiled in as a C++ class. Same [MoonLive](MoonLiveEffect.md) engine as a scripted effect, pointed at a different job.

A [modifier](modifiers.md) reshapes how a Layer's output maps onto the physical lights — mirror it, shift it, swap its axes. Each hand-written one is a class, a rebuild and a reflash. A scripted one is a line of text, applied as you type.

<img src="../../assets/light/MoonLiveModifier.png" width="300" alt="MoonLiveModifier">

## Writing one

The script transforms **one coordinate**. It does not loop, because the Layer already does: it calls the script once per physical light while it builds its mapping.

```c
setXYZ(0, width - 1 - x, y, z);   // mirror along x
setXYZ(0, y, x, z);               // swap the axes
setXYZ(0, x + 4, y, z);           // shift by four
setXYZ(0, (width - 1 - x) * 2, y, z);   // mirror, then stretch
```

`setXYZ(index, x, y, z)` writes the transformed position, mirroring `setRGB(index, r, g, b)`. The index is the destination slot: today the script is handed a single coordinate, so it is always `0`.

### What a script can read

| name | is |
|---|---|
| `x`, `y`, `z` | the position of the light being folded |
| `width`, `height`, `depth` | the box that position lives in |

`width` matters more than it looks. A mirror written against a fixed `255` sends every light of a 16-wide grid far outside the grid, the Layer discards each one as out of bounds, and the fixture goes black — with no error anywhere, because the script itself ran perfectly.

### Seeing inside a script

`print(v)` writes a value to the serial log and returns it, so it wraps any part of an expression without changing the result:

```c
setXYZ(0, print(width - 1 - x), y, z);
```

This is the only view into a running script. Prints are capped at a short burst per run: the script executes once per light, so an uncapped print on a 16,384-light wall would emit 16,384 blocking serial writes per rebuild.

## Limits

**A coordinate is a byte, so an axis spans 0..255.** A position outside that range is passed through untransformed rather than wrapped — wrapping would silently place the light somewhere it is not.

**A script cannot resize the logical box.** A modifier has two hooks: one reshapes the box once per rebuild, one folds each coordinate. A script drives only the second, so transforms that keep the box the same size work, and ones that halve it (the way the built-in [Mirror](modifiers.md#mirror) does) need the compiled modifier.

**The grammar is arithmetic over calls** — `+`, `-`, `*`, parentheses, and the usual precedence. Division, `if` and `for` are not in the language yet.

## Controls

| control | what it does |
|---|---|
| `source` | the script; editing it recompiles and re-maps live |

Editing the script asks the Layer to rebuild its mapping, so a change is visible immediately. A script that fails to compile leaves the previous mapping alone, shows the parse error on the module, and the device keeps rendering.

Detail: [technical](moxygen/MoonLiveModifier.md)
