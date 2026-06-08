# hpwit/I2SClocklessVirtualLedDriver — monthly activity digest

What landed on [hpwit/I2SClocklessVirtualLedDriver](https://github.com/hpwit/I2SClocklessVirtualLedDriver), month by month. External-context reference — a factual log of a friend repo's activity, not projectMM's own history or roadmap. Newest month on top. The reusable prompt that generates these lives in [README.md](README.md).

The library: Yves Bazin's (hpwit) "virtual pins" variant of the I2S clockless driver — drives far more strips than the chip has usable pins by fanning the I2S output through external shift registers. This multiplex technique is the load-bearing idea projectMM's LED-driver analysis singles out (factoring the shift-register multiplex out of the I2S/LCD peripheral code). Summarised via the GitHub commits API, read across all branches (`main`, `integration`, `int2`, `variable`, `hpwit-patch-1`), not just `main`.

## No activity in the digest window (Sept 2025 – May 2026)

No branch has any commits in the window these digests cover; the project went quiet at the end of 2024. Its last active stretch:

- **December 2024** (`variable` / `int2` branches) — variable-driver and integration work; this is the newest commit anywhere in the repo (`variable`, 2024-12-17).
- **November 2024** (`main`, tag `2.1`) — **ESP32-S3 support** added and merged to `main` (2024-11-20).
- **Earlier 2024** — `i2sStop` moved into IRAM (June); ESP-IDF 5 corrections (June); shift-array support and examples (Jan–Oct).

*Nothing newer to report. This file gets a new `## <Month Year>` section the next time any branch sees in-window commits — watch `integration`/`variable`, where the development historically happened before merging to `main`.*
