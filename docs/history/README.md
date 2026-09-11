# History — index

The backward-looking half of the docs (the forward-looking half is [`../backlog/`](../work/future/README.md)). This folder is **not** present-tense and agents don't read it automatically, only when planning new work. See [CLAUDE.md § Documentation](../../CLAUDE.md) for how `history/` and `backlog/` relate.

*Living index; the git log carries exact dates.*

## What's here

Three kinds of document (the friend-repo digests moved out to [`../friend-repos/`](../friend-repos/README.md)):

### Prior-project inventories

One-time surveys of earlier projects, used to decide what to harvest into projectMM. Reference, not maintained.

- [moonlight-inventory.md](../work/past/moonlight-inventory.md): MoonLight (the closest prior art; CSR mapping, layer model, control mechanisms).
- [v1-inventory.md](../work/past/v1-inventory.md): projectMM v1 (release 1.4.0).
- [v2-inventory.md](../work/past/v2-inventory.md): projectMM v2.
- [leddriver-analysis-bottom-up.md](leddriver-analysis-bottom-up.md) / [leddriver-analysis-top-down.md](leddriver-analysis-top-down.md) — the LED-driver design analyses (landscape survey + protocol-first study). The drivers shipped (RMT/MultiPin/Moon/Parlio on a shared base); kept as the how-we-got-there record.
- [shift-register-driver-analysis.md](shift-register-driver-analysis.md) — the 74HCT595 pin-expander design analysis + lab-notebook of the ring's early transport bugs. The expander + streaming ring shipped; §7.5 records what NOT to re-try.

### The plan archive

[`plans/`](../work/past/plans/README.md) holds 89 approved feature plans from before plans became temporary. Under the current rule ([CLAUDE.md § Branch](../../CLAUDE.md#branch)) a plan's text goes into its PR description and the product owner may delete the file once the plan is realized, so nothing new is added here. These files predate that: they follow the older kept-forever convention, with the outcome marked in the filename (`… (shipped).md`, `… (attempted, abandoned).md`, unmarked = never finished). Reference only, and a candidate for the same subtraction the rest of `history/` gets: the merged PRs are the permanent record of what these describe.

### Our own lessons

- [lessons.md](lessons.md): hard-won debugging lessons and gotchas (a bug, its cause, the fix), recorded with the code that proved them and pruned as they are absorbed (the PR-merge carry-forward gate writes here). A lesson that hardened into a *rule* lives in CLAUDE.md / coding-standards.md.

## Cross-repo trends

Reading across the friend-repo digests, the themes the wider ESP32-LED ecosystem converged on over this release cycle (Sept 2025 → June 2026):

- **ESP32-P4 / S3 parallel output.** FastLED poured effort into the PARLIO and LCD_CAM drivers (P4/S3 parallel LED output, big encode speedups); NightDriverStrip added custom RMT output; hpwit's I2SClocklessLedDriver pushed IDF-5.5 + arduino-less-ESP-IDF support for its I2S/LCD DMA driver (the canonical implementation of this technique), and troyhacks ran ESP32-P4 bring-up branches. The frontier is parallel, DMA-driven output on the newer chips.
- **PSRAM strategy is unsettled everywhere.** All four wrestled with PSRAM this cycle — WLED-MM moved preview buffers into PSRAM, NightDriverStrip did a full PSRAM-default reversal (then tuned the threshold), WLED added S3-no-PSRAM builds. Nobody has a clean answer; the cache-disabled-during-flash hazard recurs across repos.
- **Audio-reactive maturing.** FastLED added a silence-gate + ESP-DSP FFT backend; WLED-MM and WLED both refined audio sync and auto-disable-during-realtime; NightDriverStrip modernised its SoundAnalyzer/FFT. Audio-reactive is table stakes now, and the polish is in *not* reacting to noise/silence.
- **The FastLED dependency question.** WLED merged a *full FastLED replacement* (its own color/math); projectMM already made the same call (own color math, no FastLED in core). Two independent projects concluded the dependency wasn't worth it.
- **UI as a firmware-driven consumer.** Both WLED and NightDriverStrip pushed toward "the official UI knows nothing the firmware doesn't publish over the wire" — exactly projectMM's MoonModule-driven, no-hardcoded-knowledge UI principle. Convergent design. NightDriverStrip's **2.0.0** (June 2026) crystallised this: a brand-new web UI, a browser-based installer, and settings (like strip type) moved from compile-time to *runtime-selectable* on the device — the same "reconfigure live, no reflash" direction projectMM builds around.
- **Effect velocity.** WLED and WLED-MM shipped many new effects (PacMan, Color Clouds, Shimmer, the user_fx pack); new effects remain the most visible user-facing output.
- **Display / HDMI output beyond LED strips.** troyhacks ran a cluster of branches probing HDMI video output and large hardware panels (WaveShare 10.1″, M5Stack, ESP32-P4 panels) — driving *displays*, not just addressable strips, off the same firmware.
- **On-device live scripting.** hpwit's ESPLiveScript compiles small C-like effect scripts that run live on the ESP32 with no reflash — a different answer to effect authoring than C++ recompilation or a fixed effect table.

## What these projects do that projectMM doesn't (yet)

Observational: where the landscape is ahead of projectMM. These are *not* commitments; real adoption decisions live in the [`../backlog/`](../work/future/README.md), cross-referenced where one already exists.

- **Parallel multi-strip output on S3/P4** (PARLIO/LCD_CAM, and hpwit's I2S/shift-register drivers) — the direct parallel drivers ship (MultiPin/Moon on LCD_CAM, Parlio on P4, driving up to 16 strands and 12,288+ lights). The shift-register/'595 expander path also ships but is dormant: it works at prime-only geometries yet has a known lapping-ring sparkle at the largest configs, so it stays off by default. See the [LED-driver analysis](leddriver-analysis-top-down.md).
- **Audio-reactive input** — none of projectMM's effects are audio- or motion-reactive yet. The Peripheral role + the Pi-sensor backlog entry are the foundation; the producer→effect wiring is backlog.
- **A guided setup/installer wizard on-device** (NightDriverStrip's Setup Wizard, WLED's installer) — projectMM has the web installer + Improv, but no on-device first-run wizard.
- **A large built-in effect library** — projectMM ships a focused set (concrete-first); the WLED family ships dozens. Breadth is a deliberate non-goal until the core is proven.
- **On-device live effect scripting** (hpwit's ESPLiveScript) — projectMM effects are compiled C++; there's no runtime script path. Not a goal today, noted as a landscape contrast.

## Refreshing

Adding a month or a new friend repo is the [friend-repos](../friend-repos/README.md) workflow, and its prompt lives there. This folder's own documents are records rather than a feed: they change when the thing they record changes.
