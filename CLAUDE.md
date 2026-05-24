# CLAUDE.md

## What This Is

A high-performance multi-platform system that drives large LED installations and DMX lighting fixtures. ESP32 is the primary target. Also runs on Teensy, macOS, Windows, Linux, and Raspberry Pi. C++20. CMake.

See `docs/architecture.md` for system design. This file contains only rules and constraints for working on the project.

## Principles

- **Minimalism means simplicity.** Flat, simple, predictable code. Not clever abstractions. Not elegant templates. If a contributor can't understand it in 30 seconds, it's too complex. Every addition must pay for itself. Prefer removing code over adding it.
- **Data over objects.** Design around data flow, not class hierarchies.
- **Concrete first, abstract later.** Build one working feature end-to-end before extracting patterns into shared abstractions. Don't build the framework before the domain logic works.
- **Domain-neutral core.** Separate core infrastructure from the light domain as much as practical. When mixing is necessary, use domain-neutral naming so the code stays open to future separation.
- **Present tense only.** Code, comments, and documentation describe the system as it is now. No changelogs, no roadmaps. History lives in git commits. Exceptions: `docs/plan.md` (what to build next) and `docs/history/` (lessons, plans, inventories).

## Hard Rules

**Platform boundary.** All `#ifdef`, platform-specific `#include`s, and hardware API calls live exclusively in `src/platform/`. Everything outside `src/platform/` compiles on every target without modification.

**Hot path (render loop):**
- No heap allocations (`new`, `malloc`, `push_back`, `std::string`)
- No blocking (`delay`, `sleep`, `mutex.lock()` — use `try_lock`)
- Integer math preferred over `float` in per-light work

**Memory.** Allocate buffers as single contiguous blocks outside the hot path. Never allocate small scattered objects in loops. On ESP32 with PSRAM, use `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` for large buffers.

**Network input.** Default: process synchronously at a defined point in the frame loop. When sufficient memory is available, async input with staging buffers may be used.

**Warnings are errors.** Build with `-Wall -Wextra -Werror`.

