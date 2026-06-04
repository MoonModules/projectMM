// "Your devices" — a localStorage-backed list of projectMM devices the user
// has provisioned from this install page. Surface so a returning user can
// jump to their device UI without re-typing the IP / re-running Improv.
//
// Browser-only feature set:
//   - Visit  → window.open() of the saved URL, opens the device UI in a new tab.
//   - Erase  → Web Serial via ESP Web Tools' <esp-web-install-button erase-first>.
//   - Forget → drop the entry from localStorage.
//
// Diagnose intentionally lives on the device UI itself (same-origin), not
// here — Chrome's mixed-content blocker stops https://ewowi.github.io from
// fetch()-ing http://192.168.1.X. The device-side Diagnose button (in
// app.js) does the same job from the right side of the security boundary.
//
// State shape: `[{ name, url, lastSeen, board?, pendingBoard? }]` keyed under
// `projectMM.devices.v1` in localStorage. `board` is optional — entries
// from before Step 3 of the board-injection plan have no board field, and
// the render path treats it as absent. A schema bump (v2, …) is how future
// migrations land; additive fields like this one don't need one.
//
// `pendingBoard` carries a board name (the key into boards.json) the
// installer couldn't push directly — Improv RPC unavailable AND the in-
// orchestrator HTTP fallback blocked by mixed-content. It only influences
// the *styling* of the **Inject** button (primary-flavoured when present);
// the button itself renders whenever the entry has a `board` field at all.
// Re-clicks are idempotent (the device just re-writes the same `controls.*`
// values), so we never gate the button on a one-shot flag — popup blockers,
// mistyped URLs, or a follow-up boards.json edit all need the action to
// stay reachable. `acknowledgeBoardInject` clears `pendingBoard` after a
// click; the button stays, just neutral-styled.

const STORAGE_KEY = "projectMM.devices.v1";

// Same hostile-storage guard install-picker.js uses. Duplicated for v1;
// extract to a shared helper module if a third consumer lands.
function safeLocalGet(key) {
    try { return localStorage.getItem(key); } catch (_) { return null; }
}
function safeLocalSet(key, value) {
    try { localStorage.setItem(key, value); } catch (_) { /* ignore */ }
}

function loadDevices() {
    const raw = safeLocalGet(STORAGE_KEY);
    if (!raw) return [];
    try {
        const v = JSON.parse(raw);
        return Array.isArray(v) ? v : [];
    } catch (_) {
        // Corrupt blob — drop silently and start fresh. The trade-off:
        // a malformed saved state shouldn't leave the user with a broken
        // page; recovering is just "lose the list", not a crash.
        return [];
    }
}

function saveDevices(devices) {
    safeLocalSet(STORAGE_KEY, JSON.stringify(devices));
}

// Extract a friendly device name from the URL the Improv flow reports.
// The hostname is `MM-XXXX.local` for unrenamed devices, or `<user-given>.local`
// after a rename — both work as labels. Fall back to the raw URL when the
// shape is unexpected (e.g. a bare IP rather than mDNS).
function nameFromUrl(url) {
    try {
        const host = new URL(url).hostname;
        return host.endsWith(".local") ? host.slice(0, -".local".length) : host;
    } catch (_) {
        return url;
    }
}

// Compact "X ago" formatter — Intl.RelativeTimeFormat, same idiom as
// install-picker.js's relativeTime. Duplicated for v1 (importing across
// the installer-page <-> module-on-Pages boundary would mean a fetch on
// every render; the function is 15 lines, not worth it).
function relativeTime(iso) {
    if (!iso) return "";
    const fmt = new Intl.RelativeTimeFormat("en", { numeric: "auto" });
    const diffMs = Date.parse(iso) - Date.now();
    const absSec = Math.abs(diffMs) / 1000;
    if (absSec < 60)        return fmt.format(Math.round(diffMs / 1000), "second");
    if (absSec < 3600)      return fmt.format(Math.round(diffMs / 60000), "minute");
    if (absSec < 86400)     return fmt.format(Math.round(diffMs / 3600000), "hour");
    if (absSec < 86400 * 7) return fmt.format(Math.round(diffMs / 86400000), "day");
    return new Date(iso).toLocaleDateString();
}

