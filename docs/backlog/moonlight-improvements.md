# Effect improvements over MoonLight

Where a migrated projectMM effect **deliberately behaves differently from the MoonLight original** — a
change that *improves* the effect (more correct, smoother, works at more grid sizes, a control that
matches its label) rather than a straight port. The migration mandate is fidelity ("effects look like
MoonLight"), so every intentional divergence is registered here with its reason, so it's a decision on
record, not accidental drift.

Distinct from [moonlight-fidelity-tensions.md](moonlight-fidelity-tensions.md): that log
holds *undecided* fidelity-vs-principle conflicts awaiting a call; this doc holds *decided* improvements
the product owner approved (correctness/UX wins that ship).

Product-owner ruling (2026-07-01): improvements that increase user satisfaction are allowed even when
they diverge from MoonLight — register each here.

| Effect / primitive | MoonLight behaviour | projectMM behaviour | Why it's an improvement |
|---|---|---|---|
| `math8::map8` (audio bar heights) | `lo + scale8(in, hi-lo)` — the input top never reaches `hi`, so a one-step span (bar height 1) collapses to 0 | `lo + in*(hi-lo)/255` — the input top reaches `hi` exactly (FastLED's documented `map8 == map(in,0,255,lo,hi)`) | Audio bars reach full height; a 1-row bar is now possible. Matches FastLED's *documented* semantics. Affects every audio effect's bar heights slightly. |
| **FreqSaws** (band timing on wide panels) | Each band's sawtooth physics advanced once per **column**, so a band spanning K columns integrated K× per frame — it ran and decayed ~K× too fast | Each of the 16 bands integrates exactly **once per frame**; the column loop just draws the cached per-band Y | Animation speed/decay no longer depend on panel width — identical on a 32-wide and a 256-wide grid. Matches WLED's per-band-per-frame physics. |
| **SphereMove** (motion smoothness) | `time_interval` did the first divide as **integer** (`ms/(100-speed)`), so the origin/diameter only advanced when that ticked to a new whole number — visible stutter (~20 updates/s at 60 FPS, worse at low speed) | Full expression in float, so the shell sweeps and breathes smoothly every frame | Smooth motion at all speeds instead of discrete jumps. (MoonLight *intended* float here, so this is also more faithful.) |
| **Lissajous** (thin grids) | A 1-wide or 1-tall grid mapped every sample to coordinate **1**, which clips (only index 0 is valid) → blank | The size-1 axis maps to coordinate **0**, so the figure draws | Produces visible output on degenerate/thin grids instead of nothing; normal grids (both axes ≥2) unchanged. |
| **PaintBrush** (large grids) | Oscillator endpoints truncated into `uint8_t`, so on grids >256 per axis strokes swept only a small low corner; the top band/hue were unreachable (off-by-one) | Oscillators generated 0..255 then scaled to the full grid; loop uses `numLines-1` as the map high bound | Strokes span the full grid at any size and use the full palette/band range. ≤256-per-axis grids: identical pixels. |
| **FixedRectangle** (RGBW white channel) | On RGBW with `alternateWhite`, the **W channel was written on every box cell** (tinting coloured tiles), and left stale when `white==0` | W follows the checker: only white tiles carry `W=white`; coloured tiles clear W to 0 | Coloured tiles render as pure RGB instead of RGB+white; no stale W persists. The checker actually alternates colour vs white. |

<!-- Invisible fixes (not listed — no visible change): GEQ/StarSky/PaintBrush overflow guards on huge
grids, Tetrix 49-day millis wrap, GoL 3D OOB read + colorIndex≠0 marker, RubiksCube float→int (identical
pixels), Solid/NoiseMeter/Blurz comment+robustness. The DemoReel extrude improvement was reverted (crash)
and backlogged — see backlog-light.md. -->

<!-- StarField: the blur control was flagged as inverted, but it matches MoonLight (fadeToBlackBy(blur):
higher = stronger fade = shorter streaks). No code change — the doc comment was corrected to state the
real direction. Not an improvement, just a doc fix. -->
