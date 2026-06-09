// projectMM install picker — shared by the on-device UI (OTA flash) and the
// GitHub Pages installer (first flash via Web Serial). Renders Release +
// Board + Firmware dropdowns and an Install button; the caller wires the
// onInstall callback to the right install transport.
//
// Same data source, two install transports:
//   - Device UI: caller POSTs the chosen .bin URL to /api/firmware/url; the
//     device's HTTPS client downloads + writes to the OTA partition. No CORS
//     in the data path.
//   - Web installer: caller hands the chosen manifest URL to the custom
//     install-orchestrator.js (which drives esptool-js + improv-wifi-serial-sdk
//     over Web Serial). Manifest and binaries must be same-origin with the
//     page (CORS), which is why the Pages site self-hosts the last N releases.
//
// The picker is a presentation+state machine; it does not decide *how* to
// install. The caller passes an onInstall(firmware, manifestUrl, binaryUrl)
// callback and wires it to the right transport.
//
// "Firmware" here is the compiled binary variant (chip + radios + sdkconfig
// fragments), not the physical board. See docs/architecture.md § Firmware
// vs board. Release assets are named per firmware variant
// (firmware-<variant>-v<ver>.bin, manifest-<variant>.json).
//
// Sections (top to bottom):
//   1. Constants + module state
//   2. GitHub Releases API + sessionStorage cache
//   3. Asset parsing (manifest-*.json → firmware, firmware-*.bin → binary)
//   4. Compatibility filter (OTA only)
//   5. Relative-time helper
//   6. DOM construction + event wiring
//
// Tests: the pure helpers below — `isCompatible`, `parseFirmwaresFromAssets`,
// `relativeTime` — are exercised ad-hoc from a DevTools console against the
// real GitHub Releases response and against synthesised assets. The repo
// has no JS unit-test harness (no jsdom, no node-based runner). The C++
// frame parser at src/core/ImprovFrame.h + test/test_improv_frame.cpp is
// the equivalent test for the only piece of this work that runs on the
// device side.

// ---------------------------------------------------------------------------
// 1. Constants + module state
// ---------------------------------------------------------------------------

const API_URL = "https://api.github.com/repos/MoonModules/projectMM/releases?per_page=10";
const CACHE_KEY = "projectMM.releases.v1";
const CACHE_TTL_MS = 5 * 60 * 1000;  // 5 min — short enough to surface new RCs, long enough to avoid rate-limit thrash

// Persisted user selection — survives page reloads and full browser restarts,
// so a returning user doesn't have to re-pick their firmware every time. Keyed
// separately from the API cache (which is sessionStorage with a TTL); these
// are intent, not data, and never expire on their own.
const PREF_RELEASE_KEY  = "projectMM.picker.releaseTag";
const PREF_FIRMWARE_KEY = "projectMM.picker.firmware";
const PREF_BOARD_KEY    = "projectMM.picker.board";

// One picker instance per init() call. Each tracks its own state so multiple
// pickers on a page (unused today but possible) don't fight over selections.
function makeState() {
    return {
        container: null,
        ownFirmwareKey: null,
        onInstall: null,
        onDetect: null,        // web installer only: opens serial, returns the
                               // detected chip-family string ("ESP32" / "ESP32-S3").
                               // null on on-device OTA (no local serial), which is
                               // also what gates the Detect button off there.
        detectedChip: null,    // chip family from the last successful Detect, or null
        runDetect: null,       // render() fills this with the detect routine when
                               // the Detect button is present; runDetect() public
                               // method calls it (host's auto-fire on port grant)
        enableBoardPicker: true, // true on web installer, false on on-device OTA
        // Optional caller-owned DOM element rendered just above the Install
        // button row. The web installer uses this to slot its "Erase chip
        // first" checkbox between the firmware dropdown and the Install
        // button without giving the picker any erase-specific knowledge —
        // keeps the picker reusable on the on-device OTA UI (where erase
        // makes no sense). The picker takes ownership: it appendChild's
        // the node on every render(), so the same instance survives the
        // re-renders triggered by release-list reloads.
        installRowExtras: null,
        releases: [],          // normalised release records from the API
        sortedReleases: [],    // releases sorted newest-first; render() fills this
        releaseIdx: 0,         // index into sortedReleases
        firmware: null,        // selected firmware key
        boards: [],            // parsed docs/install/boards.json, [] if unavailable
        selectedBoard: null,   // user pick from board <select>; "" for (any board)
    };
}