// Module state.
const state = {
    container: null,
    devices: [],
    // Callback the host page passes in init({ onErase }). The host owns
    // the <esp-web-install-button> instantiation because it has the
    // last-installed manifest URL in scope; we just hand it the device
    // back when the user clicks Erase.
    onErase: null,
};

function render() {
    if (!state.container) return;
    if (state.devices.length === 0) {
        state.container.innerHTML =
            `<p class="note">No devices yet. Provision one above and it'll show up here.</p>`;
        return;
    }
    state.container.innerHTML = "";
    for (const device of state.devices) {
        const row = document.createElement("div");
        row.className = "control-row device-row";
        // Name + URL + last-seen on the left, action buttons on the right.
        // Plain DOM construction (no innerHTML on user-controllable data —
        // device names can be user-renamed, treat as untrusted).
        const info = document.createElement("div");
        info.className = "device-info";
        const nameEl = document.createElement("strong");
        nameEl.textContent = device.name;
        // URL renders as an <a> so the user can click straight through —
        // duplicates the Visit button but the URL is the affordance most
        // users reach for. noopener so the device-UI tab can't drive the
        // installer page.
        const urlEl = document.createElement("a");
        urlEl.className = "device-url";
        urlEl.href = device.url;
        urlEl.target = "_blank";
        urlEl.rel = "noopener";
        urlEl.textContent = device.url;
        const seenEl = document.createElement("div");
        seenEl.className = "device-seen";
        seenEl.textContent = `Provisioned ${relativeTime(device.lastSeen)}`;
        info.append(nameEl, urlEl);
        // Board line (between URL and last-seen) renders only when set —
        // legacy entries from before the field was added stay unchanged.
        // The orchestrator passes board into addProvisionedDevice() when
        // SET_BOARD succeeded; "(any board)" provisions skip the field.
        if (device.board) {
            const boardEl = document.createElement("div");
            boardEl.className = "device-board-name";
            boardEl.textContent = device.board;
            info.append(boardEl);
        }
        info.append(seenEl);

        const actions = document.createElement("div");
        actions.className = "device-actions";
        const visit = makeBtn("Visit", () => {
            // noopener so the device-UI tab can't drive the install page.
            window.open(device.url, "_blank", "noopener");
        }, "Open the device UI in a new tab");
        // Inject button: always rendered when the entry has a board name on
        // it (whether or not we still have a `pendingBoard` flag). Opens the
        // device UI with `?board=<name>` so the device's app.js fetches the
        // matching boards.json entry from Pages and POSTs each `controls.*`
        // field to `/api/control`. Re-clicks are idempotent (same value
        // written to the same controls), so we don't gate the button on a
        // one-shot flag — popup blockers, mistyped URLs, and "the device
        // rejected one field, retry after fixing boards.json" all need the
        // button to stay reachable. `pendingBoard` (set by the orchestrator
        // when the in-page HTTP push didn't succeed) only affects styling:
        // primary-flavoured when there's an unconfirmed push, neutral once
        // the user has actioned it once.
        if (device.board) {
            const labelName = device.pendingBoard || device.board;
            const inject = makeBtn("Inject", () => {
                if (!confirm(
                    `Open ${device.name} and inject the board config for ` +
                    `"${labelName}"?\n\n` +
                    `The device will fetch the matching entry from boards.json ` +
                    `and apply every field via /api/control. Safe to re-run — ` +
                    `the values are idempotent.`)) return;
                window.open(buildInjectUrl(device), "_blank", "noopener");
                acknowledgeBoardInject(device);
            }, `Push the boards.json config for "${labelName}" to the device`);
            if (device.pendingBoard) inject.classList.add("primary");
            actions.append(inject);
        }
        const erase = makeBtn("Erase", () => {
            if (!confirm(
                `Erase ${device.name}? This wipes WiFi credentials and all ` +
                `module state. ESP Web Tools will offer to flash a fresh ` +
                `firmware after the erase — cancel that step if you only ` +
                `want to erase.`)) return;
            if (state.onErase) state.onErase(device);
        }, "Wipe the device's flash over USB — needs a fresh install afterwards");
        const forget = makeBtn("Forget", () => {
            state.devices = state.devices.filter(d => d.url !== device.url);
            saveDevices(state.devices);
            render();
        }, "Remove this entry from your list — the device itself is untouched");
        actions.append(visit, erase, forget);

        row.append(info, actions);
        state.container.appendChild(row);
    }
}

