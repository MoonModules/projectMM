# Plan: sweep every page through the documentation standards

**The gate**: a page is done when `vale <page>` reports nothing on the whole file, and the sweep is done when `vale docs/ CLAUDE.md README.md` exits clean. That day, `.github/workflows/prose.yml` drops `filter_mode: added` and checks whole files, and `moondeck/check/check_prose.py` plus `hook_prose.py` are deleted, since their only job was the added-lines scope.

**Regenerate the worklist**, do not edit it by hand: `vale --no-exit --output=line docs/ CLAUDE.md README.md | cut -d: -f1 | sort | uniq -c | sort -rn`. A hand-kept list drifts the day someone forgets it.

**The method**, from the two pages done so far: run Vale on the whole file, place the page in one Diátaxis cell and move what belongs elsewhere, cut what the code or another page already states, then diff the old page's vocabulary against the new to prove no fact was dropped rather than moved. The Linux tutorial lost four facts on the first pass and the diff found them.

## What now enforces the standards

Three things landed with the standards page, so that it holds after this sweep rather than eroding.

- **[Diátaxis](https://diataxis.fr/)**, followed as written. Every page is one of tutorial, how-to, reference or explanation, and the test for a page is the cell it sits in. The nav splits how-to guides from tutorials, and reference pages (MoonCloud, the privacy policy, firmware variants) sit under Reference.
- **Vale**, with the rules as YAML under `.vale/styles/projectMM/`, one file per rule, ported from and replacing the hand-rolled checker's table. Seven rules: em-dash, American spelling, `e.g.` to `such as`, sentence length, negated headings, weasel words, self-reference. `.github/workflows/prose.yml` runs it on every PR, diff-scoped, failing on `error` and annotating the rest.
- **`--strict` on the docs build**, in CI and in the commit gate. A dead link or anchor fails the build. It caught 153 of them during the folder restructure; before this it was off, on a rationale that had gone stale.

## Done (whole file clean)

- `docs/tutorials/installing-on-linux.md`
- `docs/documentation-standards.md`
- `docs/coding-standards.md`

## Also cleared this branch, by deletion rather than rewrite

- `docs/adr/`: 17 records, each already stated as current behavior in `architecture.md` or the standards.
- 92 shipped plans in `docs/work/past/plans/`, whose content the code, the tests and the merged PR carry.
- `docs/history/lessons.md` lost its branch diaries (20,510 to 15,733 words). **Its goal is removal**: each surviving lesson is either a constraint that belongs in the `.h` it guards, a rule that belongs in the standards, or history that belongs in git. Three method lessons already moved to `testing.md` and CLAUDE.md that way.

## Two pages with a decided shape, pending

- `docs/performance.md`: its 234 rows of dated bench numbers overlap `docs/metrics/repo-health.md`, which is generated per commit. The measurements go to `metrics/`, the analysis stays; a hand-kept number next to a generated one is the drift the one-home rule forbids.
- `docs/architecture.md`: explanation, kept whole; reserved for a separate rework.

## Remaining: 55 pages, 2521 findings

Ordered by findings, most first.

| Findings | Page |
|---|---|
| 211 | `docs/history/lessons.md` |
| 204 | `docs/history/leddriver-analysis-bottom-up.md` |
| 173 | `docs/history/shift-register-driver-analysis.md` |
| 164 | `docs/architecture.md` |
| 121 | `docs/moonmodules/light/power-functions.md` |
| 116 | `docs/history/leddriver-analysis-top-down.md` |
| 112 | `docs/performance.md` |
| 95 | `docs/usecases/home-automation.md` |
| 94 | `docs/testing.md` |
| 90 | `docs/work/present/Plan-20260630 - MoonLight migration (multi-stage).md` |
| 89 | `docs/usecases/build-your-own-moonmodules.md` |
| 81 | `docs/moonmodules/light/drivers.md` |
| 77 | `docs/moonmodules/core/system.md` |
| 70 | `docs/building.md` |
| 67 | `docs/moonmodules/light/MoonLiveEffect.md` |
| 66 | `docs/moonmodules/light/layouts.md` |
| 65 | `docs/gettingstarted.md` |
| 53 | `docs/MIGRATING.md` |
| 49 | `CLAUDE.md` |
| 45 | `docs/moonmodules/core/ui.md` |
| 35 | `docs/moonmodules/light/modifiers.md` |
| 33 | `docs/work/present/Plan-20260901 - Input mapping and scripted sensors.md` |
| 30 | `docs/reference/esp32-s31-coreboard.md` |
| 28 | `docs/moonmodules/core/services.md` |
| 26 | `docs/reference/gpio-usage.md` |
| 25 | `docs/tutorials/how-projectmm-works.md` |
| 23 | `docs/moonmodules/light/effects.md` |
| 22 | `docs/reference/mhc-wled-esp32-p4-shield.md` |
| 21 | `docs/moonmodules/light/supporting.md` |
| 21 | `docs/tutorials/generative-effects.md` |
| 20 | `docs/work/present/Plan-20260830 - Two-way control surfaces.md` |
| 16 | `docs/history/README.md` |
| 16 | `docs/moonmodules/core/control.md` |
| 15 | `docs/logging-an-issue.md` |
| 14 | `README.md` |
| 13 | `docs/moonmodules/light/MoonLiveLayout.md` |
| 12 | `docs/tutorials/panel-cards.md` |
| 12 | `docs/work/present/Plan-20260827 - Config backup and restore.md` |
| 11 | `docs/tutorials/installing-to-desktop.md` |
| 8 | `docs/moonmodules/light/MoonLiveModifier.md` |
| 7 | `docs/usecases/led-signal-integrity.md` |
| 7 | `docs/work/present/Plan-20260903 - MoonLive palettes.md` |
| 6 | `docs/work/present/Plan-20260829 - OSC control ingest.md` |
| 6 | `docs/work/present/Plan-20260910 - projectMM writes British English.md` |
| 5 | `docs/moonmodules/core/supporting.md` |
| 5 | `docs/work/present/Plan-20260908 - Stream the WebSocket state instead of buffering it (attempted, reverted).md` |
| 4 | `docs/index.md` |
| 4 | `docs/reference/light-fixtures.md` |
| 4 | `docs/why-we-write-our-own.md` |
| 3 | `docs/mooncloud.md` |
| 3 | `docs/privacy-policy.md` |
| 3 | `docs/tutorials/control-surface.md` |
| 3 | `docs/work/present/Plan-20260910 - MoonCloud.md` |
| 1 | `docs/work/present/OPEN-WORK.md` |
