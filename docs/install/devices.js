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
// State shape: `[{ name, url, lastSeen }]` keyed under
// `projectMM.devices.v1` in localStorage. A schema bump (v2, …) is how
// future migrations land.

const STORAGE_KEY = "projectMM.devices.v1";

// Same hostile-storage guard release-picker.js uses. Duplicated for v1;
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
// release-picker.js's relativeTime. Duplicated for v1 (importing across
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
        const urlEl = document.createElement("div");
        urlEl.className = "device-url";
        urlEl.textContent = device.url;
        const seenEl = document.createElement("div");
        seenEl.className = "device-seen";
        seenEl.textContent = `Provisioned ${relativeTime(device.lastSeen)}`;
        info.append(nameEl, urlEl, seenEl);

        const actions = document.createElement("div");
        actions.className = "device-actions";
        const visit = makeBtn("Visit", () => {
            // noopener so the device-UI tab can't drive the install page.
            window.open(device.url, "_blank", "noopener");
        });
        const erase = makeBtn("Erase", () => {
            if (!confirm(
                `Erase ${device.name}? This wipes WiFi credentials and all ` +
                `module state. ESP Web Tools will offer to flash a fresh ` +
                `firmware after the erase — cancel that step if you only ` +
                `want to erase.`)) return;
            if (state.onErase) state.onErase(device);
        });
        const forget = makeBtn("Forget", () => {
            state.devices = state.devices.filter(d => d.url !== device.url);
            saveDevices(state.devices);
            render();
        });
        actions.append(visit, erase, forget);

        row.append(info, actions);
        state.container.appendChild(row);
    }
}

function makeBtn(label, handler) {
    const b = document.createElement("button");
    b.type = "button";
    b.className = "device-btn";
    b.textContent = label;
    b.addEventListener("click", handler);
    return b;
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
     */
    addProvisionedDevice(url) {
        if (!url || typeof url !== "string") return;
        const existing = state.devices.find(d => d.url === url);
        const now = new Date().toISOString();
        if (existing) {
            existing.lastSeen = now;
        } else {
            state.devices.push({ name: nameFromUrl(url), url, lastSeen: now });
        }
        saveDevices(state.devices);
        render();
    },
};
