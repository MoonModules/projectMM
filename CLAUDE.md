# CLAUDE.md

## What This Is

A high-performance multi-platform system that drives large LED installations and DMX lighting fixtures. ESP32 is the primary target. Also runs on Teensy, macOS, Windows, Linux, and Raspberry Pi. C++20. CMake.

See `docs/architecture.md` for system design. This file contains only rules and constraints for working on the project.

## Principles

- **Minimalism means simplicity.** Flat, simple, predictable code. Not clever abstractions. Not elegant templates. If a contributor can't understand it in 30 seconds, it's too complex. Every addition must pay for itself. Prefer removing code over adding it.
- **Data over objects.** Design around data flow, not class hierarchies.
- **Concrete first, abstract later.** Build one working feature end-to-end before extracting patterns into shared abstractions. Don't build the framework before the domain logic works.
- **Domain-neutral core.** Separate core infrastructure from the light domain as much as practical. When mixing is necessary, use domain-neutral naming so the code stays open to future separation.
- **Present tense only.** Code, comments, and documentation describe the system as it is now. No changelogs, no roadmaps. History lives in git commits. The one exception is `docs/history/decisions.md`.

## Hard Rules

**Platform boundary.** All `#ifdef`, platform-specific `#include`s, and hardware API calls live exclusively in `src/platform/`. Everything outside `src/platform/` compiles on every target without modification.

**Hot path (render loop):**
- No heap allocations (`new`, `malloc`, `push_back`, `std::string`)
- No blocking (`delay`, `sleep`, `mutex.lock()` — use `try_lock`)
- Integer math preferred over `float` in per-light work

**Memory.** Allocate buffers as single contiguous blocks outside the hot path. Never allocate small scattered objects in loops. On ESP32 with PSRAM, use `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` for large buffers.

**Network input.** Default: process synchronously at a defined point in the frame loop. When sufficient memory is available, async input with staging buffers may be used.

**Warnings are errors.** Build with `-Wall -Wextra -Werror`.

**Tests must pass.** Run `cmake --build build --target test` before considering work complete. New core logic needs a corresponding module test. Full pipeline needs a scenario test. See [docs/testing.md](docs/testing.md) for the test inventory and [docs/architecture.md](docs/architecture.md#testing) for the testing strategy.

## Process Rules

**Specs before code.** Module docs (`docs/moonmodules/*.md`) and the UI spec must be sufficient to implement from before writing code. What's sufficient is case by case. When in doubt, ask.

**Ask, don't guess.** When uncertain about requirements, behavior, or approach — ask the product owner. Asking is always preferred over guessing. This is the default.

**Plan before implementing.** Use `/plan` mode before every feature. Review plans for: unnecessary files, inheritance where structs suffice, modifications outside the relevant directory. Reject and regenerate bad plans.

**Consider extending before creating.** When adding a feature, check if an existing module can be extended cleanly. If a new file is genuinely cleaner, that's fine — but justify it.

**Anti-stalling.** If a build error or test failure takes more than 2 attempts to fix, STOP. Do not rewrite surrounding files. Ask the product owner for guidance or rollback with Git and re-approach.

**No staging files.** Do not `git add` files. The product owner stages, tests, and commits manually.

**No pushing.** Do not `git push`. The product owner pushes manually.

**KPI in every commit.** Run `uv run scripts/check/collect_kpi.py --commit` and include the output in the commit message: one-liner summary as the FIRST line of the commit description (line after the title), full details at the bottom.

**Mandatory subtraction.** Periodically review and remove code and docs that no longer earn their place. If nothing can be removed, justify why.

**Reference, don't copy.** Previous prototype branches have working solutions. Reference them for proven approaches but don't copy code structure.

## Implementation Process

Build one capability at a time. Each commit produces visible output. The product owner picks what to build next.

1. **Pick what to build.** One layout, one effect, one driver, one modifier, one system module — whatever adds the next useful capability.
2. **Review only the relevant module drafts.** Cherry-pick from `docs/moonmodules_draft/`. Promote only what's needed to `docs/moonmodules/`.
3. **`/plan` it.** Plan references only the promoted specs + architecture docs.
4. **Implement, test on hardware, commit.**
5. **Repeat.**

What the agent reads:
- Always: `CLAUDE.md`, `architecture.md`, `architecture-light.md`
- For this commit: `docs/moonmodules/<only the promoted specs>`
- Never automatically: `docs/history/*`, `docs/moonmodules_draft/*`

## Documentation

```
CLAUDE.md                  ← this file (rules and process)
docs/
  architecture.md          ← core system design
  architecture-light.md    ← light domain design
  history/                 ← accumulated wisdom
    decisions.md           ← actions, lessons, proven patterns
    moonlight-inventory.md ← MoonLight gems to harvest
    v1-inventory.md        ← projectMM v1 inventory
    v2-inventory.md        ← projectMM v2 inventory
  moonmodules/             ← one page per MoonModule (specs before code)
  moonmodules_draft/           ← draft specs from prototype, to be reviewed
```

Documentation describes the system as it is. Git commits are the history. Module specs are written before implementation. Doc pages are kept current with the code.

The `history/` folder is the distilled experience of years of building LED/light systems — from WLED, WLED-MM, StarLight, MoonLight, through projectMM. It contains proven patterns, memory tricks, control mechanisms, and hard-won lessons. We cherry-pick from it — we never implement it wholesale.

## Code Style

- `#pragma once`
- `constexpr` over `#define`
- `std::span` over pointer + length
- Namespace: `mm`, platform code in `mm::platform`
- No `using namespace` in headers
- MoonModules are single-file (`.h` only, implementation inline)
- No hard line wraps in markdown — let the editor soft-wrap

## Agent Roles

The project uses Claude Code agents in defined roles. The user is the **Product Owner** — defines requirements, sets priorities, approves work.

| Agent | Model | Focus | Does |
|-------|-------|-------|------|
| **Architect** | Opus | System design | Reviews against architecture, designs components, validates boundaries |
| **Developer** | Sonnet | Implementation | Writes code in worktrees, follows all rules, one step at a time |
| **Reviewer** | Opus | Code quality | Reviews diffs against CLAUDE.md rules, flags violations |
| **Tester** | Sonnet | Verification | Writes tests, verifies architectural rules in code |
| **Runner** | Haiku | Quick checks | Runs MoonDeck scripts, platform boundary checks, build verification |

Agents work in parallel on independent steps. Agents never commit — only the product owner approves commits after testing.

## Build

See [scripts/MoonDeck.md](scripts/MoonDeck.md) for all build, run, test, and check commands. Quick start: `uv run scripts/moondeck.py`