function makeBtn(label, handler, title) {
    const b = document.createElement("button");
    b.type = "button";
    b.className = "device-btn";
    b.textContent = label;
    if (title) b.title = title;
    b.addEventListener("click", handler);
    return b;
}

// Build `<device.url>?board=<name>` for the Inject button. The device UI's
// `consumePendingBoardParam()` reads the param, fetches the matching entry
// from boards.json on Pages, and POSTs each `controls.*` field to the
// device's `/api/control`. URLSearchParams handles encoding so names with
// spaces (e.g. "Olimex ESP32-Gateway Rev G") round-trip cleanly.
// `pendingBoard` is the name the orchestrator couldn't push directly;
// after the first Inject click that flag clears, but the button stays
// reachable and re-injects using the persistent `board` field.
function buildInjectUrl(device) {
    const name = device.pendingBoard || device.board;
    if (!name) return device.url;
    try {
        const u = new URL(device.url);
        u.searchParams.set("board", name);
        return u.toString();
    } catch (_) {
        return device.url;
    }
}

// Single-shot: once the user clicks Inject, drop `pendingBoard` from the
// entry so the button doesn't reappear next time. The fetch + fan-out on
// the device side either succeeded (board fields applied) or failed
// (network error, BoardModule validation rejected a value) — either way
// we don't auto-retry; the user re-adds via a fresh install or sets the
// fields manually via MoonDeck.
function acknowledgeBoardInject(device) {
    if (!device.pendingBoard) return;
    const stored = state.devices.find(d => d.url === device.url);
    if (!stored) return;
    delete stored.pendingBoard;
    saveDevices(state.devices);
    render();
}

export const myDevices = {
    /**
     * Mount the device list into the given container.
     * @param {object} opts
     * @param {HTMLElement} opts.container
     * @param {(device: {name, url, lastSeen}) => void} [opts.onErase]
     *   Called when the user clicks Erase on a row. The host page builds
     *   the <esp-web-install-button erase-first> because it owns the
     *   last-installed manifest URL.
     */
    init({ container, onErase }) {
        state.container = container;
        state.onErase = onErase || null;
        state.devices = loadDevices();
        render();
    },

    /**
     * Add (or refresh) a device the user just provisioned. URL is the
     * post-Improv success URL — typically `http://MM-XXXX.local/` or
     * `http://<ip>/` depending on the firmware.
     * @param {string} url
     * @param {string} [board] - physical board name from the picker
     *   (Step 3 of the board-injection plan). Empty / undefined = user
     *   picked "(any board)" or the SET_BOARD RPC was skipped; the bookmark
     *   row omits the board line. Non-empty updates an existing entry's
     *   board on re-flash; never blanks a previously-set value.
     * @param {object} [opts]
     * @param {boolean} [opts.pendingBoardPush] - true when the installer
     *   couldn't push the board itself (HTTPS Pages → HTTP device blocked
     *   by mixed-content). Renders the row's Inject button with the
     *   primary style and seeds `pendingBoard` so the user knows a push
     *   is needed. Ignored when `board` is empty (nothing to push).
     */
    addProvisionedDevice(url, board, opts) {
        if (!url || typeof url !== "string") return;
        // Restrict to http/https — the Visit button does window.open(url),
        // which would happily launch javascript: or file: URLs if a future
        // state migration or hand-edited localStorage smuggled one in. The
        // Improv success URL is always http://<ip>/, so this check rejects
        // nothing legitimate.
        let parsed;
        try { parsed = new URL(url); } catch (_) { return; }
        if (parsed.protocol !== "http:" && parsed.protocol !== "https:") return;
        const pendingBoardPush = !!(opts && opts.pendingBoardPush && board);
        const existing = state.devices.find(d => d.url === url);
        const now = new Date().toISOString();
        if (existing) {
            existing.lastSeen = now;
            // Only overwrite board when caller supplied a value — re-flashing
            // with "(any board)" mustn't blank a previously-set entry.
            if (board) existing.board = board;
            if (pendingBoardPush) existing.pendingBoard = board;
        } else {
            const entry = {
                name: nameFromUrl(url), url, lastSeen: now,
                board: board || "",
            };
            if (pendingBoardPush) entry.pendingBoard = board;
            state.devices.push(entry);
        }
        saveDevices(state.devices);
        render();
    },
};
