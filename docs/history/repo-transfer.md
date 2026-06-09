# Repo transfer: ewowi/projectMM → MoonModules/projectMM

A handoff runbook for moving the repository from the `ewowi` personal account to the `MoonModules` organisation, and cutting the web-installer URLs over to the org's `moonmodules.org` domain.

**Who this is for:** a Claude Code session in a freshly-cloned `MoonModules/projectMM` that has none of the planning conversation's context. Follow the stages in order. Each stage is independently verifiable, so a break is easy to localise.

**Why two stages.** The org `MoonModules` already serves Pages on the primary domain `moonmodules.org` (e.g. `moonmodules.org/MoonLight/`). The instant the repo lands in the org, its Pages auto-serve at `moonmodules.org/projectMM/` and the old `ewowi.github.io/projectMM/` URLs **301-redirect** there. That redirect is the safety net: it lets us split "rename the repo references" (Stage 1) from "cut the installer URLs to the canonical domain" (Stage 2, the only CORS-sensitive change) and prove each works on its own.

---

## Pre-transfer baselines (capture before anything moves)

So you can diff against a known-good state afterwards. Captured 2026-06-09 on `ewowi`:

- `https://ewowi.github.io/projectMM/install/` → **HTTP 200**, `server: GitHub.com`.
- `https://ewowi.github.io/projectMM/install/boards.json` → **HTTP 200**, `content-type: application/json`, **`access-control-allow-origin: *`** (the CORS-open header the installer's fetch depends on).
- `moonmodules.github.io/MoonLight/` → **301** → `moonmodules.org/MoonLight/` (proves the org primary domain is live).

## Scope of the rewrites (exact, as of this doc)

- **Stage 1** — `ewowi/projectMM` → `MoonModules/projectMM`: **~82 occurrences across ~28 files** (clone URLs, `github.com/...blob/...` and `raw.githubusercontent.com/...` links in docs, JS, workflows, headers).
- **Stage 2** — `ewowi.github.io/projectMM` → `moonmodules.org/projectMM`: **~11 occurrences across ~9 files** (docs + the installer JS constants in `src/ui/app.js` and `docs/install/`).

### Do NOT rewrite these `ewowi` references

The rewrite must use **exact full-string patterns**, because some `ewowi` mentions are legitimate and permanent:

- **This file (`docs/history/repo-transfer.md`)** — it documents the `ewowi/projectMM` and `ewowi.github.io` strings on purpose. **Skip it in both stages**, or the sweep rewrites its own instructions. Exclude it explicitly from the rewrite.
- **`ewowi/StarLight`, `ewowi/projectMM-v1`, `ewowi/projectMM-v2`** — these are *different repositories* (prior projects) that genuinely live under the `ewowi` personal account. They stay.
- **`ewowi` as a commit-author name** (e.g. in `docs/history/hpwit-I2SClocklessLedDriver.md`) — a historical fact, not a URL.
- The two patterns are independent strings: `ewowi.github.io/projectMM` does **not** contain `ewowi/projectMM`, so a targeted replace of one never disturbs the other. Replace exact strings, not bare `ewowi`.

---

## Stage 1 — transfer the repo + rewrite repo references

### 1a. Product-owner: transfer on GitHub
- `ewowi/projectMM` → Settings → **Transfer ownership** → `MoonModules`.
- After transfer, on `MoonModules/projectMM` → Settings → **Pages**:
  - **Source: GitHub Actions** (projectMM deploys via `actions/deploy-pages@v4` in `release.yml` — do **not** switch to "Deploy from a branch"; MoonLight uses a branch, projectMM does not, and that's correct).
  - **Custom domain: leave BLANK** (MoonLight's is blank too — the org primary domain handles path-based serving). If the transfer injected a CNAME, clear it.
  - **Enforce HTTPS: on.**
- Org-level (likely already fine — verify only if the first CI run fails): Org → Settings → Actions → General allows `actions/checkout`, `actions/deploy-pages`, `actions/upload-pages-artifact`, `actions/configure-pages`; the `github-pages` environment exists.

### 1b. Agent: repoint the local clone
```sh
git remote set-url origin https://github.com/MoonModules/projectMM.git
git remote -v   # confirm both fetch+push point at MoonModules
```

### 1c. Agent: rewrite `ewowi/projectMM` → `MoonModules/projectMM` ONLY
Leave every `ewowi.github.io` reference untouched in this stage. Suggested sweep (review the file list first, then apply):
```sh
grep -rl 'ewowi/projectMM' --include='*.md' --include='*.py' --include='*.json' \
  --include='*.html' --include='*.js' --include='*.yml' --include='*.yaml' \
  --include='*.h' --include='*.cpp' --include='*.cmake' . | grep -v '/.git/'
# then, per file, replace the exact string 'ewowi/projectMM' with 'MoonModules/projectMM'
```
Verify nothing in the excluded list got hit:
```sh
grep -rn 'ewowi/StarLight\|ewowi/projectMM-v' . | grep -v '/.git/'   # must still say ewowi
grep -rn 'MoonModules/projectMM' . | grep -v '/.git/' | wc -l        # ~82
```
Run the spec check (the only gate for a docs/scripts change): `uv run scripts/check/check_specs.py`.

### 1d. Product-owner: push, merge, prove it
- Push `release-prep`; CodeRabbit re-reviews (now under MoonModules).
- Process findings, merge the open PR into `main`.
- Trigger a release run (the `latest` build on merge to main, or a test tag) and verify:
  - CI is green under MoonModules.
  - Pages deploys; `moonmodules.org/projectMM/install/` returns 200.
  - The installer still flashes a device — its URLs still say `ewowi.github.io` but **work via the 301 redirect**. (This is the proof that Stage 1 is complete without touching installer URLs.)

**Checkpoint: do not proceed to Stage 2 until the above is green.**

---

## Stage 2 — cut installer URLs to the canonical domain

The only CORS-sensitive change, isolated so it's testable on its own. A 301 redirect *can* trip a CORS preflight, so the installer's `boards.json` fetch must hit the final `moonmodules.org` URL directly, not via redirect.

### 2a. Agent: rewrite `ewowi.github.io/projectMM` → `moonmodules.org/projectMM`
```sh
grep -rl 'ewowi.github.io' . | grep -v '/.git/'   # ~9 files (docs/install/, src/ui/app.js, docs, scripts)
# replace exact string 'ewowi.github.io/projectMM' with 'moonmodules.org/projectMM'
grep -rn 'ewowi.github.io' . | grep -v '/.git/'    # must be empty afterwards
```
Key file: `src/ui/app.js` has `BOARDS_JSON_URL = "https://ewowi.github.io/projectMM/install/boards.json"` → must become `https://moonmodules.org/projectMM/install/boards.json`. Spec check, commit, push.

### 2b. Product-owner: test the installer specifically
- Open `https://moonmodules.org/projectMM/install/`.
- Confirm `boards.json` fetches CORS-clean (compare headers to the pre-transfer baseline above: 200 + `access-control-allow-origin: *`).
- Flash a real device end-to-end (board pick → firmware → flash → WiFi via Improv).

---

## Stage 3 — finish the v1.0.0 release (after Stage 2 is green)

Sequencing matters: the permanent v1.0.0 release body must carry the final `moonmodules.org` + `MoonModules/projectMM/v1.0.0` URLs, so publish **after** Stage 2.

1. **F2 URL swap** — set the draft release body to the `MoonModules/projectMM/v1.0.0` (and `moonmodules.org`) URLs:
   `gh release edit v1.0.0 --notes-file docs/history/release-notes-v1.0.0.md`
   (After the swap the draft's images look broken until publish, because the `v1.0.0` tag doesn't exist yet — expected.)
2. **Hardware test** (product-owner only) — ≥1 ESP32 + macOS + Windows on the 1.0.0 binaries.
3. **Publish** the draft (product-owner only) — creates the `v1.0.0` tag at `main`, with correct URLs from day one.

## State at the time this doc was written (2026-06-09)

- Remote still `ewowi/projectMM`; nothing transferred yet.
- `release-prep` is 3 commits ahead of `main`, open as **PR #15**.
- Draft release `v1.0.0` exists (untagged) with a `main`-URL preview body; `latest` pre-release exists; the rc1/rc2 releases were already deleted.
- The release-notes file `docs/history/release-notes-v1.0.0.md` currently holds `ewowi` + `v1.0.0`-pinned URLs (rewritten in Stages 1–2, then pushed into the release body at Stage 3 step 1).
