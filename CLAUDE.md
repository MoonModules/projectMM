# CLAUDE.md

## What This Is

A high-performance multi-platform system that drives large LED installations and DMX lighting fixtures. ESP32 is the primary target. Also runs on Teensy, macOS, Windows, Linux, and Raspberry Pi. C++20. CMake.

See `docs/architecture.md` for system design. This file contains only rules and constraints for working on the project.

## Principles

- **Common patterns first.** This repo is meant to be a recognisable example of good practice across code, docs, tests, and UI — not a Frankenstein of bespoke conventions only the authors understand. Hold every decision against it, especially in core architecture and documentation. Before introducing a pattern, name a widely-used project / framework / canonical resource that uses it; if you can't, treat it as bespoke and justify the divergence in a one-line comment at the introduction site. A new contributor with general C++/web experience should recognise the pattern within 30 seconds. Bespoke choices are allowed — header-only light modules, the MoonModule lifecycle, present-tense docs — but each carries its reason at the place it's introduced.
- **Minimalism means simplicity.** Flat, simple, predictable code. Not clever abstractions. Not elegant templates. If a contributor can't understand it in 30 seconds, it's too complex. Every addition must pay for itself. Prefer removing code over adding it.
- **Data over objects.** Design around data flow, not class hierarchies.
- **Concrete first, abstract later.** Build one working feature end-to-end before extracting patterns into shared abstractions. Don't build the framework before the domain logic works.
- **Domain-neutral core.** Separate core infrastructure from the light domain as much as practical. When mixing is necessary, use domain-neutral naming so the code stays open to future separation.
- **Present tense only.** Code, comments, and documentation describe the system as it is now. No changelogs, no roadmaps. History lives in git commits. Exceptions: `docs/plan.md` (what to build next) and `docs/history/` (lessons, plans, inventories).

## Hard Rules

The design rationale for each rule below lives in [docs/architecture.md](docs/architecture.md). The one-liners here are what the agent holds in working memory.

**Tests must pass.** Run `ctest` (unit tests) and `./build/test/mm_scenarios` (scenarios) before considering work complete. New core logic needs a corresponding module test. Full pipeline needs a scenario test. See [docs/testing.md](docs/testing.md) for the strategy and test inventory.

**Warnings are errors.** Build with `-Wall -Wextra -Werror`. No warning is "harmless" — if it's noise, fix it or silence it explicitly with a `-Wno-…` justified in code.