// Module-level handle to the most recently mounted picker's state, so the
// host page can call installPicker.getSelectedBoard() without threading the
// state object through every callback. Web installer mounts exactly one
// picker per page; if a future page mounts multiple, this becomes wrong
// (returns whichever initialized last). See comment at makeState — pickers
// are otherwise isolated.
let _lastState = null;

// ---------------------------------------------------------------------------
// 2. GitHub Releases API + sessionStorage cache
// ---------------------------------------------------------------------------

// Safe sessionStorage accessor — private/incognito Safari throws on
// getItem/setItem entirely, not just quota. Wrap every access so a host
// browser with storage disabled is a cache miss, not an exception.
function safeStorageGet(key) {
    try { return sessionStorage.getItem(key); } catch (_) { return null; }
}
function safeStorageSet(key, value) {
    try { sessionStorage.setItem(key, value); } catch (_) { /* ignore */ }
}

// Same wrappers for localStorage — the persisted user selection (release tag,
// firmware) survives across browser sessions. Same try/catch shape so a hostile
// storage backend is a "no preference" miss, not an exception.
function safeLocalGet(key) {
    try { return localStorage.getItem(key); } catch (_) { return null; }
}
function safeLocalSet(key, value) {
    try { localStorage.setItem(key, value); } catch (_) { /* ignore */ }
}

// Returns the parsed releases array, or null on hard failure (network, 403).
// Stale cache is served on transient failures so the dropdown doesn't go empty.
async function loadReleases({ bypassCache = false } = {}) {
    if (!bypassCache) {
        const raw = safeStorageGet(CACHE_KEY);
        if (raw) try {
            const obj = JSON.parse(raw);
            if (Date.now() - obj.ts < CACHE_TTL_MS) return obj.data;
        } catch (_) { /* corrupt cache — fall through to fetch */ }
    }
    try {
        const res = await fetch(API_URL);
        if (!res.ok) {
            // 403 = anonymous rate-limit (60/hr). Fall back to stale cache if any.
            const raw = safeStorageGet(CACHE_KEY);
            if (raw) try { return JSON.parse(raw).data; } catch (_) { /* fall through */ }
            return null;
        }
        const data = await res.json();
        safeStorageSet(CACHE_KEY, JSON.stringify({ ts: Date.now(), data }));
        return data;
    } catch (_) {
        const raw = safeStorageGet(CACHE_KEY);
        if (raw) try { return JSON.parse(raw).data; } catch (_) { /* fall through */ }
        return null;
    }
}

// Boards catalog — same-origin docs/install/boards.json. ~1 KB, no rate-limit
// concern (CDN serves it on the public site, preview_installer serves it
// from disk locally), so no sessionStorage cache: caching adds invalidation
// bugs without saving bytes. Graceful degradation: any fetch / parse failure
// returns [] and the picker silently omits the board <select>.
async function loadBoards() {
    try {
        const res = await fetch("./boards.json");
        if (!res.ok) return [];
        const data = await res.json();
        return Array.isArray(data) ? data : [];
    } catch (_) {
        return [];
    }
}

// ---------------------------------------------------------------------------
// 3. Asset parsing
// ---------------------------------------------------------------------------

