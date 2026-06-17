# Backlog — index

The forward-looking half of the docs (the backward-looking half is [`../history/`](../history/)). This folder is **not** present-tense and agents don't read it automatically — only when planning new work. See [CLAUDE.md § Documentation](../../CLAUDE.md) for how `backlog/` and `history/` relate.

## What's here

### The prioritised to-build list

- [backlog.md](backlog.md) — what to build next, grouped by theme (distribution, effects, drivers, modifiers, …). Completed items are removed; the file is deleted when empty.
- [ui-deferred.md](ui-deferred.md) — UI items not yet in the live [ui.md](../moonmodules/core/ui.md): deferred-to-1.x features, open design questions, and the gap analysis against v1. The backward-looking v1 UI reverse-engineering lives in [history/v1-inventory.md](../history/v1-inventory.md).
- [leddriver-deferred.md](leddriver-deferred.md) — the LED-driver increments (RMT single-strand, multi-pin RMT, LCD_CAM on S3) all shipped; this is what's left and tracked nowhere else: the sigrok flicker test, the core-1 driver task, fuller show error handling, the per-driver buffer window, 16-bit/dither, and moving-head preview.

### In-flight draft specs

A spec for a not-yet-built module can live here as a plain draft `.md` (alongside the design studies below) until the module ships — at which point its final spec is written in [`../moonmodules/`](../moonmodules/) and the draft is deleted. There's no dedicated subfolder or promote step: a draft is just a forward-looking markdown file like the rest of `backlog/`. None are in flight right now (every drafted module has shipped; the former UI draft moved to [ui-deferred.md](ui-deferred.md) and [history/v1-inventory.md](../history/v1-inventory.md)).

### Design studies

One-off research documents that informed a future direction, kept for the reasoning rather than as living specs.

- [leddriver-analysis-top-down.md](leddriver-analysis-top-down.md) — reasons from the end goal (driving WS2812-class LEDs from a GPIO pin) toward a generic driver architecture, per-platform implementation, and a testing strategy.
- [leddriver-analysis-bottom-up.md](leddriver-analysis-bottom-up.md) — the companion landscape survey: catalogues the existing LED-driver libraries across ESP32, Teensy, Raspberry Pi, and PC, and recommends a path.

(The 3-layer installer plan these analyses' sibling produced shipped fully and its deferred items already had homes in [backlog.md](backlog.md), so its file was deleted per [*Mandatory subtraction*](../../CLAUDE.md#process-rules). The installer lives in `docs/install/` + `scripts/build/`; the durable reasoning is in `architecture.md` / `history/decisions.md`.)
