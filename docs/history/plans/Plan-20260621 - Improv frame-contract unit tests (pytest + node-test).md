# Plan — Improv frame-contract unit tests (pytest + node:test, in CI)

## Context

MoonDeck (Python) and the web installer (JS) have **no unit tests** today (no pytest dep, no package.json). The single highest-value target is the **Improv wire frame**, implemented **three times** that must agree byte-for-byte:

- device C++ — `src/core/ImprovFrame.h` (parser) + `src/platform/esp32/platform_esp32_improv.cpp` (handlers)
- Python — `scripts/build/improv_provision.py::build_frame` / `checksum`
- installer JS — `docs/install/install-orchestrator.js::buildImprovFrame` + the APPLY_OP chunker

All three use `IMPROV` magic + version 1 + type + length + payload + **sum-mod-256** checksum (verified: `ImprovFrame.h:115`, JS `& 0xff`, Py `& 0xFF`). Drift here silently breaks provisioning. Pin it with a **golden vector** asserted on both the Python and JS sides (and hand-checked against C++).

## Decisions (product owner)

- Frameworks: **pytest** (Python, add as dev dep, `uv run pytest`) + **node:test** (JS, `node --test`, zero npm deps).
- Scope: **Improv frame contract first** (not MoonDeck/installer-wide logic yet).
- CI: **add to commit gates + a new PR-triggered `.github/workflows/test.yml`** (no PR test gate exists today).
- JS testability: **extract `docs/install/improv-frame.js`** — the orchestrator's top-level `import` from `unpkg.com` makes the module non-importable in node, so move the pure byte-building into a dependency-free shared module both the orchestrator and the test import.
- Layout: **`test/python/` + `test/js/`** (mirrors the existing `test/` C++ dirs).

## Files

1. **New** `docs/install/improv-frame.js` — pure, dependency-free ES module: `IMPROV_MAGIC`, `IMPROV_FRAME_TYPE_RPC`, `IMPROV_CMD_*`, `APPLY_OP_CHUNK_MAX`, `buildImprovFrame(type, payload)`, `encodeApplyOpFrames(op)` (returns the array of frames for an op, incl. chunking). ~40 lines moved out of the orchestrator — not new logic.
2. **Edit** `docs/install/install-orchestrator.js` — `import` those from `./improv-frame.js`; delete the inlined copies + constants. No behavior change. `sendApplyOpFrame` becomes "encode via `encodeApplyOpFrames`, write each + pace."
3. **New** `test/js/improv-frame.test.mjs` — `node:test`: frame layout, checksum, APPLY_OP single-frame + multi-chunk (seq/last), and the golden vector.
4. **New** `test/python/test_improv_frame.py` — pytest over `improv_provision.build_frame`/`checksum` (imported via `sys.path`), same golden vector. (`import serial` is already lazy/`try`-guarded, so the import is clean without pyserial.)
5. **Edit** `pyproject.toml` — add `pytest` dev dependency.
6. **New** `.github/workflows/test.yml` — PR-triggered: `uv run pytest test/python` + `node --test test/js`.
7. **Edit** `CLAUDE.md` Event 1 (commit gates) — add the pytest + node:test step (trigger: `scripts/**`, `docs/install/**`, `test/python/**`, `test/js/**` changed).

## Golden vector

One fixed input → exact expected bytes, asserted identically in both test files (and documented for the C++ side). E.g. `buildImprovFrame(0x03, [0x01]) == IMPROV + [1,3,1,1] + [checksum]`, and an `APPLY_OP` of a known small op → its single frame; a >125-byte op → N frames with correct seq/last.

## Verification

`uv run pytest`, `node --test` both green; golden vector matches across Python ↔ JS; hand-verified against `ImprovFrame.h`. Existing 423 C++ tests + gates unaffected (no `src/` change).

## Scope

~40 lines moved (JS extract, net-neutral) + 2 small test files + 1 dev-dep + CI/gate glue. Mostly new test files; one clean no-behavior-change source extraction.