// Each release's `assets[]` contains both manifest-<firmware>.json and
// firmware-<firmware>-v<ver>(.bin|-bootloader.bin|-partition-table.bin|-ota-data.bin).
// The picker needs: per-firmware → {manifestUrl, binaryUrl}. Manifest URL drives
// ESP Web Tools (web installer); binary URL drives /api/firmware/url (device OTA).
function parseFirmwaresFromAssets(assets, tag) {
    if (!assets) return [];
    const firmwares = new Map();
    const manifestRe = /^manifest-(.+)\.json$/;
    const binaryRe = /^firmware-(.+)-v.+?\.bin$/;  // app .bin only (no -bootloader/-partition-table/-ota-data)

    for (const a of assets) {
        const m = manifestRe.exec(a.name);
        if (m) {
            const firmware = m[1];
            const entry = firmwares.get(firmware) || { firmware, manifestUrl: null, binaryUrl: null };
            entry.manifestUrl = a.browser_download_url;
            firmwares.set(firmware, entry);
        }
    }
    for (const a of assets) {
        // Reject the part-suffixed .bins (bootloader / partition-table / ota-data)
        // — they're install fragments, not the main image. The OTA path needs the
        // app image only; esp_https_ota internally fetches what it needs.
        if (/-(?:bootloader|partition-table|ota-data)\.bin$/.test(a.name)) continue;
        const m = binaryRe.exec(a.name);
        if (m) {
            const firmware = m[1];
            const entry = firmwares.get(firmware) || { firmware, manifestUrl: null, binaryUrl: null };
            entry.binaryUrl = a.browser_download_url;
            firmwares.set(firmware, entry);
        }
    }
    // Only return firmwares that have BOTH a manifest and a binary — partial uploads
    // (mid-release-publish race) shouldn't appear in the dropdown.
    return Array.from(firmwares.values()).filter(f => f.manifestUrl && f.binaryUrl);
}

// ---------------------------------------------------------------------------
// 4. Compatibility filter (OTA only)
// ---------------------------------------------------------------------------

// Bespoke rule for projectMM's firmware keys: strip the `-eth*` suffix from
// both sides; equal identities are mutually OTA-compatible. So `esp32`,
// `esp32-eth`, and `esp32-eth-wifi` can all flash each other (same physical
// ESP32 silicon; the variant decides which radios are compiled in).
// `esp32s3-n16r8` is only compatible with itself (different chip family AND
// different partition table). Web installer passes ownFirmwareKey=null →
// all candidates compatible.
function isCompatible(ownFirmwareKey, candidateFirmwareKey) {
    if (!ownFirmwareKey) return true;
    const strip = f => f.replace(/-eth.*$/, "");
    return strip(ownFirmwareKey) === strip(candidateFirmwareKey);
}

// ---------------------------------------------------------------------------
// 5. Relative-time helper
// ---------------------------------------------------------------------------

// "2 days ago", "in 3 hours", etc. Uses Intl.RelativeTimeFormat (Chrome 71+,
// every modern browser). 15 lines, no external library.
const rtf = new Intl.RelativeTimeFormat("en", { numeric: "auto" });
function relativeTime(iso) {
    if (!iso) return "";
    const diffMs = new Date(iso) - new Date();
    const units = [
        ["year",   365 * 24 * 60 * 60 * 1000],
        ["month",   30 * 24 * 60 * 60 * 1000],
        ["week",     7 * 24 * 60 * 60 * 1000],
        ["day",          24 * 60 * 60 * 1000],
        ["hour",              60 * 60 * 1000],
        ["minute",                 60 * 1000],
    ];
    for (const [unit, ms] of units) {
        if (Math.abs(diffMs) >= ms) return rtf.format(Math.round(diffMs / ms), unit);
    }
    return rtf.format(Math.round(diffMs / 1000), "second");
}

// ---------------------------------------------------------------------------
// 6. DOM construction + event wiring
// ---------------------------------------------------------------------------

// Rebuild a board <select>: a leading pass-through option (label varies by
// context) followed by one option per board. Shared by render() (full catalog)
// and applyDetectedChip() (chip-narrowed list) so the option-building shape
// lives in one place. Pure DOM — no serial, no network: safe in the firmware
// build (this file embeds into the device via embed_ui.cmake).
function fillBoardOptions(boardEl, boards, passthroughLabel) {
    boardEl.replaceChildren();
    const any = document.createElement("option");
    any.value = "";
    any.textContent = passthroughLabel;
    boardEl.appendChild(any);
    for (const b of boards) {
        const opt = document.createElement("option");
        opt.value = b.name;
        opt.textContent = b.name;
        boardEl.appendChild(opt);
    }
}

