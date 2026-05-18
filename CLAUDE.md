# CLAUDE.md

## What This Is

A high-performance multi-platform system that drives large LED installations
and DMX lighting fixtures. ESP32 (ESP-IDF, no Arduino) is the primary
target. Also runs on Teensy, macOS, Windows, Linux, and Raspberry Pi.
C++20. CMake.

See `docs/architecture.md` for system design. This file contains only
rules and constraints for working on the project.

## Principles

- **Minimalism.** Every addition must pay for itself. Prefer removing code
  over adding it. No speculative abstractions.
- **Data over objects.** Design around data flow, not class hierarchies.
- **Let structure emerge.** Don't pre-design files, classes, or interfaces
  for things that don't exist yet. Build what you need, refactor when
  patterns become clear.
- **Domain-neutral core.** Separate core infrastructure from the light
  domain as much as practical. When mixing is necessary for performance
  or simplicity, use domain-neutral naming (e.g. "producer buffer" not
  "LED buffer") so the code stays open to future separation.
- **Present tense only.** Code, comments, and documentation describe the
  system as it is now. No changelogs, no roadmaps, no "will be added
  later" notes. History lives in git commits. The one exception is
  `docs/decisions.md` — a record of approaches we tried and rejected,
  to avoid repeating mistakes.

## Hard Rules

**Platform boundary.** All `#ifdef`, platform-specific `#include`s, and
hardware API calls live exclusively in `src/platform/`. Everything outside
`src/platform/` compiles on every target without modification.

**Hot path (render loop):**
- No heap allocations (`new`, `malloc`, `push_back`, `std::string` construction)
- No blocking (`delay`, `sleep`, `mutex.lock()` — use `try_lock` or lock-free)
- Integer math preferred over `float` in per-pixel work

**Memory.** Allocate buffers as single contiguous blocks outside the hot
path. Never allocate small scattered objects in loops. On ESP32 with PSRAM,
use `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` for large buffers.

**Network input.** Process synchronously at a defined point in the frame
loop. No async network tasks writing into render buffers. Minimize all
network-related buffer overhead.

**Build errors.** Stop. Diagnose root cause. Do not retry or work around.

**No staging files.** Do not `git add` files. The product owner stages
and commits manually.

**Warnings are errors.** Build with `-Wall -Wextra -Werror`.

**Tests must pass.** Run `cmake --build build --target test` before
considering work complete. New core logic needs a corresponding test.

## Code Style

- `#pragma once`
- `constexpr` over `#define`
- `std::span` over pointer + length
- Namespace: `mm`, platform code in `mm::platform`
- No `using namespace` in headers
- MoonModules are single-file (`.h` only, implementation inline) to
  minimize files and make authoring easy

## Agent Roles

The project uses Claude Code agents in defined roles. The user is the
**Product Owner** — defines requirements, sets priorities, approves work.

| Agent | Model | Focus | Does |
|-------|-------|-------|------|
| **Architect** | Opus | System design | Reviews against architecture, designs components, validates boundaries |
| **Developer** | Sonnet | Implementation | Writes code in worktrees, follows all rules, one step at a time |
| **Reviewer** | Opus | Code quality | Reviews diffs against CLAUDE.md rules, flags violations |
| **Tester** | Sonnet | Verification | Writes tests, verifies architectural rules in code |
| **Runner** | Haiku | Quick checks | Runs MoonDeck scripts, platform boundary checks, build verification, formatting |

Agents work in parallel on independent steps (e.g. core types and
platform code). Agents never commit — only the product owner approves
commits after testing.

## Build

See [scripts/MoonDeck.md](scripts/MoonDeck.md) for all build, run, test,
and check commands. Quick start: `uv run scripts/moondeck.py`
