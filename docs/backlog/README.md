# Backlog — index

The forward-looking half of the docs (the backward-looking half is [`../history/`](../history/)). This folder is **not** present-tense and agents don't read it automatically — only when planning new work. See [CLAUDE.md § Documentation](../../CLAUDE.md) for how `backlog/` and `history/` relate.

## What's here

### The prioritised to-build list

- [backlog.md](backlog.md) — what to build next, grouped by theme (distribution, effects, drivers, modifiers, …). Completed items are removed; the file is deleted when empty.

### Draft module specs

- [moonmodules_draft/](moonmodules_draft/) — specs for modules not yet implemented, split into [core/](moonmodules_draft/core/) and [light/](moonmodules_draft/light/). Selected and promoted to [`../moonmodules/`](../moonmodules/) as each ships, then deleted from the draft.

### Design studies

One-off research documents that informed a future direction, kept for the reasoning rather than as living specs.

- [leddriver-analysis-top-down.md](leddriver-analysis-top-down.md) — reasons from the end goal (driving WS2812-class LEDs from a GPIO pin) toward a generic driver architecture, per-platform implementation, and a testing strategy.
- [leddriver-analysis-bottom-up.md](leddriver-analysis-bottom-up.md) — the companion landscape survey: catalogues the existing LED-driver libraries across ESP32, Teensy, Raspberry Pi, and PC, and recommends a path.