// After a successful Detect, narrow the board <select> to ONLY the boards whose
// `chip` matches the detected family — the other family is removed from the list
// entirely (plug in an S3, the classic-ESP32 boards disappear, and vice versa).
// The pass-through is relabelled "Other / generic <chip>" so a user with a board
// not in the catalog can still flash the right firmware for their silicon. The
// returned status string is shown next to the Detect button.
// Detection gives chip FAMILY only — it can't tell esp32 / esp32-eth /
// esp32-eth-wifi apart (same silicon, different wiring), so when several boards
// share the family we narrow + let the user pick rather than guessing.
function applyDetectedChip(state, boardEl) {
    const matches = state.boards.filter(b => b.chip === state.detectedChip);
    if (matches.length === 0) {
        // A chip we ship no board for: don't strand the user — leave the full
        // list and report it. (selectedBoard unchanged.)
        return `Detected ${state.detectedChip} — no matching board, pick manually`;
    }
    fillBoardOptions(boardEl, matches, `Other / generic ${state.detectedChip}`);
    if (matches.length === 1) {
        state.selectedBoard = matches[0].name;   // exactly one board → auto-pick
    } else if (!matches.find(b => b.name === state.selectedBoard)) {
        // Several boards in this family; the prior pick (if any) belongs to the
        // other family — clear it back to the generic pass-through so we don't
        // carry a now-hidden selection.
        state.selectedBoard = "";
    }
    boardEl.value = state.selectedBoard || "";
    return matches.length === 1
        ? `Detected ${state.detectedChip} — selected ${matches[0].name}`
        : `Detected ${state.detectedChip} — pick your board (${matches.length} match)`;
}

