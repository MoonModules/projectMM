# CLAUDE.md

## What This Is

A high-performance multi-platform system that drives large LED installations and DMX lighting fixtures. ESP32 is the primary target. Also runs on Teensy, macOS, Windows, Linux, and Raspberry Pi. C++20. CMake.

See `docs/architecture.md` for system design. This file contains only rules and constraints for working on the project.

## Principles

- **Common patterns first.** This repo is meant to be a recognisable example of good practice across code, docs, tests, and UI, not a Frankenstein of bespoke conventions only the authors understand. Hold every decision against it, especially in core architecture and documentation. Before introducing a pattern, name a widely-used project / framework / canonical resource that uses it; if you can't, treat it as bespoke and justify the divergence in a one-line comment at the introduction site. A new contributor with general C++/web experience should recognise the pattern within 30 seconds. Bespoke choices are allowed (header-only light modules, the MoonModule lifecycle, present-tense docs), but each carries its reason at the place it's introduced.
- **Industry standards, our own code.** Reach for the established, recognisable solution: the textbook *algorithm* (a DC-blocker high-pass, a Hann window, RMS, an integer-square-root) AND the textbook *name* for every variable, function, and UI control. That's *Common patterns first* applied to both domains, core and light: take the textbook approach over a clever or borrowed one. Study the prior art hard, whatever sharpens our thinking: repos, datasheets, vendor sites. Respect it, learn from it, credit it by name in the `history/` digests and per-module "Prior art" sections. Then write every line fresh against our own architecture: **carry the ideas forward, but write our own code rather than copying theirs or tracing their structure.** The method that guarantees it: spec from primary sources (ESP32 / peripheral / sensor datasheets, Espressif docs, reference standards), pin the behaviour as tests (unit + scenario), and let the worker-bee agents implement against the process ([CLAUDE.md](CLAUDE.md)) and architecture ([architecture.md](docs/architecture.md)). The result is independent by construction, not a renamed copy.
- **Minimalism means simplicity.** Flat, simple, predictable code. Not clever abstractions. Not elegant templates. If a contributor can't understand it in 30 seconds, it's too complex.
- **Core grows slower than the domain.** Adding a domain module is expected growth: a new unit of domain capability adds lines because it adds a feature, and that's fine. The **core** (domain-neutral infrastructure: `src/core/`, the platform layer, the docs that describe them) is held to a higher bar: while the system is still being built the core does grow, but each core change should buy proportionally more than a domain addition: new infrastructure that many modules use, not a one-off. A core change is suspect until that leverage is shown. Watch the ratio; core is meant to be the lean base under a wider domain, not the other way around.
- **Default to subtraction.** The reflex on most changes (a bug fix, a review finding, a refactor) should be *can this remove or replace code, or land net-neutral?*, not *what do I add?* If a change only ever grows the line count and the doc count, that's the smell this rule exists to catch. Prefer removing code over adding it; a deletion that preserves behaviour is the best kind of change.
- **No duplication, in code or docs.** Same logic in two places belongs in one shared function; same fact in two docs belongs in one place the other links to. A comment or doc paragraph that restates what the code already says is duplication too; delete it. (Reuse a recognisable shape rather than inventing one; see *Common patterns first* above.)
- **Data over objects in the hot path.** Where speed and memory matter most, design around plain contiguous data, not an object graph: a flat buffer of elements that one stage writes and the next stage reads, following the producer/consumer data flow in [docs/architecture.md](docs/architecture.md). This is a deliberate performance choice: a contiguous buffer is cache-friendly and lets a stage do integer math straight on the array, whereas per-element objects with virtual accessors are cache-hostile and allocation-heavy, exactly what the hot-path rules forbid. The one deliberate class hierarchy is the module tree (one `MoonModule` base, shallow subclasses, a single virtual-dispatch boundary), because uniform polymorphism is what lets the UI render any module generically with zero per-module UI code. Don't add inheritance elsewhere, and don't wrap hot-path buffer data in objects.
- **Concrete first, abstract later.** Build one working feature end-to-end before extracting patterns into shared abstractions. Don't build the framework before the domain logic works.
- **Robust to any input.** A running device tolerates any sequence of UI actions or API calls: add, delete, replace, or reconfigure any module in any order, at any grid size, and it keeps running. Degraded or idle is acceptable; crashed is not. This robustness is a defining strongpoint of projectMM, and it's guarded by the test framework, not by hope: a discovered crash drives a new test that pins the fix (see the Hard Rule). Out of scope: power loss, malformed OTA, brown-out, and other physical/electrical faults the firmware can't intercept; this principle is about what the software accepts as input.
- **No reboot to apply a configuration change.** Every setting takes effect live, on the next render tick — change a pin map, a strand length, an output protocol, a mic pin or rate, anything, on a running device and it just works. There is no init-once-at-boot step, and no *config* change requires a restart, which sets projectMM apart from most LED-controller firmware (where a pin or protocol change means a reboot). Like robustness, this is a defining strongpoint, and it falls out of the architecture for free rather than being hand-built per module: any control whose change reshapes derived state routes through the generic `onBuildState()` rebuild sweep, so drivers, the audio peripheral, effects, layouts, modifiers and network I/O all inherit it. When adding a feature, don't reach for a reboot/restart to apply config; make the change live. Full mechanism + rationale: [architecture.md § Live reconfiguration](docs/architecture.md#live-reconfiguration-every-change-applies-without-a-reboot). The one exception is what you'd expect: a *firmware* OTA flash swaps the binary and needs the usual power cycle — that's not a configuration change, and (like power loss and brown-out) it's the same physical-fault boundary the robustness principle draws.
- **Domain-neutral core.** Separate core infrastructure from the light domain as much as practical. When mixing is necessary, use domain-neutral naming so the code stays open to future separation.
- **Present tense only.** Code, comments, and documentation describe the system as it is now. No changelogs, no roadmaps. History lives in git commits. Exceptions: `docs/backlog/` (forward-looking) and `docs/history/` (backward-looking).

