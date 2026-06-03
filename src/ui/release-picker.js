// projectMM release-channel picker — shared by the on-device UI (OTA flash)
// and the GitHub Pages installer (first flash via Web Serial).
//
// Same data source, two install transports:
//   - Device UI: caller POSTs the chosen .bin URL to /api/firmware/url; the
//     device's HTTPS client downloads + writes to the OTA partition. No CORS
//     in the data path.
//   - Web installer: caller sets the chosen manifest URL on
//     <esp-web-install-button>; ESP Web Tools flashes via Web Serial. Manifest
//     and binaries must be same-origin with the page (CORS), which is why the
//     Pages site self-hosts the last N releases.
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
// `relativeTime` — have been exercised ad-hoc from a DevTools console against
// the real GitHub Releases response and against synthesised assets. There is
// no JS unit-test harness in this codebase today (no jsdom, no node-based
// test runner); adding one is on the 2.0 roadmap (`docs/plan.md`). The C++
// frame parser at src/core/ImprovFrame.h + test/test_improv_frame.cpp is the
// equivalent test for the only piece of this work that runs on the device side.

// ---------------------------------------------------------------------------
// 1. Constants + module state
// ---------------------------------------------------------------------------

const API_URL = "https://api.github.com/repos/ewowi/projectMM/releases?per_page=10";
const CACHE_KEY = "projectMM.releases.v1";
const CACHE_TTL_MS = 5 * 60 * 1000;  // 5 min — short enough to surface new RCs, long enough to avoid rate-limit thrash

// Persisted user selection — survives page reloads and full browser restarts,
// so a returning user doesn't have to re-pick their firmware every time. Keyed
// separately from the API cache (which is sessionStorage with a TTL); these
// are intent, not data, and never expire on their own.
const PREF_RELEASE_KEY  = "projectMM.picker.releaseTag";
const PREF_FIRMWARE_KEY = "projectMM.picker.firmware";

// One picker instance per init() call. Each tracks its own state so multiple
// pickers on a page (unused today but possible) don't fight over selections.
function makeState() {
    return {
        container: null,
        ownFirmwareKey: null,
        onInstall: null,
        releases: [],          // normalised release records from the API
        sortedReleases: [],    // releases sorted newest-first; render() fills this
        releaseIdx: 0,         // index into sortedReleases
        firmware: null,        // selected firmware key
    };
}

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

    state.container.innerHTML = `
        <div class="control-row">
            <span class="control-label">Release</span>
            <select id="rp-release" class="rp-select"></select>
        </div>
        <div class="control-row">
            <span class="control-label">Firmware</span>
            <select id="rp-firmware" class="rp-select"></select>
        </div>
        <div class="control-row">
            <span class="control-label"></span>
            <button id="rp-install" class="action-btn" type="button">Install</button>
        </div>
        <div class="control-row rp-status-row">
            <span class="control-label"></span>
            <span id="rp-status" class="rp-status"></span>
        </div>
    `;

    const releaseEl = state.container.querySelector("#rp-release");
    const firmwareEl = state.container.querySelector("#rp-firmware");
    const installBtn = state.container.querySelector("#rp-install");
    const statusEl = state.container.querySelector("#rp-status");

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
        const r = sorted[state.releaseIdx];
        if (!r) {
            firmwareEl.innerHTML = `<option value="">—</option>`;
            installBtn.disabled = true;
            return;
        }
        const compatible = (r.firmwares || []).filter(f => isCompatible(state.ownFirmwareKey, f.firmware));
        if (compatible.length === 0) {
            // Distinguish two no-match reasons: device reports its firmware
            // but this release has nothing matching it, vs device's firmware
            // is "unknown" (build didn't propagate MM_FIRMWARE_NAME) and the
            // compatibility filter rejects everything. The latter is a build
            // bug — surface it so a developer can spot it.
            const reason = state.ownFirmwareKey === "unknown"
                ? "device firmware is 'unknown' — rebuild with MM_FIRMWARE_NAME set"
                : "no compatible firmwares in this release";
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
        // Prefer the user's last pick if it's still compatible with this release;
        // otherwise default to the first option (the existing behaviour).
        const savedFirmware = safeLocalGet(PREF_FIRMWARE_KEY);
        const savedHere = savedFirmware && compatible.find(f => f.firmware === savedFirmware);
        state.firmware = savedHere ? savedFirmware : compatible[0].firmware;
        firmwareEl.value = state.firmware;
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

    installBtn.addEventListener("click", async () => {
        const r = sorted[state.releaseIdx];
        if (!r || !state.firmware) return;
        const entry = (r.firmwares || []).find(f => f.firmware === state.firmware);
        if (!entry) return;
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

export const releasePicker = {
    /**
     * Mount a picker into the given container.
     * @param {object} opts
     * @param {HTMLElement} opts.container - DOM element to mount into.
     * @param {string|null} opts.ownFirmwareKey - device's MM_FIRMWARE_NAME for
     *   OTA compatibility filtering, or null for the web installer (no filter).
     * @param {(firmware: string, manifestUrl: string, binaryUrl: string) => Promise<void>} opts.onInstall
     *   - Called when the user clicks Install. The picker doesn't decide how
     *     to install — the caller decides.
     */
    async init({ container, ownFirmwareKey, onInstall }) {
        const state = makeState();
        state.container = container;
        state.ownFirmwareKey = ownFirmwareKey || null;
        state.onInstall = onInstall;

        container.innerHTML =
            `<div class="control-row"><span class="control-label">Releases</span>` +
            `<span class="rp-status">Loading…</span></div>`;
        const bypass = new URLSearchParams(location.search).get("nocache") === "1";
        const data = await loadReleases({ bypassCache: bypass });
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
};