// Builds the picker UI into `state.container`. Idempotent — calling more than
// once just rebuilds.
//
// Uses the same `.control-row` / `.control-label` / `<select>` shape as the
// rest of `createControl()` in app.js so the picker visually integrates with
// the card it's mounted in. The web installer overrides these with its own
// styles in docs/install/index.html, which gives the installer page the same
// look without app.js loading.
function render(state) {
    // newest-first; releases without `published_at` (drafts) sort last.
    const sorted = state.releases.slice().sort((a, b) => {
        const aT = a.published_at ? Date.parse(a.published_at) : 0;
        const bT = b.published_at ? Date.parse(b.published_at) : 0;
        return bT - aT;
    });
    state.sortedReleases = sorted;

    // Row order: Release → Board → Firmware. Release first because it's the
    // version the user wants to flash (the picker's primary identity);
    // Board second so the firmware narrowing happens in front of Firmware;
    // Firmware last so it shows the narrowed list immediately below the
    // board that filtered it. The board row only renders when (a) the
    // caller didn't opt out and (b) the catalog actually loaded —
    // on-device OTA passes enableBoardPicker:false (the device already
    // knows its board); catalog-missing on the web installer (rare) falls
    // back to a two-row Release+Firmware layout with no board narrowing.
    const boardRow = (state.enableBoardPicker && state.boards.length > 0) ? `
        <div class="control-row">
            <span class="control-label">Board</span>
            <select id="rp-board" class="rp-select"></select>
        </div>` : "";
    state.container.innerHTML = `
        <div class="control-row">
            <span class="control-label">Release</span>
            <select id="rp-release" class="rp-select"></select>
        </div>` + boardRow + `
        <div class="control-row">
            <span class="control-label">Firmware</span>
            <select id="rp-firmware" class="rp-select"></select>
        </div>
        <div class="control-row" id="rp-install-row">
            <span class="control-label"></span>
            <button id="rp-install" class="action-btn" type="button">Install</button>
        </div>
        <div class="control-row rp-status-row">
            <span class="control-label"></span>
            <span id="rp-status" class="rp-status"></span>
        </div>
    `;
    // installRowExtras: caller-owned element slotted right before the
    // Install row. Re-attached on every render() so the row survives the
    // innerHTML reset above. insertBefore moves the existing DOM node
    // rather than cloning — the caller wires the listeners once and they
    // keep firing across renders.
    if (state.installRowExtras) {
        const installRow = state.container.querySelector("#rp-install-row");
        state.container.insertBefore(state.installRowExtras, installRow);
    }

    const boardEl = state.container.querySelector("#rp-board");
    const releaseEl = state.container.querySelector("#rp-release");
    const firmwareEl = state.container.querySelector("#rp-firmware");
    const installBtn = state.container.querySelector("#rp-install");
    const statusEl = state.container.querySelector("#rp-status");

    if (boardEl) {
        // Full catalog, no chip filter yet. The "(any board)" pass-through
        // means the firmware dropdown shows every compatible firmware, just
        // as if the board picker didn't exist.
        fillBoardOptions(boardEl, state.boards, "(any board)");
        // Restore the user's last picked board if it's still in the catalog
        // (the catalog may have changed since their last visit; falling
        // through to "(any board)" if their pick is gone is the safe shape).
        const savedBoard = safeLocalGet(PREF_BOARD_KEY);
        if (savedBoard && state.boards.find(b => b.name === savedBoard)) {
            state.selectedBoard = savedBoard;
        }
        boardEl.value = state.selectedBoard || "";
    }

    // One option per release, newest-first. RC tags carry a "(beta)" suffix
    // and a different colour so a casual user can't mistake them for a
    // production release. The compatibility filter at the firmware step
    // handles the "is this binary for my chip?" question, so the release
    // dropdown doesn't pre-filter on it — a user can still see every release
    // that exists, even ones whose binaries don't match their firmware.
    //
    // Use textContent rather than innerHTML so a tag name that contains
    // HTML (a compromised release tag, a typo with a literal `<`) renders
    // as text instead of getting injected. r.tag_name is GitHub-API-supplied
    // — outside our control once the picker fetches it.
    releaseEl.replaceChildren();
    sorted.forEach((r, i) => {
        const opt = document.createElement("option");
        opt.value = String(i);
        const flag = r.prerelease ? " (beta)" : "";
        const age = relativeTime(r.published_at);
        opt.textContent = `${r.tag_name}${flag} — ${age}`;
        releaseEl.appendChild(opt);
    });

    // Default selection order:
    //   1. Last release tag the user picked, if it's still in the list.
    //   2. Newest stable.
    //   3. Newest prerelease (falls through when no stable exists yet).
    const savedTag = safeLocalGet(PREF_RELEASE_KEY);
    const savedIdx = savedTag ? sorted.findIndex(r => r.tag_name === savedTag) : -1;
    const firstStable = sorted.findIndex(r => !r.prerelease);
    state.releaseIdx = savedIdx >= 0 ? savedIdx
                     : firstStable >= 0 ? firstStable
                     : 0;
    releaseEl.value = String(state.releaseIdx);

    function refreshFirmwareDropdown() {
        firmwareEl.disabled = false;  // re-enable in case prior state had a single-firmware board
        const r = sorted[state.releaseIdx];
        if (!r) {
            firmwareEl.innerHTML = `<option value="">—</option>`;
            installBtn.disabled = true;
            return;
        }
        let compatible = (r.firmwares || []).filter(f => isCompatible(state.ownFirmwareKey, f.firmware));
        // Narrow by selected board (web installer only — selectedBoard stays
        // null on the on-device picker since the board <select> isn't rendered).
        // Defensive: a board the user picked that isn't in the catalog (e.g.
        // catalog edited mid-session) skips the narrow — better than rejecting
        // every firmware silently.
        if (state.selectedBoard) {
            const board = state.boards.find(b => b.name === state.selectedBoard);
            if (board) {
                compatible = compatible.filter(f => board.firmwares.includes(f.firmware));
            }
        }
        if (compatible.length === 0) {
            // Distinguish the no-match reasons:
            //   - device firmware "unknown" (build didn't propagate
            //     MM_FIRMWARE_NAME) → a build bug; surface it for the dev.
            //   - a board whose firmware exists in ANOTHER release we can see
            //     but not the selected one → a newer firmware variant (e.g. a
            //     board added after the last stable). Point the user at the
            //     release that has it instead of dead-ending. This is exactly
            //     the case for boards added between releases: their binary only
            //     lands in `latest` / the next tag, not the older stable.
            //   - otherwise → genuinely no build for this board/firmware.
            let reason;
            if (state.ownFirmwareKey === "unknown") {
                reason = "device firmware is 'unknown' — rebuild with MM_FIRMWARE_NAME set";
            } else if (state.selectedBoard) {
                const board = state.boards.find(b => b.name === state.selectedBoard);
                const wanted = board ? board.firmwares : [];
                // Newest other release whose assets include a firmware this board needs.
                const elsewhere = sorted.find((rel, idx) =>
                    idx !== state.releaseIdx
                    && (rel.firmwares || []).some(f => wanted.includes(f.firmware)));
                reason = elsewhere
                    ? `${state.selectedBoard} needs a newer release — select ${elsewhere.tag_name}${elsewhere.prerelease ? " (beta)" : ""} above`
                    : `no compatible firmware for ${state.selectedBoard} in this release`;
            } else {
                reason = "no compatible firmwares in this release";
            }
            firmwareEl.innerHTML = `<option value="">— ${reason} —</option>`;
            installBtn.disabled = true;
            return;
        }
        // Same XSS guard as the release dropdown: textContent over innerHTML.
        // f.firmware comes from parseFirmwaresFromAssets parsing GitHub asset
        // names with a strict regex, so the risk is lower here, but
        // consistency wins.
        firmwareEl.replaceChildren();
        compatible.forEach(f => {
            const opt = document.createElement("option");
            opt.value = f.firmware;
            opt.textContent = f.firmware;
            firmwareEl.appendChild(opt);
        });
        // Precedence: own firmware > last user pick > board default > first
        // compatible.
        //   1. The device's currently-flashed firmware (ownFirmwareKey) wins
        //      because the OTA picker's natural default is "re-flash what
        //      I'm running" — even if last week the user flashed something
        //      else. Only present on the on-device picker; the web installer
        //      passes null so this branch falls through.
        //   2. localStorage saved pick wins next: a returning user expects
        //      their last choice to stick across page loads, including the
        //      case where they hit board.default_firmware once but actually
        //      want a non-default variant (e.g. esp32-eth-wifi on Olimex,
        //      where the catalog's default is esp32-eth). Filtered through
        //      `compatible` so a stale saved value (release dropped that
        //      firmware) falls through harmlessly.
        //   3. The board's default_firmware — fallback for first-time
        //      visitors who haven't picked anything yet.
        //   4. First option in the narrowed list — last-resort fallback.
        const savedFirmware = safeLocalGet(PREF_FIRMWARE_KEY);
        const savedHere = savedFirmware && compatible.find(f => f.firmware === savedFirmware);
        let preferred = null;
        if (state.ownFirmwareKey && compatible.find(f => f.firmware === state.ownFirmwareKey)) {
            preferred = state.ownFirmwareKey;
        } else if (savedHere) {
            preferred = savedFirmware;
        } else if (state.selectedBoard) {
            const board = state.boards.find(b => b.name === state.selectedBoard);
            if (board && board.default_firmware
                && compatible.find(f => f.firmware === board.default_firmware)) {
                preferred = board.default_firmware;
            }
        }
        state.firmware = preferred || compatible[0].firmware;
        firmwareEl.value = state.firmware;
        // Single-firmware UX: when the narrow leaves exactly one option, the
        // <select> reads as a fixed badge (user sees what's being flashed but
        // can't change it — there's nothing to change to). Re-enabled at the
        // top of refreshFirmwareDropdown for the next call.
        firmwareEl.disabled = (compatible.length === 1);
        installBtn.disabled = false;
    }
    refreshFirmwareDropdown();

    releaseEl.addEventListener("change", () => {
        state.releaseIdx = Number(releaseEl.value);
        // Persist the tag name (not the index) — indexes shift when new releases
        // land, but tag names are stable identifiers.
        const r = sorted[state.releaseIdx];
        if (r) safeLocalSet(PREF_RELEASE_KEY, r.tag_name);
        refreshFirmwareDropdown();
    });

    firmwareEl.addEventListener("change", () => {
        state.firmware = firmwareEl.value;
        safeLocalSet(PREF_FIRMWARE_KEY, state.firmware);
    });

    if (boardEl) {
        // Picking a board narrows the firmware dropdown and may pre-select
        // the board's default_firmware. Persisted to localStorage so a
        // returning user (who usually flashes the same board over and over)
        // doesn't have to re-pick. Same rationale as PREF_FIRMWARE_KEY; if a
        // user is actually flashing a different board, they pick from the
        // dropdown and the new choice persists.
        boardEl.addEventListener("change", () => {
            state.selectedBoard = boardEl.value;
            safeLocalSet(PREF_BOARD_KEY, state.selectedBoard);
            refreshFirmwareDropdown();
        });
    }

    if (boardEl && state.onDetect) {
        // The detect routine: opens the serial port (via onDetect — the seam to
        // the serial/esptool code in install-orchestrator.js), reads the chip
        // family, narrows the board list to that family. The Detect BUTTON lives
        // in the host page (under the port picker), not here — the page calls
        // installPicker.runDetect() and shows the returned status string. This
        // keeps the button out of the firmware-embedded picker, and the
        // narrowing logic in the picker (which owns the board <select>).
        // Returns a status string for the caller to display ("" on success-less
        // states is never returned — applyDetectedChip always yields a message).
        state.runDetect = async (onStatus) => {
            if (onStatus) onStatus("Detecting…");
            // Clear any prior detection up front so a failed re-detect can't leave
            // the board list narrowed to a stale chip family (e.g. detect S3, then
            // a later detect fails on a wrong port — without this the list would
            // still hide the classic boards and claim an S3 was found).
            state.detectedChip = null;
            let status;
            try {
                state.detectedChip = await state.onDetect();   // "ESP32" | "ESP32-S3" | ...
                status = applyDetectedChip(state, boardEl);
            } catch (e) {
                // Restore the full, unfiltered board list — detection didn't land,
                // so don't keep any narrowing from a previous attempt.
                fillBoardOptions(boardEl, state.boards, "(any board)");
                state.selectedBoard = "";
                boardEl.value = "";
                status = `Detect failed: ${e && e.message ? e.message : e}`;
            }
            safeLocalSet(PREF_BOARD_KEY, state.selectedBoard || "");
            refreshFirmwareDropdown();
            if (onStatus) onStatus(status);
            return status;
        };
    }

    installBtn.addEventListener("click", async () => {
        const r = sorted[state.releaseIdx];
        if (!r || !state.firmware) return;
        const entry = (r.firmwares || []).find(f => f.firmware === state.firmware);
        if (!entry) return;
        // Mismatch guard: if Detect ran and the user then overrode the board to
        // one of a different chip family, confirm before flashing the wrong
        // binary (which would fail at the bootloader with a cryptic error).
        // Gated on detectedChip, which is only ever set on the web installer —
        // never reached on the on-device OTA build.
        if (state.detectedChip && state.selectedBoard) {
            const board = state.boards.find(b => b.name === state.selectedBoard);
            if (board && board.chip && board.chip !== state.detectedChip
                && !confirm(`You picked ${state.selectedBoard} (${board.chip}) but the connected device is ${state.detectedChip}. Flash anyway?`)) {
                return;
            }
        }
        // Install click is the strongest "yes, this is my choice" signal —
        // remember it explicitly in addition to the on-change writes above, in
        // case the user reaches this point without having interacted with the
        // dropdowns (defaults restored, click straight through).
        safeLocalSet(PREF_RELEASE_KEY, r.tag_name);
        safeLocalSet(PREF_FIRMWARE_KEY, state.firmware);
        installBtn.disabled = true;
        statusEl.textContent = `Installing ${r.tag_name} (${state.firmware})…`;
        try {
            await state.onInstall(state.firmware, entry.manifestUrl, entry.binaryUrl);
            statusEl.textContent = `Install request sent — watch device status for progress.`;
        } catch (e) {
            statusEl.textContent = `Error: ${e && e.message ? e.message : e}`;
        } finally {
            installBtn.disabled = false;
        }
    });
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

export const installPicker = {
    /**
     * Mount a picker into the given container.
     * @param {object} opts
     * @param {HTMLElement} opts.container - DOM element to mount into.
     * @param {string|null} opts.ownFirmwareKey - device's MM_FIRMWARE_NAME for
     *   OTA compatibility filtering, or null for the web installer (no filter).
     * @param {(firmware: string, manifestUrl: string, binaryUrl: string) => Promise<void>} opts.onInstall
     *   - Called when the user clicks Install. The picker doesn't decide how
     *     to install — the caller decides.
     * @param {() => Promise<string>} [opts.onDetect] - web installer only:
     *   opens the serial port and returns the connected chip-family string
     *   ("ESP32" / "ESP32-S3"). When provided, the picker renders a "Detect
     *   my board" button that narrows the board list to the detected family.
     *   Omit on the on-device OTA picker (no local serial) — the button then
     *   never renders. All serial/esptool work lives behind this callback.
     * @param {boolean} [opts.enableBoardPicker=true] - true on the web
     *   installer (renders a board <select> above firmware, narrows firmware
     *   list to the board's compatible variants); false on the on-device OTA
     *   picker where the device already knows its board (BoardModule).
     * @param {HTMLElement} [opts.installRowExtras] - optional caller-owned
     *   element rendered just above the Install button row. Web installer
     *   uses this for the "Erase chip first" checkbox; on-device OTA omits
     *   it. The picker re-attaches the SAME node on every render(), so
     *   listeners the caller wired on the element keep firing.
     */
    async init({ container, ownFirmwareKey, onInstall, onDetect = null,
                 enableBoardPicker = true, installRowExtras = null }) {
        const state = makeState();
        state.container = container;
        state.ownFirmwareKey = ownFirmwareKey || null;
        state.onInstall = onInstall;
        state.onDetect = onDetect;
        state.enableBoardPicker = enableBoardPicker;
        state.installRowExtras = installRowExtras;

        container.innerHTML =
            `<div class="control-row"><span class="control-label">Releases</span>` +
            `<span class="rp-status">Loading…</span></div>`;
        const bypass = new URLSearchParams(location.search).get("nocache") === "1";
        // Parallel: GitHub Releases API (slow, ~200ms) + local boards.json
        // (fast, ~5ms). Boards-disabled path skips the fetch entirely.
        const [data, boards] = await Promise.all([
            loadReleases({ bypassCache: bypass }),
            enableBoardPicker ? loadBoards() : Promise.resolve([]),
        ]);
        state.boards = boards;
        _lastState = state;
        if (!data) {
            container.innerHTML =
                `<div class="control-row"><span class="control-label">Releases</span>` +
                `<span class="rp-status">Couldn't reach GitHub — refresh to retry.</span></div>`;
            return;
        }
        // Normalise: enrich each release with its parsed firmwares list.
        state.releases = data.map(r => ({
            tag_name: r.tag_name,
            name: r.name || r.tag_name,
            prerelease: !!r.prerelease,
            published_at: r.published_at || r.created_at,
            html_url: r.html_url,
            firmwares: parseFirmwaresFromAssets(r.assets, r.tag_name),
        }))
        // Drop releases with zero usable firmwares (no firmware-* / manifest-* assets).
        .filter(r => r.firmwares.length > 0);

        if (state.releases.length === 0) {
            container.innerHTML =
                `<div class="control-row"><span class="control-label">Releases</span>` +
                `<span class="rp-status">No releases with installable firmware found.</span></div>`;
            return;
        }
        render(state);
    },

    /**
     * Returns the user-picked board name (catalog `name` field) from the
     * most recently mounted picker, or "" when the picker is in
     * "(any board)" mode, the catalog is unavailable, or the picker isn't
     * mounted yet. Used by the install-orchestrator to know what to push
     * via Improv SET_BOARD after WiFi provisioning succeeds.
     */
    getSelectedBoard() {
        return _lastState ? (_lastState.selectedBoard || "") : "";
    },

    /**
     * Chip family from the last successful Detect ("ESP32" / "ESP32-S3"), or ""
     * if Detect hasn't run / failed / isn't wired. Always "" on the on-device
     * OTA picker (no onDetect). Currently informational; the mismatch guard at
     * install time reads state.detectedChip directly.
     */
    getDetectedChip() {
        return _lastState ? (_lastState.detectedChip || "") : "";
    },

    /**
     * Detect the connected chip and narrow the board list to its family. Called
     * by the host's "Detect my board" button (under the port picker) and auto-
     * fired after a fresh port grant (the ESP Web Tools / ESPHome model: detect
     * immediately on connect). `onStatus(text)` is invoked with "Detecting…"
     * then the final status ("Detected ESP32-S3 — …" / "Detect failed: …") so
     * the page can show progress; the final string is also returned. No-op
     * returning "" if the picker isn't mounted or detect isn't wired (on-device
     * OTA). Never throws — failures come back as a status string.
     */
    async runDetect(onStatus) {
        if (_lastState && _lastState.runDetect) return await _lastState.runDetect(onStatus);
        return "";
    },
};