## Hard Rules

The design rationale for each rule below lives in [docs/architecture.md](docs/architecture.md). The one-liners here are what the agent holds in working memory.

**Tests must pass.** Run `ctest` (unit tests) and `uv run scripts/scenario/run_scenario.py` (scenarios) before considering work complete. The Python wrapper invokes the C++ runner and persists per-target observations back to each scenario JSON (drift visibility); direct `./build/test/mm_scenarios` is fine for ad-hoc pass/fail checks but skips the JSON write-back. New core logic needs a corresponding module test. Full pipeline needs a scenario test. See [docs/testing.md](docs/testing.md) for the strategy and test inventory.

**Warnings are errors.** Build with `-Wall -Wextra -Werror`. No warning is "harmless"; if it's noise, fix it or silence it explicitly with a `-Wno-…` justified in code.

**Platform boundary.** No `#ifdef`, platform-specific `#include`, or hardware API call outside `src/platform/`. Compile-time platform branching uses `if constexpr` on `platform_config.h` flags. Full rule: [architecture.md § Platform Abstraction](docs/architecture.md#platform-abstraction).

**Hot path discipline.** In the render loop and anything it calls: no heap allocations (`new`, `malloc`, `push_back`, `std::string`), no blocking (`delay`, `sleep`, `mutex.lock()`; use `try_lock`), integer math preferred over `float` per-light. Memory: single contiguous blocks outside the hot path, PSRAM via `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` for large buffers. Network input: synchronous by default. Full rules + rationale: [architecture.md § Hot path discipline](docs/architecture.md#hot-path-discipline).

**Effects must run at every grid size and tick rate.** No crash on 0×0×0; animation math doesn't truncate to zero on fast devices. Full rule + rationale: [architecture.md § Effects](docs/architecture.md#effects).

**Robust to any input.** No UI action or API-call sequence crashes or wedges a running device, including deleting, replacing, or clearing modules in any order. A crash or hang is a bug, not the user's fault; the fix is incomplete until a test reproduces the sequence. Full rule + rationale: [architecture.md § Robustness](docs/architecture.md#robustness).

## Process Rules

**Specs before code.** Module docs (`docs/moonmodules/*.md`) and the UI spec must be sufficient to implement from before writing code. What's sufficient is case by case. When in doubt, ask.

**Ask, don't guess.** When uncertain about requirements, behavior, or approach, ask the product owner. Asking is always preferred over guessing. This is the default.

**Sanity-check every request before acting.** When the product owner asks for something, first hold it against three references: does the request make sense, and does it align with [README.md](README.md), this [CLAUDE.md](CLAUDE.md), and [docs/architecture.md](docs/architecture.md)? If it does, proceed. If it doesn't, if the request contradicts a principle, breaks a hard rule, fights the architecture, or just doesn't add up given context the product owner may have missed, push back briefly before doing the work: name what looks off, name which doc says what, and offer the alternative. The product owner can still say "do it anyway" (they often have context the agent doesn't), but the check has happened. This catches bad decisions early instead of after the diff lands.

**Refactor for simplicity.** When the product owner asks to make something simpler, more consistent, or "cleaner," do not start moving files or rewriting code until three questions are answered in writing:

1. **Enumerate alternatives.** List the 2–4 plausible end states. One line each (e.g. "A: split into core/light", "B: split into core/light/platform", "C: keep flat with naming convention").
2. **For each, name what's gained and what's lost.** Concrete and measurable: lines removed, ambiguities resolved, duplicated parsers eliminated, contributor friction added (every extra subfolder is friction; every empty placeholder dir is friction; every renamed-but-unused alias is friction).
3. **Pick the leanest that solves the actual problem.** Subtraction beats addition. An empty subfolder, a parser duplicated "for clarity", a renamed alias kept "for compatibility" all add friction without paying for themselves; don't propose them.

Then check the recommendation against [§ Principles](#principles) (minimalism, data over objects, concrete first) and propose it as a question, not a fait accompli. The product owner picks; the agent implements only what was picked. If the picked option turns out to need a follow-up change (e.g. an updated naming convention to make the new layout consistent), surface that *before* starting the move so it's a single coherent refactor, not three round-trips.

**Plan before implementing.** Use `/plan` mode before every feature. Review plans for: unnecessary files, inheritance where structs suffice, modifications outside the relevant directory. Reject and regenerate bad plans.

**Use `uv` for every Python invocation.** Never type `python` or `python3` directly; always go through `uv run` (e.g. `uv run scripts/build/build_desktop.py`, `uv run python -c "…"`). This applies to shell commands, CMake `add_custom_command` / `execute_process`, documentation examples, and anything that shells out. In CMake, resolve `find_program(UV_EXECUTABLE NAMES uv REQUIRED HINTS "$ENV{USERPROFILE}/.local/bin" "$ENV{HOME}/.local/bin")` once and use `${UV_EXECUTABLE} run python …` thereafter. Reason: uv manages the project venv and is the project standard ([scripts/MoonDeck.md](scripts/MoonDeck.md)); bare `python3` isn't on PATH on Windows (and macOS Python Launcher pops a Store prompt). If you catch yourself about to type `python`, stop and prefix with `uv run`.

The **one exception** is `esp32/main/CMakeLists.txt`: ESP-IDF builds use IDF's bundled Python venv, not the project venv; adding uv to ESP-IDF docker would be a bigger CI lift than the portability win pays for. That file uses `find_package(Python3 REQUIRED COMPONENTS Interpreter)` and invokes `${Python3_EXECUTABLE}`, so CMake locates whichever Python IDF set up (`.venv\Scripts\python.exe` on Windows IDF, `.venv/bin/python3` on macOS/Linux IDF). The shared `src/ui/embed_ui.cmake` script takes a `PYTHON_CMD` parameter that callers pass: desktop passes `${UV_EXECUTABLE};run;python`, ESP32 passes `${Python3_EXECUTABLE}`.

**Consider extending before creating.** When adding a feature, check if an existing module can be extended cleanly. If a new file is genuinely cleaner, that's fine, but justify it.

**Do not remove comments** unless they are outdated or factually wrong. Comments document intent and context. Removing them silently loses knowledge.

**Anti-stalling.** If a build error or test failure takes more than 2 attempts to fix, STOP. Do not rewrite surrounding files. Ask the product owner for guidance or rollback with Git and re-approach.

**Git: only on explicit request.** Do not `git add`, `git commit`, or `git push` on your own initiative. Only execute these when the product owner explicitly asks (e.g. "commit now", "push it"). The product owner controls when changes are staged, committed, and pushed.

**Gate lists are initiated by the product owner, not by agents. Never start a gate list on your own; always ask first.** The full set of gates (especially the Opus reviewer at PR-merge and the ESP32 build at commit) easily takes 10 minutes per event and burns real tokens; agents must not initiate them unprompted. When the product owner says "run pre-commit" / "commit now" / "pre-merge" / "ready to release" the relevant list below runs. Do NOT start any list automatically because you finished a feature, because tests pass, because a milestone feels reached, because completion seems imminent, or because an earlier instruction implied a sequence ending in "commit". If you finished feature work and are unsure whether the product owner wants to proceed, **ask**: "Feature work is done, should I run pre-commit, or do you want to look first?" Treat each list as a gate the product owner opens; the agent's job is finishing the work and reporting status so the product owner can decide when to open it.

The full gate lists per lifecycle event (commit, push, PR merge, release) live in **Lifecycle Events** below.

**Mandatory subtraction.** Periodically review and remove code and docs that no longer earn their place. If nothing can be removed, justify why.

**Reference, don't copy.** Previous prototype branches have working solutions. Reference them for proven approaches but don't copy code structure.

## Implementation Process

Each commit produces visible output. The product owner picks what to build next.

### Per-feature workflow

1. **Pick what to build.** One layout, one effect, one driver, one modifier, one system module: whatever adds the next useful capability.
2. **Review only the relevant module drafts.** Select from `docs/backlog/moonmodules_draft/`. Promote only what's needed to `docs/moonmodules/`.
3. **`/plan` it.** Plan references only the promoted specs + architecture docs. Plans are not promoted to the repo; the implemented code, docs, and commit message together describe what landed.
4. **Implement in a branch** (`next-iteration` or feature branch). Test on hardware. Run the commit gates (see Lifecycle Events below). Commit.
5. **Push.** Product owner pushes. CodeRabbit reviews the PR. Process findings.
6. **Repeat.**

### Lifecycle Events

The project has **three** gated lifecycle events: **commit**, **PR merge into `main`**, **release tag**. (Push has no gate of its own: every check that needs to land before code goes out either lives in the commit gates or is the CodeRabbit / human PR review.) Each event has its own checklist below. Gates within a list run **only when the change makes them applicable**: every conditional gate states its trigger objectively (e.g. "any file under `src/` changed"). A gate that doesn't apply is skipped; a gate that *does* apply but the product owner chooses to skip must have a one-line reason in the commit body / PR description / release notes. The trail stays honest and auditable.

Initiation is always the product owner's call; see the rule above. Agents never start a list on their own.

#### Event 1: Commit

The narrow safety net: "this snapshot is internally consistent."

**Always run (cheap, applies to every commit):**

1. Spec check, `check_specs.py`, fast (<1s), catches `docs/moonmodules/*.md` ↔ control-name drift even on doc-only commits.

**Conditional (run if trigger matches):**

2. Desktop build, `cmake --build build` (zero warnings), if any file that compiles into the desktop binary changed: `src/`, `test/`, `CMakeLists.txt` (root or `test/`), `library.json`. A YAML / docs / `scripts/` / `.claude/` change cannot break the build.
3. Unit tests, `ctest --output-on-failure` (all pass), same trigger as Desktop build. No build, no tests.
4. Scenario tests, `uv run scripts/scenario/run_scenario.py` (all pass; wraps `mm_scenarios` and persists per-target `observed.<target>` blocks back to each scenario JSON for drift tracking), same trigger as Desktop build, plus any `test/scenarios/*.json` change.
5. Platform boundary, `check_platform_boundary.py`, if any file under `src/` (excluding `src/platform/`) changed.
6. ESP32 build, `build_esp32.py`, if any file under `src/` (excluding `src/platform/desktop/`), `esp32/`, `CMakeLists.txt`, or `library.json` changed.
7. KPI collection, `collect_kpi.py --commit`, if any file under `src/` changed. **The one-liner MUST include `tick:Xus(FPS:Y)` for every supported target** (PC + ESP32 today; Teensy/RPi when added). If a target's tick/FPS is missing (e.g. ESP32 wasn't monitored recently and `esp32/monitor.log` is stale), re-run a short live capture before committing, or note explicitly in the commit body why the value is absent.
8. Board catalog, `check_boards.py`, fast (<1s), if `docs/install/boards.json` or `scripts/check/check_boards.py` changed. Validates the installer catalog: required fields, `default_firmware ∈ firmwares`, every `image` resolves on disk, each `Board.board` control equals its entry `name`, module `type`s are factory-registered (or boot-wired singletons), `pins` controls live only on `*LedDriver` modules, and `supported` capabilities stay within the known vocabulary.

A commit that touches *only* `.github/`, `docs/`, `scripts/` (non-test), `README.md`, `CLAUDE.md`, or `.claude/` therefore runs only the spec check; the rest are no-ops because their triggers don't fire. This is the intended pre-commit cost for CI-only or doc-only changes.

**Recommended (manual, not blocking):**

- **Improv smoke test**, `uv run scripts/build/improv_smoke_test.py --port <port>` (or MoonDeck → ESP32 → **Improv Smoke Test**), recommended when a connected ESP32 is available and any of these changed: `src/core/ImprovFrame.h`, `src/platform/esp32/platform_esp32_improv.cpp`, `docs/install/index.html`, `src/ui/install-picker.js`, `scripts/build/improv_*.py`. Three-step end-to-end check (probe + WiFi provision + LAN reachability). Not a blocking gate because it needs hardware that isn't always plugged in; pair with `preview_installer`'s flash-ready mode for the browser-side equivalent.

**After all gates pass:** stop and wait for the product owner to explicitly say "commit now" (or equivalent). Do not commit on your own initiative.

**When "commit now" is received**, compile the commit message in this format and execute the commit:

8. Commit message format (the structure below uses hard newlines *between* its parts: title, summary, KPI line, bullets are each their own line. But do **not** hard-wrap *within* a part: the summary paragraph and each bullet are a single unbroken line that the viewer soft-wraps, same reasoning as the no-hard-wraps-in-markdown rule in [coding-standards.md](docs/coding-standards.md), keeps diffs clean and renders correctly on GitHub. Only the title obeys a length cap; everything else runs as long as it needs to on one line):
   - **Title line**: short imperative summary of the change (≤ 72 chars), e.g. `Add MirrorModifier and fix PreviewDriver sampling`
   - **Short summary**: a TL;DR for the commit: 1–3 sentences max, end-user readable, plain language. State *what* changed and *why* at the level a release-notes reader cares about; do NOT recap the change sections that follow (the bullets do that), and do NOT enumerate files. If your draft is longer than three sentences or restates section headers, cut it. A reader who only sees the title + this paragraph should know what shipped and why.
   - **KPI one-liner**: the `tick:Xus(FPS:Y)` line from step 7. Omit if the KPI gate didn't run (no `src/` changes).
   - **Change sections**: one section per applicable category below; omit a section entirely if nothing in that area changed. Each section is a bulleted list, one bullet per module/file, in your own words. **Core and Light domain are the preferred default categories**: a test for a core module goes under Core, a script fix that touches a light driver goes under Light domain. Only use the other categories for changes that have no meaningful connection to Core or Light domain:
     - **Core** (`src/core/`, `src/platform/`): e.g. `- HttpServerModule: added 409 guard to prevent overlapping OTA jobs`
     - **Light domain** (`src/light/`): e.g. `- PreviewDriver: replaced strided sampling with max-pooling to fix empty frames`
     - **UI** (`src/ui/`): e.g. `- app.js: auto-fit camera distance on first preview frame`
     - **Scripts / MoonDeck** (`scripts/`): e.g. `- MoonDeck: added per-scenario dropdown to scenario card`
     - **Tests** (`test/`): e.g. `- test_preview_driver: updated default assertions for new fps/detail/decompress values`
     - **Docs / CI** (`docs/`, `README.md`, `CLAUDE.md`, `.github/`, `CMakeLists.txt`): e.g. `- README: consolidated ESP32 install info to web installer`
     - **Reviews**: if any review findings were processed in this commit, one bullet per finding, prefixed by reviewer: 🐇 for CodeRabbit findings, 👾 for internal Reviewer agent findings. Each bullet states what was flagged, what was done (fixed / accepted / deferred), and, if not fixed, why. Omit if no review findings were processed.
   - Full KPI details block at the bottom (as produced by `collect_kpi.py`)

**Not at commit-time** (these run at PR-merge): Reviewer agent; live perf analysis + `docs/performance.md` update; documentation sync sweep; permission review.

**On-demand reviewer.** The product owner can ask for a reviewer pass mid-branch on a single risky commit (say "run reviewer" before commit), and the agent runs it on the staged diff with the same scope as the merge-time gate. Default is fast (no reviewer); the on-demand path is a safety valve when something specific feels off.

#### Event 2: PR merge into `main`

The "this is now trunk" moment. Where the wider hygiene checks live, because once it's in trunk it gets shipped.

**Mandatory:**

1. All commit gates passed on every commit in the PR.
2. PR feedback addressed (CodeRabbit + human review).
3. **Carry forward lessons**: if the branch produced a hard-won lesson, a proven pattern, or a non-obvious decision worth keeping, note it in `docs/history/decisions.md` as part of the branch's commits, so the lesson lands in `main` with the code that proved it. Do this on the branch before the merge commit.
4. **Documentation sync**: every new module / control / API endpoint has matching docs (`docs/moonmodules/*.md`, `docs/testing.md`, `docs/architecture*.md`).
5. **Reviewer agent**: trigger this **first** so it runs while the other checks (docs sync, carry-forward lessons, conditional gates) proceed in parallel. Opus reviewer over the **whole branch diff** (`git diff main...HEAD`). Scope: domain boundary, **common patterns first** (flag any new convention (naming scheme, file shape, build flag, control mechanism, UI affordance) that isn't recognisable from a widely-used project / framework / canonical resource; bespoke choices must carry a stated reason at the introduction site, see the principle in § Principles), **unnecessary abstractions** (no-op / pass-through wrappers that only rename or re-namespace an existing function, single-call-site indirection that would read clearer inlined, names that obscure where the real code lives), **duplicated patterns** (same logic in multiple places that belongs in a base class or shared function), hot-path violations, spec conformance, bloat, platform boundary. Architectural drift is more visible across N commits than across one: "three commits each added a wrapper" reads as a pattern that one commit hides. Findings either get fixed in additional branch commits before merge, or are accepted with a one-line reason in the PR description. CodeRabbit complements this: CodeRabbit handles line-level bugs in the PR; the Reviewer agent handles architectural drift.
6. **PR title and description**: review and update if the work done differs from what the PR title/description says. The description is the permanent record of what landed and why; it should reflect the actual diff, not the original intent.

**Conditional:**

7. **Live perf snapshot**: `docs/performance.md` updated, if the branch touches anything under `src/light/`, `src/core/Scheduler.h`, `src/core/HttpServerModule.cpp`, or any platform code that runs in the tick path. Compare new tick/FPS to the previous committed values; explain significant changes.
8. **Permission review**: scan `.claude/settings.local.json`. The `allow` list grows organically and accumulates one-off entries (specific `sed` line ranges, one-time `lldb` invocations, `/tmp/probe` paths, hard-coded device-IP curls, bare `python3 -c` scratch) that will never recur. Propose to the product owner: (a) one-off entries that can be deleted, and (b) clusters of narrow entries that could collapse into one broad pattern (e.g. several `./build/test/mm_tests -tc="..."` lines → `Bash(./build/test/mm_tests:*)`) so routine commands stop prompting. Advisory: agent suggests, product owner approves. Never broaden permissions for destructive or network-mutating commands without explicit approval; err toward keeping the list tight. **After the product owner approves the cleaned list, immediately `cp .claude/settings.local.json .claude/settings.local.cleaned.json` to save a reference snapshot, and commit `settings.local.cleaned.json` (it is *tracked*, unlike the gitignored live file).** Reason: Claude Code auto-appends a new `allow` entry for every fresh command shape it runs, so the live file silently re-accumulates noise *during the very cleanup* and between merges — it looks like the cleanup "reset back," but it's new session churn, not a revert. The committed snapshot is the canonical clean baseline to diff against and `cp` back from next time, immune to that churn. Not commit-blocking but always worth doing once per merge since this is when noise has accumulated.
9. **README + quick-start refresh**: if the change altered build, flash, or first-run UX.

#### Event 3: Release tag

The "end users will use this" moment. Per-release criteria are defined by the product owner; this is the generic envelope.

**Mandatory:**

1. All PR-merge gates passed on the trunk commit being tagged.
2. **Real hardware test**: at minimum one ESP32, plus any other target the release claims to support. Cannot be agent-verified; **product owner only**.
3. **No known critical bugs**: open issues reviewed; any flagged "release-blocker" closed or downgraded.
4. **Per-release criteria**: every release criterion set by the product owner for this tag is done.

**Conditional:**

5. **Changelog / release notes**: drafted in the GitHub release body. Skip only for unreleased pre-1.0 tags.
6. **Cross-platform smoke**: run scenarios on every supported platform (today: PC + ESP32; later: + Teensy, RPi), if the release claims new platform support or the version bumps a major or minor.
7. **Principles audit**: sweep `docs/` (except `docs/backlog/` and `docs/history/`) and `src/` for forward-looking language ("roadmap", "will be", "planned", "in the future", "currently lacks", `TODO`, `FIXME`) and other violations of § Principles. Acceptable hits carry a one-line justification; the rest get rewritten present-tense or moved to `docs/backlog/backlog.md` / `docs/history/`. The reviewer agent can run this end-to-end. Skip only for releases where the diff against the previous tag is doc-empty.

What the agent reads:
- Always: `CLAUDE.md`, `architecture.md`
- For this commit: `docs/moonmodules/<only the promoted specs>`
- Never automatically: `docs/history/*`, `docs/backlog/*`

## Documentation

```text
CLAUDE.md                  ← this file (rules and process)
docs/
  architecture.md          ← system design (core + light domain)
  coding-standards.md      ← how code is written (conventions, file shape, checks)
  building.md              ← how to build, flash, run for every target
  testing.md               ← test inventory and strategy
  performance.md           ← per-module timing, memory, sizeof for each platform
  backlog/                 ← forward-looking: what to build next (not present-tense)
    README.md              ← index: what's here (list + draft specs + design studies)
    backlog.md             ← the prioritised to-build list
    moonmodules_draft/     ← draft specs for unimplemented modules (promoted out as they ship)
  history/                 ← backward-looking: accumulated wisdom
    README.md              ← index: what's here + cross-repo trends + digest prompt
    decisions.md           ← actions, lessons, proven patterns
    *-inventory.md         ← prior-project surveys (v1, v2, moonlight)
    <repo>.md              ← friend-repo monthly activity digests (FastLED, WLED, …)
  moonmodules/             ← one page per MoonModule (specs before code)
```

Documentation describes the system as it is. Git commits are the history. Module specs are written before implementation. Doc pages are kept current with the code.

**Module specs are end-user / API-integrator documentation, not tech documentation.** Each `docs/moonmodules/<Name>.md` page exists to answer "what does this module do that I can't trivially read off the source file?" Concretely, it should carry:

- **Wire contracts**: REST URLs, JSON shapes, status codes, WebSocket framing, binary frame layouts. Anything an integrator outside the codebase needs.
- **Cross-domain wiring**: how this module connects to other modules through plain data structures (e.g. `HttpServerModule` reads a `PreviewFrame` that `PreviewDriver` writes; the wiring happens in `main.cpp`). Things that span multiple files and don't belong as a comment in any single one.
- **Prior art**: the v1/v2/MoonLight lineage links. History/credits the code can't carry.
- **At minimum, one mention of every control name**: `scripts/check/check_specs.py` enforces this, so the spec stays minimally accurate to the source.

Do **not** repeat facts the `.h` already states: the controls list (the .h has `controls_.addX(...)`), the method signatures (they're declared), the implementation strategy ("uses a TcpServer abstraction", visible in the includes), or architectural rules that belong in `architecture.md` (domain boundary, hot-path discipline, etc.). When in doubt: if a fact is visible in the file's `.h`, the `.md` can drop it. The spec-check script and a comment header in the `.h` together carry the contract; the `.md` carries what the file can't.

The `history/` folder is the distilled experience of years of building LED/light systems, from WLED, WLED-MM, StarLight, MoonLight, through projectMM. It contains proven patterns, memory tricks, control mechanisms, and hard-won lessons, studied under the [*Industry standards, our own code*](#principles) principle. Per-project credits live in the `history/` digests and the per-module "Prior art" sections.

The `backlog/` folder is its forward-looking counterpart: `backlog.md` is the prioritised to-build list, and `moonmodules_draft/` holds specs for modules not yet implemented (selected and promoted to `moonmodules/` as each ships, then deleted from the draft). Both `history/` and `backlog/` are exempt from the present-tense rule and agents don't read them automatically; only when planning new work.

## Code Style

All coding conventions, general (`#pragma once`, `constexpr`, `std::span`, namespaces, semantic names, markdown wrapping) and structural (header-only vs `.h` + `.cpp` for light vs core modules, exception-reason comment), live in [docs/coding-standards.md](docs/coding-standards.md).

## Agent Roles

The project uses Claude Code agents in defined roles. The user is the **Product Owner**, the critical success factor in agentic coding.

**What the product owner does:**
- Reviews every line of code and every spec change before committing
- Specifies requirements in detail; agents ask, they don't guess
- Controls staging, committing, and pushing (agents never do this)
- Tests on hardware before approving
- Decides what to build next and in what order
- Catches architectural drift, bloat, and unnecessary complexity early
- Evaluates agent suggestions critically; not everything proposed gets built

**Why this matters:** Earlier in this project's history, agents had more autonomy. The result was bloat, architectural drift, and compounding bugs. The current approach (tight product owner control with agents as tools, not decision-makers) produces cleaner, more predictable code. The agent writes; the product owner thinks.

| | Agent | Model | Focus | Does |
|--|-------|-------|-------|------|
| 🤖 | **Architect** | Opus | System design | Reviews against architecture, designs components, validates boundaries |
| 👽 | **Developer** | Sonnet | Implementation | Writes code in worktrees, follows all rules, one step at a time |
| 👾 | **Reviewer** | Fable when available, otherwise Opus | Pre-merge check | Runs at PR merge over the whole branch diff (Event 2, gate 5); available on-demand pre-commit on the staged diff when the product owner asks. Complements CodeRabbit (which handles line-level bugs in the PR). |
| 🛸 | **Tester** | Sonnet | Verification | Writes tests, verifies architectural rules in code |
| 💀 | **Runner** | Haiku | Quick checks | Runs MoonDeck scripts, platform boundary checks, build verification |

Agents work in parallel on independent steps. Agents never commit; only the product owner approves commits after testing.

## Build

How to build, flash, run, monitor, and check the project for every target: [docs/building.md](docs/building.md). Per-script reference: [scripts/MoonDeck.md](scripts/MoonDeck.md).

Agents use the CLI (`uv run scripts/<group>/<name>.py`); humans typically use MoonDeck (`uv run scripts/moondeck.py`, port 8420). Same scripts under both. Every gate in [Lifecycle Events](#lifecycle-events) ultimately invokes one of these scripts.