**Tests must pass.** Run `ctest` (unit tests) and `./build/test/mm_scenarios` (scenarios) before considering work complete. New core logic needs a corresponding module test. Full pipeline needs a scenario test. See [docs/testing.md](docs/testing.md) for the test inventory and [docs/architecture.md](docs/architecture.md#testing) for the testing strategy.

## Process Rules

**Specs before code.** Module docs (`docs/moonmodules/*.md`) and the UI spec must be sufficient to implement from before writing code. What's sufficient is case by case. When in doubt, ask.

**Ask, don't guess.** When uncertain about requirements, behavior, or approach — ask the product owner. Asking is always preferred over guessing. This is the default.

**Plan before implementing.** Use `/plan` mode before every feature. Review plans for: unnecessary files, inheritance where structs suffice, modifications outside the relevant directory. Reject and regenerate bad plans.

**Consider extending before creating.** When adding a feature, check if an existing module can be extended cleanly. If a new file is genuinely cleaner, that's fine — but justify it.

**Do not remove comments** unless they are outdated or factually wrong. Comments document intent and context. Removing them silently loses knowledge.

**Anti-stalling.** If a build error or test failure takes more than 2 attempts to fix, STOP. Do not rewrite surrounding files. Ask the product owner for guidance or rollback with Git and re-approach.

**Git: only on explicit request.** Do not `git add`, `git commit`, or `git push` on your own initiative. Only execute these when the product owner explicitly asks (e.g. "commit now", "push it"). The product owner controls when changes are staged, committed, and pushed.

**Pre-commit is initiated by the product owner, not by agents. Never start the checklist on your own — always ask first.** The full checklist (especially the Opus reviewer in gate 7 and the ESP32 build in gate 6) easily takes 10 minutes and burns real tokens; agents must not initiate it unprompted. When the product owner says "run pre-commit" / "pre-commit go" / "commit now" the checklist below runs. Do NOT start it automatically because you finished a feature, because tests pass, because a milestone feels reached, because completion seems imminent, or because an earlier instruction implied a sequence ending in "commit". If you finished the feature work and are unsure whether the product owner wants to commit now, **ask**: "Feature work is done — should I run pre-commit, or do you want to look first?" Treat the checklist as a gate the product owner opens; the agent's job is finishing the work and reporting status so the product owner can decide when to open it.

**Pre-commit checklist (mandatory, in this order):**
1. Desktop build — `cmake --build build` (zero warnings)
2. Unit tests — `ctest --output-on-failure` (all pass)
3. Scenario tests — `./build/test/mm_scenarios` (all pass)
4. Platform boundary — `check_platform_boundary.py` (PASS)
5. Spec check — `check_specs.py` (all ok)
6. ESP32 build — `build_esp32.py` (clean)
7. Reviewer agent — Opus agent reviews staged changes for: domain boundary, **unnecessary abstractions** (no-op / pass-through wrappers that only rename or re-namespace an existing function, single-call-site indirection that would read clearer inlined, names that obscure where the real code lives), **duplicated patterns** (same logic in multiple places that belongs in a base class or shared function), hot-path violations, spec conformance, bloat, platform boundary. Must PASS.
8. KPI collection — `collect_kpi.py --commit` (include in commit message: one-liner as FIRST line of description, full details at bottom). **The one-liner MUST include `tick:Xus(FPS:Y)` for every supported target** (PC + ESP32 today; Teensy/RPi when added). If a target's tick/FPS is missing — e.g. ESP32 wasn't monitored recently and `esp32/monitor.log` is stale — re-run a short live capture before committing, or note explicitly in the commit message why the value is absent.
9. Live scenario analysis — run scenarios on both PC and ESP32 (if available), update `docs/performance.md` with new measurements. Compare with previous values and explain significant changes.
10. Documentation check — verify all new functionality has matching docs: module specs updated, testing.md entries added, architecture docs reflect changes.
11. Permission review — scan `.claude/settings.local.json`. The `allow` list grows organically and accumulates one-off entries (specific `sed` line ranges, one-time `lldb` invocations, `/tmp/probe` paths) that will never recur. Propose to the product owner: (a) one-off entries that can be deleted, and (b) clusters of narrow entries that could collapse into one broad pattern (e.g. several `./build/test/mm_tests -tc="..."` lines → `Bash(./build/test/mm_tests:*)`) so routine commands stop prompting. This is advisory — the agent suggests, the product owner approves any change. Never broaden permissions for destructive or network-mutating commands without explicit approval; err toward keeping the list tight.

Do not commit until steps 1-10 (the functional gates) all pass. Step 11 (permission review) is advisory and does not block a commit. Do not skip the Reviewer agent.

**Mandatory subtraction.** Periodically review and remove code and docs that no longer earn their place. If nothing can be removed, justify why.

**Reference, don't copy.** Previous prototype branches have working solutions. Reference them for proven approaches but don't copy code structure.

## Implementation Process

Build one capability at a time. Each commit produces visible output. The product owner picks what to build next.

### Per-feature workflow

1. **Pick what to build.** One layout, one effect, one driver, one modifier, one system module — whatever adds the next useful capability.
2. **Review only the relevant module drafts.** Cherry-pick from `docs/moonmodules_draft/`. Promote only what's needed to `docs/moonmodules/`.
3. **`/plan` it.** Plan references only the promoted specs + architecture docs. Save the plan as `docs/history/plan-NN.md` (numbered sequentially).
4. **Implement in a branch** (`next-iteration` or feature branch). Test on hardware. Run pre-commit checklist. Commit.
5. **Push.** Product owner pushes. CodeRabbit reviews the PR. Process findings.
6. **Repeat.**

### Branch merge

When a set of features is complete and stable:
1. Review plans (`docs/history/plan-*.md`) — were they followed? What changed?
2. Process lessons learned into `docs/history/decisions.md`
3. Move reviewed plans to `docs/history/archive/`
4. Merge branch to `main` via PR
5. Tag if it's a release milestone

### Releases

A GitHub release marks a milestone useful to end users. Release criteria are defined in `docs/plan.md` per release. General requirements:
- All tests and scenarios pass on all target platforms
- Tested on real hardware
- README updated with quick-start instructions
- No known critical bugs

What the agent reads:
- Always: `CLAUDE.md`, `architecture.md`, `architecture-light.md`
- For this commit: `docs/moonmodules/<only the promoted specs>`
- Never automatically: `docs/history/*`, `docs/moonmodules_draft/*`

## Documentation

```
CLAUDE.md                  ← this file (rules and process)
docs/
  plan.md                  ← what to build next
  architecture.md          ← core system design
  architecture-light.md    ← light domain design
  performance.md           ← per-module timing, memory, sizeof for each platform
  testing.md               ← test inventory with anchored sections
  history/                 ← accumulated wisdom
    decisions.md           ← actions, lessons, proven patterns
    plan-NN.md             ← plans for each feature (numbered)
    archive/               ← reviewed plans moved here after branch merge
  moonmodules/             ← one page per MoonModule (specs before code)
  moonmodules_draft/       ← draft specs for unimplemented modules (temporary, will be empty)
```

Documentation describes the system as it is. Git commits are the history. Module specs are written before implementation. Doc pages are kept current with the code.

**Module specs are end-user / API-integrator documentation, not tech documentation.** Each `docs/moonmodules/<Name>.md` page exists to answer "what does this module do that I can't trivially read off the source file?" Concretely, it should carry:

- **Wire contracts** — REST URLs, JSON shapes, status codes, WebSocket framing, binary frame layouts. Anything an integrator outside the codebase needs.
- **Cross-domain wiring** — how this module connects to other modules through plain data structures (e.g. `HttpServerModule` reads a `PreviewFrame` that `PreviewDriver` writes; the wiring happens in `main.cpp`). Things that span multiple files and don't belong as a comment in any single one.
- **Prior art** — the v1/v2/MoonLight lineage links. History/credits the code can't carry.
- **At minimum, one mention of every control name** — `scripts/check/check_specs.py` enforces this, so the spec stays minimally accurate to the source.

Do **not** repeat facts the `.h` already states: the controls list (the .h has `controls_.addX(...)`), the method signatures (they're declared), the implementation strategy ("uses a TcpServer abstraction" — visible in the includes), or architectural rules that belong in `architecture.md` / `architecture-light.md` (domain boundary, hot-path discipline, etc.). When in doubt: if a fact is visible in the file's `.h`, the `.md` can drop it. The spec-check script and a comment header in the `.h` together carry the contract; the `.md` carries what the file can't.

The `history/` folder is the distilled experience of years of building LED/light systems — from WLED, WLED-MM, StarLight, MoonLight, through projectMM. It contains proven patterns, memory tricks, control mechanisms, and hard-won lessons. We cherry-pick from it — we never implement it wholesale.

## Code Style

- `#pragma once`
- `constexpr` over `#define`
- `std::span` over pointer + length
- Namespace: `mm`, platform code in `mm::platform`
- No `using namespace` in headers
- **Light-domain modules and the `MoonModule` base: header-only.** Every effect, modifier, driver, layout, the light-domain containers (`Layouts`, `Layers`, `Drivers`, `Layer`), and the `MoonModule` base class live in a single `.h` with implementation inline. The benefit is concrete: a contributor copies `RainbowEffect.h`, edits, saves as `MyEffect.h`, registers one line in `main.cpp` — no "where does the `.cpp` go, what does CMake need" friction. The chain `RainbowEffect.h → EffectBase.h → MoonModule.h` is uniform — readers don't pivot to a different file shape at the base. When a light-domain file outgrows one concern, extract a helper into its own header (`BlendMap`, `MappingLUT`) rather than splitting to `.h` + `.cpp`. Header-only is a feature of the light domain.
- **Core service modules: `.h` + `.cpp`.** Core modules that bridge to the platform layer or implement substantial infrastructure (`HttpServerModule`, `FilesystemModule`, `NetworkModule`, `Scheduler`, `SystemModule`) ship as a `.h` (interface) plus a `.cpp` (implementation). Three reasons that compound: (a) implementation changes recompile only the `.cpp`, not every TU that includes the header — incremental builds are 2-5× faster on the kind of edits that happen in development; (b) readers want the interface separately from the body; (c) symbol bloat and link-time stay bounded. Small core utilities that are *almost entirely declarations or inline accessors* — `types.h`, `color.h`, `version.h`, `PreviewFrame.h`, `JsonUtil.h`, `Control.h`, `JsonSink.h`, `Sha1.h`, `Base64.h` — stay header-only. Templates (e.g. `ModuleFactory::registerType<T>`) also must stay in the header because of C++ instantiation rules; a module that's mostly template can therefore stay header-only.
- **Exceptions need a one-line comment at the top of the file naming the reason.** Without a stated reason the file is expected to follow the default for its category. When in doubt: light → header-only, core → `.h` + `.cpp`.
- Semantic variable names — name variables for what they represent, not just their type. `availableHeap` not `available`, `internalHeap` not `internal`, `lutBytes` not `bytes`. A reader should understand the variable without looking at its assignment.
- No hard line wraps in markdown — let the editor soft-wrap

## Agent Roles

The project uses Claude Code agents in defined roles. The user is the **Product Owner** — the critical success factor in agentic coding.

**What the product owner does:**
- Reviews every line of code and every spec change before committing
- Specifies requirements in detail — agents ask, they don't guess
- Controls staging, committing, and pushing (agents never do this)
- Tests on hardware before approving
- Decides what to build next and in what order
- Catches architectural drift, bloat, and unnecessary complexity early
- Evaluates agent suggestions critically — not everything proposed gets built

**Why this matters:** Previous iterations (v1, v2) gave agents more autonomy. The result was bloat, architectural drift, and compounding bugs. The v3 approach — tight product owner control with agents as tools, not decision-makers — produces cleaner, more predictable code. The agent writes; the product owner thinks.

| | Agent | Model | Focus | Does |
|--|-------|-------|-------|------|
| 🤖 | **Architect** | Opus | System design | Reviews against architecture, designs components, validates boundaries |
| 👽 | **Developer** | Sonnet | Implementation | Writes code in worktrees, follows all rules, one step at a time |
| 👾 | **Reviewer** | Opus | Pre-PR check | Runs locally before push as pre-commit step 7 — see that step for the full review scope. Complements CodeRabbit (which handles line-level bugs in the PR). |
| 🛸 | **Tester** | Sonnet | Verification | Writes tests, verifies architectural rules in code |
| 💀 | **Runner** | Haiku | Quick checks | Runs MoonDeck scripts, platform boundary checks, build verification |

Agents work in parallel on independent steps. Agents never commit — only the product owner approves commits after testing.

## Build

See [scripts/MoonDeck.md](scripts/MoonDeck.md) for all build, run, test, and check commands. Quick start: `uv run scripts/moondeck.py`