**Platform boundary.** No `#ifdef`, platform-specific `#include`, or hardware API call outside `src/platform/`. Compile-time platform branching uses `if constexpr` on `platform_config.h` flags. Full rule: [architecture.md § Platform Abstraction](docs/architecture.md#platform-abstraction).

**Hot path discipline.** In the render loop and anything it calls: no heap allocations (`new`, `malloc`, `push_back`, `std::string`), no blocking (`delay`, `sleep`, `mutex.lock()` — use `try_lock`), integer math preferred over `float` per-light. Memory: single contiguous blocks outside the hot path, PSRAM via `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` for large buffers. Network input: synchronous by default. Full rules + rationale: [architecture.md § Hot path discipline](docs/architecture.md#hot-path-discipline).

**Effects must run at every grid size and tick rate.** No crash on 0×0×0; animation math doesn't truncate to zero on fast devices. Full rule + rationale: [architecture.md § Effects](docs/architecture.md#effects).

## Process Rules

**Specs before code.** Module docs (`docs/moonmodules/*.md`) and the UI spec must be sufficient to implement from before writing code. What's sufficient is case by case. When in doubt, ask.

**Ask, don't guess.** When uncertain about requirements, behavior, or approach — ask the product owner. Asking is always preferred over guessing. This is the default.

**Plan before implementing.** Use `/plan` mode before every feature. Review plans for: unnecessary files, inheritance where structs suffice, modifications outside the relevant directory. Reject and regenerate bad plans.

**Consider extending before creating.** When adding a feature, check if an existing module can be extended cleanly. If a new file is genuinely cleaner, that's fine — but justify it.

**Do not remove comments** unless they are outdated or factually wrong. Comments document intent and context. Removing them silently loses knowledge.

**Anti-stalling.** If a build error or test failure takes more than 2 attempts to fix, STOP. Do not rewrite surrounding files. Ask the product owner for guidance or rollback with Git and re-approach.

**Git: only on explicit request.** Do not `git add`, `git commit`, or `git push` on your own initiative. Only execute these when the product owner explicitly asks (e.g. "commit now", "push it"). The product owner controls when changes are staged, committed, and pushed.

**Gate lists are initiated by the product owner, not by agents. Never start a gate list on your own — always ask first.** The full set of gates (especially the Opus reviewer at push and the ESP32 build at commit) easily takes 10 minutes per event and burns real tokens; agents must not initiate them unprompted. When the product owner says "run pre-commit" / "commit now" / "pre-push" / "pre-merge" / "ready to release" the relevant list below runs. Do NOT start any list automatically because you finished a feature, because tests pass, because a milestone feels reached, because completion seems imminent, or because an earlier instruction implied a sequence ending in "commit". If you finished feature work and are unsure whether the product owner wants to proceed, **ask**: "Feature work is done — should I run pre-commit, or do you want to look first?" Treat each list as a gate the product owner opens; the agent's job is finishing the work and reporting status so the product owner can decide when to open it.

The full gate lists per lifecycle event (commit, push, PR merge, release) live in **Lifecycle Events** below.

**Mandatory subtraction.** Periodically review and remove code and docs that no longer earn their place. If nothing can be removed, justify why.

**Reference, don't copy.** Previous prototype branches have working solutions. Reference them for proven approaches but don't copy code structure.

## Implementation Process

Build one capability at a time. Each commit produces visible output. The product owner picks what to build next.

### Per-feature workflow

1. **Pick what to build.** One layout, one effect, one driver, one modifier, one system module — whatever adds the next useful capability.
2. **Review only the relevant module drafts.** Cherry-pick from `docs/moonmodules_draft/`. Promote only what's needed to `docs/moonmodules/`.
3. **`/plan` it.** Plan references only the promoted specs + architecture docs. Save the plan as `docs/history/plan-NN.md` (numbered sequentially).
4. **Implement in a branch** (`next-iteration` or feature branch). Test on hardware. Run the commit gates (see Lifecycle Events below). Commit.
5. **Push.** Run the push gates. Product owner pushes. CodeRabbit reviews the PR. Process findings.
6. **Repeat.**

### Lifecycle Events

The project has **four** gated lifecycle events: **commit**, **push**, **PR merge into `main`**, **release tag**. Each has its own checklist below. Gates within a list run **only when the change makes them applicable** — every conditional gate states its trigger objectively (e.g. "any file under `src/` changed"). A gate that doesn't apply is skipped; a gate that *does* apply but the product owner chooses to skip must have a one-line reason in the commit body / PR description / release notes. The trail stays honest and auditable.

Initiation is always the product owner's call — see the rule above. Agents never start a list on their own.

#### Event 1 — Commit

The narrow safety net: "this snapshot is internally consistent."

**Mandatory (always run):**

1. Desktop build — `cmake --build build` (zero warnings)
2. Unit tests — `ctest --output-on-failure` (all pass)
3. Scenario tests — `./build/test/mm_scenarios` (all pass)

**Conditional (run if trigger matches):**

4. Platform boundary — `check_platform_boundary.py` — if any file under `src/` (excluding `src/platform/`) changed.
5. Spec check — `check_specs.py` — if any `src/` file with controls or any `docs/moonmodules/*.md` changed.
6. ESP32 build — `build_esp32.py` — if any file under `src/` (excluding `src/platform/desktop/`), `esp32/`, `CMakeLists.txt`, or `library.json` changed.
7. KPI collection — `collect_kpi.py --commit` — if any file under `src/` changed. The one-liner goes as the **first** line of the commit body, full details at the bottom. **The one-liner MUST include `tick:Xus(FPS:Y)` for every supported target** (PC + ESP32 today; Teensy/RPi when added). If a target's tick/FPS is missing — e.g. ESP32 wasn't monitored recently and `esp32/monitor.log` is stale — re-run a short live capture before committing, or note explicitly in the commit body why the value is absent.

**Not at commit-time** (these moved to push or PR-merge): Reviewer agent → push; live perf analysis + `docs/performance.md` update → PR merge; documentation sync sweep → PR merge; permission review → PR merge.

#### Event 2 — Push

The "external eyes about to see this" moment. Cheap if commit gates passed; the additions are checks that benefit from being run on a complete branch, not per-commit.

**Mandatory:**

1. All commit gates have passed for every commit in the push range (`git log origin/<branch>..HEAD`).
2. Branch is rebased / up-to-date with its target.

**Conditional:**

3. Reviewer agent — Opus reviewer over the **push range** (`git diff origin/<branch>..HEAD`). Scope: domain boundary, **common patterns first** (flag any new convention — naming scheme, file shape, build flag, control mechanism, UI affordance — that isn't recognisable from a widely-used project / framework / canonical resource; bespoke choices must carry a stated reason at the introduction site, see the principle in § Principles), **unnecessary abstractions** (no-op / pass-through wrappers that only rename or re-namespace an existing function, single-call-site indirection that would read clearer inlined, names that obscure where the real code lives), **duplicated patterns** (same logic in multiple places that belongs in a base class or shared function), hot-path violations, spec conformance, bloat, platform boundary. Must PASS. Skip if the push is a single doc-only or test-only commit.

The PR review (CodeRabbit, human review) happens after push and is outside the local gating system. Complements the Reviewer agent — CodeRabbit handles line-level bugs in the PR; the Reviewer agent handles architectural drift before push.

#### Event 3 — PR merge into `main`

The "this is now trunk" moment. Where the wider hygiene checks live, because once it's in trunk it gets shipped.

**Mandatory:**

1. All push gates passed.
2. PR feedback addressed (CodeRabbit + human review).
3. **Plan reconciliation** — for each plan in `docs/history/plan-*.md` covered by this branch: was it followed? What changed? Note in `docs/history/decisions.md` if anything is worth carrying forward. Move reviewed plans to `docs/history/archive/`.
4. **Documentation sync** — every new module / control / API endpoint has matching docs (`docs/moonmodules/*.md`, `docs/testing.md`, `docs/architecture*.md`).

**Conditional:**

5. **Live perf snapshot** — `docs/performance.md` updated — if the branch touches anything under `src/light/`, `src/core/Scheduler.h`, `src/core/HttpServerModule.cpp`, or any platform code that runs in the tick path. Compare new tick/FPS to the previous committed values; explain significant changes.
6. **Permission review** — scan `.claude/settings.local.json`. The `allow` list grows organically and accumulates one-off entries (specific `sed` line ranges, one-time `lldb` invocations, `/tmp/probe` paths) that will never recur. Propose to the product owner: (a) one-off entries that can be deleted, and (b) clusters of narrow entries that could collapse into one broad pattern (e.g. several `./build/test/mm_tests -tc="..."` lines → `Bash(./build/test/mm_tests:*)`) so routine commands stop prompting. Advisory — agent suggests, product owner approves. Never broaden permissions for destructive or network-mutating commands without explicit approval; err toward keeping the list tight. Not commit-blocking but always worth doing once per merge since this is when noise has accumulated.
7. **README + quick-start refresh** — if the change altered build, flash, or first-run UX.

#### Event 4 — Release tag

The "end users will use this" moment. Per-release criteria are defined by the product owner; this is the generic envelope.

**Mandatory:**

1. All PR-merge gates passed on the trunk commit being tagged.
2. **Real hardware test** — at minimum one ESP32, plus any other target the release claims to support. Cannot be agent-verified; **product owner only**.
3. **No known critical bugs** — open issues reviewed; any flagged "release-blocker" closed or downgraded.
4. **Per-release criteria** — every release criterion set by the product owner for this tag is done.

**Conditional:**

5. **Changelog / release notes** — drafted in the GitHub release body. Skip only for unreleased pre-1.0 tags.
6. **Cross-platform smoke** — run scenarios on every supported platform (today: PC + ESP32; later: + Teensy, RPi) — if the release claims new platform support or the version bumps a major or minor.

What the agent reads:
- Always: `CLAUDE.md`, `architecture.md`
- For this commit: `docs/moonmodules/<only the promoted specs>`
- Never automatically: `docs/history/*`, `docs/moonmodules_draft/*`

## Documentation

```
CLAUDE.md                  ← this file (rules and process)
docs/
  plan.md                  ← what to build next
  architecture.md          ← system design (core + light domain)
  coding-standards.md      ← how code is written (conventions, file shape, checks)
  building.md              ← how to build, flash, run for every target
  testing.md               ← test inventory and strategy
  performance.md           ← per-module timing, memory, sizeof for each platform
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

Do **not** repeat facts the `.h` already states: the controls list (the .h has `controls_.addX(...)`), the method signatures (they're declared), the implementation strategy ("uses a TcpServer abstraction" — visible in the includes), or architectural rules that belong in `architecture.md` (domain boundary, hot-path discipline, etc.). When in doubt: if a fact is visible in the file's `.h`, the `.md` can drop it. The spec-check script and a comment header in the `.h` together carry the contract; the `.md` carries what the file can't.

The `history/` folder is the distilled experience of years of building LED/light systems — from WLED, WLED-MM, StarLight, MoonLight, through projectMM. It contains proven patterns, memory tricks, control mechanisms, and hard-won lessons. We cherry-pick from it — we never implement it wholesale.

## Code Style

All coding conventions — general (`#pragma once`, `constexpr`, `std::span`, namespaces, semantic names, markdown wrapping) and structural (header-only vs `.h` + `.cpp` for light vs core modules, exception-reason comment) — live in [docs/coding-standards.md](docs/coding-standards.md).

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
| 👾 | **Reviewer** | Opus | Pre-PR check | Runs locally at push (Event 2, gate 3) — see that gate for the full review scope. Complements CodeRabbit (which handles line-level bugs in the PR). |
| 🛸 | **Tester** | Sonnet | Verification | Writes tests, verifies architectural rules in code |
| 💀 | **Runner** | Haiku | Quick checks | Runs MoonDeck scripts, platform boundary checks, build verification |

Agents work in parallel on independent steps. Agents never commit — only the product owner approves commits after testing.

## Build

How to build, flash, run, monitor, and check the project for every target: [docs/building.md](docs/building.md). Per-script reference: [scripts/MoonDeck.md](scripts/MoonDeck.md).

Agents use the CLI (`uv run scripts/<group>/<name>.py`); humans typically use MoonDeck (`uv run scripts/moondeck.py`, port 8420). Same scripts under both. Every gate in [Lifecycle Events](#lifecycle-events) ultimately invokes one of these scripts.
