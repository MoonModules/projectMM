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
- [leddriver-increment-1-plan.md](leddriver-increment-1-plan.md) — the concrete first-increment plan distilled from the two analyses: RMT/WS2812B on classic ESP32, the unified one-base driver hierarchy (ArtNet + LED + Preview as peer interpreters of the light preset), the platform seam, and the loopback + host-encoder test strategy. Locked product-owner decisions at the top.
- [leddriver-increment-2-plan.md](leddriver-increment-2-plan.md) — the second increment: 2a multi-pin RMT (implemented; classic + S3 via SOC capability constants) and 2b parallel LCD_CAM on the S3 (open: lane count and LEDs-per-lane targets). Locked decisions and the deferred per-driver buffer window at the top.
- [installer-3layer-plan.md](installer-3layer-plan.md) — the cross-cutting plan tying together the MCU → Board → Device config-provenance model, the installer catalogs + picture-based picker, device-side pin injection, shared installer code across the three clients, the classic-ESP32 firmware-variant collapse, and the partition-table dedup. Links the scattered backlog entries it sequences rather than duplicating them; running status per workstream.
