# hpwit/ESPLiveScript — monthly activity digest

What landed on [hpwit/ESPLiveScript](https://github.com/hpwit/ESPLiveScript), month by month. External-context reference — a factual log of a friend repo's activity, not projectMM's own history or roadmap. Newest month on top. The reusable prompt that generates these lives in [README.md](README.md).

The library: Yves Bazin's (hpwit) C-like compiler/interpreter for the ESP32 — small scripts (e.g. LED effects) compiled and run live on-device without a full recompile-and-flash cycle. Summarised via the GitHub commits API.

**Branch note:** `main` is quiet (last touched June 2025), but this repo develops on a long series of **version branches** (`v2`…`v4.3`, plus `vjson`/`vjson2`/`vdrop`/`memory*`), and that's where the recent work is. The activity below is read across those branches, not just `main`.

## February 2026

*Latest in-window activity, on the `vjson2` branch.*

- **JSON exchange refinements** (`vjson2`) — continued work on the script↔host JSON path begun in mid-2025, plus a code-refactoring cleanup pass.

## March 2025 (and earlier 2025, on the `v4.x` branches)

*The `v4` → `v4.3` line, Jan–Mar 2025.*

- **New HSV function** with a FastLED-style example; register-variable handling and array management improved for speed.
- Documentation + interrupts work; assorted bug corrections. This is the most substantial recent feature stretch.

## Mid-2025 baseline (`main`, June 2025, tag `1.3.2`)

- A **JSON option/return path** (`enjoy json`) so scripts can exchange JSON values with the host firmware; interpreter speedups; a "does this function exist?" pre-call check; `void*` parameter support.

*No `main`-branch commits fall in the Sept 2025 – May 2026 window; the in-window work lives on `vjson2`. This file gets a new `## <Month Year>` section as either `main` or the active version branch sees further commits.*
