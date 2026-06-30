// projectMM Web UI — all logic in one hand-maintained file per CLAUDE.md.
// Loaded as <script type="module"> so it can import the shared install-picker
// component used by both the device UI (here, OTA flash) and the GitHub Pages
// installer (first flash via Web Serial). Module loading is deferred by
// default; entry-point is the WS init at the bottom — no ordering surprises.
import { installPicker } from "/install-picker.js";
import { preview } from "/preview3d.js";
import { isNewer, parse } from "/semver.js";

// Sections (top to bottom):
//   1. State + storage
//   2. WebSocket (with keepalive, visibility pause, bfcache, exponential backoff)
//   3. REST helpers + module mutations
//   4. Render pipeline: render() → renderNav() → renderCards() → createCard() → createControl()
//   5. State patching (no-rebuild contract): updateValues() + updateModuleControls()
//   6. Type picker
//   7. Drag-to-reorder (HTML5 DnD on desktop; touchstart-gated on mobile)
//   (3D WebGL preview lives in preview3d.js — imported as `preview`)
//   8. Status bar wiring (device name, sys stats, theme, reboot)
//   9. Boot
//
// Load-bearing invariants:
//   - dragTs[mid:key] cooldown: ignore WS pushes for a control the user has touched
//     in the last 1s. Prevents slider snap-back during drag.
//   - ctrl.hidden: skip rendering hidden controls (plan-10 feature). Persistence still
//     loads them — toggling visibility doesn't lose state.
//   - No-rebuild contract: WS state updates patch values in place via querySelector.
//     We only rebuild the DOM on structural changes (add/delete/move) and explicit
//     select-driven onBuildControls rebuilds.

// ---------------------------------------------------------------------------
// 1. State + storage
// ---------------------------------------------------------------------------

let state = null;
let selectedModule = null;
let availableTypes = [];        // populated from GET /api/types after first connection
let ws = null;
let wsRetryMs = 500;             // exponential backoff: 500 → 1000 → 2000 → 4000 → 5000
let wsHeartbeat = null;
let wsPaused = false;            // gated by document.visibilityState

const dragTimers = {};           // per-control debounce timers (clearTimeout handles)
const dragTs = {};               // per-control last-touched timestamp (ms)
// Control types whose value the user can edit — updateModuleControls suppresses a
// WS state push for one of these while the user is mid-edit (see dragTs). The
// read-only types (display/display-int/time/progress) and the composite `list`
// are absent on purpose: they always reflect the latest push.
const EDITABLE_CONTROL_TYPES = new Set(
    ["uint8", "uint16", "int16", "pin", "bool", "text", "textarea", "password", "select", "palette", "ipv4"]);
const TIMING_MODES = ["fps", "ms"];

// localStorage keys per ui.md
const LS_SELECTED  = "mm_selectedRoot";
const LS_THEME     = "mm_theme";
const LS_TIMING    = "mm_timing_mode";

// One-release migration for the old key from the pre-spec UI
function lsRead(key, legacyKey, defaultVal) {
    const v = localStorage.getItem(key);
    if (v !== null) return v;
    if (legacyKey) {
        const legacy = localStorage.getItem(legacyKey);
        if (legacy !== null) return legacy;
    }
    return defaultVal;
}

let timingMode = lsRead(LS_TIMING, null, "fps");
let theme      = lsRead(LS_THEME, null, "dark");

// ---------------------------------------------------------------------------
// 2. WebSocket
// ---------------------------------------------------------------------------

function connectWs() {
    if (ws) {
        try { ws.close(); } catch {}
        ws = null;
    }
    const url = `ws://${location.host}/ws`;
    ws = new WebSocket(url);
    ws.binaryType = "arraybuffer";

    ws.onopen = () => {
        wsRetryMs = 500;                                    // reset backoff
        setWsDot(true);
        // Keepalive ping every 25s — Safari kills idle WebSockets otherwise
        clearInterval(wsHeartbeat);
        wsHeartbeat = setInterval(() => {
            if (ws && ws.readyState === WebSocket.OPEN) ws.send("ping");
        }, 25000);
    };

    ws.onmessage = (e) => {
        if (wsPaused) return;
        if (e.data instanceof ArrayBuffer) {
            preview.onBinaryMessage(e.data);
            return;
        }
        try {
            const data = JSON.parse(e.data);
            // The same /ws also carries WLED-compatibility {state,info} frames for the
            // native WLED app (see HttpServerModule's WLED shim). Those are not our
            // module-state shape — ignore anything without a `modules` array, or it would
            // clobber `state` and blank the module view until the next module frame.
            if (!data || !Array.isArray(data.modules)) return;
            state = data;
            updateValues();
        } catch {
            // ignore malformed messages
        }
    };

    ws.onclose = () => {
        setWsDot(false);
        clearInterval(wsHeartbeat);
        wsHeartbeat = null;
        // Exponential backoff with 5s ceiling
        setTimeout(connectWs, wsRetryMs);
        wsRetryMs = Math.min(wsRetryMs * 2, 5000);
    };

    ws.onerror = () => { /* onclose will fire next */ };
}

function setWsDot(connected) {
    const dot = document.getElementById("ws-dot");
    if (!dot) return;
    dot.className = connected ? "ws-dot connected" : "ws-dot disconnected";
}

// Visibility / bfcache hooks
document.addEventListener("visibilitychange", () => {
    wsPaused = (document.visibilityState === "hidden");
});
window.addEventListener("pageshow", (e) => {
    if (e.persisted) {
        // Safari restored from bfcache: re-establish state
        wsPaused = false;
        if (!ws || ws.readyState !== WebSocket.OPEN) connectWs();
    }
});

// ---------------------------------------------------------------------------
// 3. REST helpers + module mutations
// ---------------------------------------------------------------------------

async function init() {
    applyTheme(theme);
    setupStatusBarButtons();
    setupUpdateBadge();
    try {
        const resp = await fetch("/api/state");
        state = await resp.json();
        const savedSel = lsRead(LS_SELECTED, "mm.selectedModule", null);
        if (state.modules && state.modules.length > 0) {
            const exists = savedSel && state.modules.some(m => m.name === savedSel);
            selectedModule = exists ? savedSel : state.modules[0].name;
        }
        renderNav();
        renderCards();
        updateStatusBar();
        // /api/types arrived in plan-11; fetch in parallel. When it arrives, re-render
        // so reset-to-default buttons (whose defaults come from this payload) appear.
        fetch("/api/types").then(r => r.json()).then(j => {
            availableTypes = j.types || [];
            if (state) renderCards();
        }).catch(() => {});
    } catch (err) {
        document.getElementById("main").textContent = "Error: " + err.message;
    }
    connectWs();
    preview.init();
    preview.setupLayout();
}

async function sendControl(moduleName, controlName, value) {
    // Best-effort by design — failures are not retried here. Non-ok responses +
    // network errors are logged to console so a user with devtools open can see
    // what went wrong (e.g. a control value the device-side validator rejected).
    try {
        const res = await fetch("/api/control", {
            method: "POST",
            headers: {"Content-Type": "application/json"},
            body: JSON.stringify({module: moduleName, control: controlName, value: value})
        });
        if (!res.ok) {
            console.warn(`[control] POST ${moduleName}.${controlName} failed (status=${res.status})`);
        }
    } catch (e) {
        console.warn(`[control] POST ${moduleName}.${controlName} failed (error=${e && e.message ? e.message : e})`);
    }
}

async function refetchState() {
    try {
        const r = await fetch("/api/state");
        state = await r.json();
        renderNav();
        renderCards();
    } catch {}
}

async function addModule(type, parentName) {
    if (!type) return;
    const body = {type: type};
    if (parentName) body.parent_id = parentName;
    await fetch("/api/modules", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify(body)
    });
    refetchState();
}

async function deleteModule(name) {
    await fetch("/api/modules/" + encodeURIComponent(name), {method: "DELETE"});
    refetchState();
}

// move to absolute index (0..siblings.length-1). Called from drag-and-drop.
async function moveModuleTo(name, toIndex) {
    await fetch("/api/modules/" + encodeURIComponent(name) + "/move", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify({to: toIndex})
    });
    refetchState();
}

// swap a module for another type at the same position. The replacement starts
// with its own default control values — a clean swap, not a value carry-over.
async function replaceModule(name, newType) {
    if (!newType) return;
    await fetch("/api/modules/" + encodeURIComponent(name) + "/replace", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify({type: newType})
    });
    refetchState();
}

async function rebootDevice() {
    try {
        await fetch("/api/reboot", {method: "POST"});
    } catch { /* connection may drop mid-response — that's the device restarting */ }
    // WS will reconnect on its own via onclose backoff
}

// ---------------------------------------------------------------------------
// 4. Render pipeline
// ---------------------------------------------------------------------------

function renderNav() {
    const nav = document.getElementById("nav");
    if (!nav || !state) return;
    nav.innerHTML = "";

    // One entry per root module. Clicking selects that root — only the selected
    // root's card subtree is rendered (one root visible at a time).
    const list = document.createElement("div");
    list.className = "nav-list";
    for (const mod of state.modules) {
        const item = document.createElement("button");
        item.type = "button";
        item.className = "nav-item";
        item.textContent = mod.name;
        item.dataset.module = mod.name;
        if (mod.name === selectedModule) item.classList.add("active");
        item.addEventListener("click", () => selectModule(mod.name));
        list.appendChild(item);
    }
    nav.appendChild(list);
    nav.appendChild(buildNavFooter());
}

// Footer pinned to the bottom of the side nav: copyright + social links.
function buildNavFooter() {
    const footer = document.createElement("footer");
    footer.className = "nav-footer";

    const links = document.createElement("div");
    links.className = "nav-social";
    const SOCIAL = [
        ["GitHub",  "https://github.com/MoonModules/projectMM",
         "M12 .5C5.65.5.5 5.65.5 12a11.5 11.5 0 0 0 7.86 10.92c.58.1.79-.25.79-.56v-2c-3.2.7-3.88-1.54-3.88-1.54-.53-1.34-1.3-1.7-1.3-1.7-1.06-.72.08-.71.08-.71 1.17.08 1.79 1.2 1.79 1.2 1.04 1.79 2.73 1.27 3.4.97.1-.76.41-1.27.74-1.56-2.55-.29-5.24-1.28-5.24-5.69 0-1.26.45-2.29 1.19-3.1-.12-.29-.52-1.46.11-3.05 0 0 .97-.31 3.18 1.18a11 11 0 0 1 5.8 0c2.2-1.49 3.17-1.18 3.17-1.18.63 1.59.23 2.76.11 3.05.74.81 1.19 1.84 1.19 3.1 0 4.42-2.69 5.39-5.25 5.68.42.36.8 1.08.8 2.18v3.23c0 .31.21.67.8.56A11.5 11.5 0 0 0 23.5 12C23.5 5.65 18.35.5 12 .5Z"],
        ["Discord", "https://discord.gg/TC8NSUSCdV",
         "M20.32 4.37A19.8 19.8 0 0 0 15.45 2.9a13.6 13.6 0 0 0-.62 1.27 18.3 18.3 0 0 0-5.67 0A13 13 0 0 0 8.54 2.9 19.7 19.7 0 0 0 3.67 4.37C.57 8.96-.27 13.44.15 17.85a19.9 19.9 0 0 0 6 3.03c.49-.66.92-1.36 1.29-2.1-.71-.27-1.39-.6-2.03-.99.17-.12.34-.25.5-.38a14.2 14.2 0 0 0 12.18 0c.16.13.33.26.5.38-.64.39-1.32.72-2.03.99.37.74.8 1.44 1.29 2.1a19.8 19.8 0 0 0 6-3.03c.5-5.1-.85-9.55-3.58-13.48ZM8.02 15.13c-1.18 0-2.15-1.08-2.15-2.41 0-1.33.95-2.42 2.15-2.42 1.2 0 2.17 1.1 2.15 2.42 0 1.33-.95 2.41-2.15 2.41Zm7.96 0c-1.18 0-2.15-1.08-2.15-2.41 0-1.33.95-2.42 2.15-2.42 1.2 0 2.17 1.1 2.15 2.42 0 1.33-.95 2.41-2.15 2.41Z"],
        ["Reddit",  "https://reddit.com/r/moonmodules",
         "M22 12c0-1.1-.9-2-2-2-.55 0-1.04.22-1.4.58a9.8 9.8 0 0 0-5.1-1.55l.87-4.1 2.85.6a1.5 1.5 0 1 0 .15-1l-3.18-.67a.5.5 0 0 0-.59.38l-.97 4.57a9.8 9.8 0 0 0-5.16 1.55A2 2 0 1 0 4 13.66a3.9 3.9 0 0 0-.05.6c0 3.3 3.86 5.98 8.62 5.98 4.76 0 8.62-2.68 8.62-5.98 0-.2-.02-.4-.05-.6.53-.36.86-.96.86-1.66ZM8 13.5a1.5 1.5 0 1 1 3 0 1.5 1.5 0 0 1-3 0Zm8.32 4.07c-1.04 1.04-3.02 1.12-3.6 1.12-.58 0-2.57-.08-3.6-1.12a.4.4 0 0 1 .56-.56c.65.65 2.05.88 3.04.88.99 0 2.39-.23 3.04-.88a.4.4 0 0 1 .56.56ZM16 15a1.5 1.5 0 1 1 0-3 1.5 1.5 0 0 1 0 3Z"],
        ["YouTube", "https://www.youtube.com/@MoonModulesLighting",
         "M23.5 6.5a3 3 0 0 0-2.12-2.12C19.5 3.87 12 3.87 12 3.87s-7.5 0-9.38.51A3 3 0 0 0 .5 6.5C0 8.38 0 12 0 12s0 3.62.5 5.5a3 3 0 0 0 2.12 2.12c1.88.51 9.38.51 9.38.51s7.5 0 9.38-.51a3 3 0 0 0 2.12-2.12C24 15.62 24 12 24 12s0-3.62-.5-5.5ZM9.6 15.6V8.4l6.2 3.6-6.2 3.6Z"],
    ];
    for (const [name, url, path] of SOCIAL) {
        const a = document.createElement("a");
        a.href = url;
        a.target = "_blank";
        a.rel = "noopener";
        a.title = name;
        a.setAttribute("aria-label", name);
        a.innerHTML = `<svg viewBox="0 0 24 24" width="18" height="18" fill="currentColor"><path d="${path}"/></svg>`;
        links.appendChild(a);
    }
    footer.appendChild(links);

    // Diagnostic bundle download. Fetches /api/state + /api/system from
    // the *same* origin we're on (the device itself) — sidesteps Chrome's
    // mixed-content blocker that prevents the install page (HTTPS Pages)
    // from doing the same fetch against the device (HTTP LAN). Output is
    // a single JSON blob the user can attach to a bug report.
    const diag = document.createElement("a");
    diag.href = "#";
    diag.className = "nav-diag-link";
    diag.textContent = "Download diagnostics";
    diag.addEventListener("click", async (ev) => {
        ev.preventDefault();
        try {
            const [stateResp, systemResp] = await Promise.all([
                fetch("/api/state"),
                fetch("/api/system"),
            ]);
            const [stateJson, systemJson] = await Promise.all([
                stateResp.json(),
                systemResp.json(),
            ]);
            const bundle = {
                capturedAt: new Date().toISOString(),
                origin: location.origin,
                state: stateJson,
                system: systemJson,
            };
            const blob = new Blob([JSON.stringify(bundle, null, 2)],
                                  { type: "application/json" });
            // Devicename comes from system.deviceName if present, else
            // falls back to the hostname (e.g. "MM-BD3C.local") so the
            // filename is still useful when SystemModule's wire shape
            // doesn't include the name field.
            const devName = (systemJson && systemJson.deviceName)
                || location.hostname || "device";
            const fname = `projectMM-diag-${devName}-${Date.now()}.json`;
            const a = document.createElement("a");
            const blobUrl = URL.createObjectURL(blob);
            a.href = blobUrl;
            a.download = fname;
            a.click();
            // Defer the revoke so the browser has time to start the download.
            // Revoking immediately after click() is technically race-safe on
            // recent Chrome / Firefox (the click navigation is synchronous)
            // but Safari has been observed dropping downloads under a fast
            // revoke. A few seconds is the canonical workaround.
            setTimeout(() => URL.revokeObjectURL(blobUrl), 4000);
        } catch (e) {
            alert(`Diagnostic capture failed: ${e && e.message ? e.message : e}`);
        }
    });
    footer.appendChild(diag);

    const copy = document.createElement("div");
    copy.className = "nav-copyright";
    copy.textContent = `© ${new Date().getFullYear()} MoonModules`;
    footer.appendChild(copy);

    return footer;
}

function selectModule(name) {
    selectedModule = name;
    localStorage.setItem(LS_SELECTED, name);
    document.querySelectorAll(".nav-item").forEach((el) => {
        el.classList.toggle("active", el.dataset.module === name);
    });
    renderCards();
    closeNavDrawer();
}

function findModule(name, modules) {
    if (!modules) modules = state.modules;
    for (const m of modules) {
        if (m.name === name) return m;
        if (m.children) {
            const found = findModule(name, m.children);
            if (found) return found;
        }
    }
    return null;
}

function renderCards() {
    const main = document.getElementById("main");
    if (!main || !state) return;
    main.innerHTML = "";

    // One root visible at a time: render only the selected root's subtree.
    // Falls back to the first root if the selection is missing or stale.
    let root = selectedModule ? findModule(selectedModule) : null;
    if (!root && state.modules.length > 0) {
        root = state.modules[0];
        selectedModule = root.name;
    }
    if (root) renderModuleTree(root, main, 0);
}

function renderModuleTree(mod, parentEl, depth) {
    const { card, childrenEl } = createCard(mod, depth);
    parentEl.appendChild(card);
    // Children render inside this card's .card-children wrapper, not as flat
    // siblings. childrenEl is null for modules that don't accept children.
    if (childrenEl && mod.children && mod.children.length > 0) {
        for (const child of mod.children) {
            renderModuleTree(child, childrenEl, depth + 1);
        }
    }
}

function createCard(mod, depth) {
    const card = document.createElement("div");
    card.className = "card";
    card.dataset.module = mod.name;
    card.dataset.depth = String(depth);

    // -- Title row: [enabled?] [name] [stats] [actions] --
    const title = document.createElement("div");
    title.className = "card-title";

    // The enabled toggle is built here but appended later — it joins the
    // right-hand action cluster (next to ✎ × ?) rather than sitting at the start
    // of the row, for visual grouping with the other per-card controls.
    // Rendered as a <button> styled as a 26×26 rounded box (matching .card-btn);
    // showing ✓ when on, blank when off. Stores its checked state in
    // data-checked so updateValues can sync from WS pushes. A native <input>
    // would not match the other buttons' frame and corner radius.
    const enabled = document.createElement("button");
    enabled.type = "button";
    enabled.className = "module-enabled";
    enabled.dataset.mid = mod.name;
    enabled.dataset.key = "enabled";
    enabled.setAttribute("aria-pressed", "true");
    enabled.title = "Enable / disable";
    const setEnabledUi = (on) => {
        enabled.dataset.checked = on ? "true" : "false";
        enabled.textContent = "⏻";
        enabled.classList.toggle("module-enabled--off", !on);
        enabled.setAttribute("aria-pressed", on ? "true" : "false");
        card.classList.toggle("card--disabled", !on);
    };
    setEnabledUi(mod.enabled === undefined ? true : !!mod.enabled);
    enabled.addEventListener("click", () => {
        const next = enabled.dataset.checked !== "true";
        setEnabledUi(next);
        // Stamp dragTs so a WS state push older than this click can't revert
        // the toggle before the server has acknowledged. updateValues reads
        // dragTs[mod.name + ":enabled"] on line ~952 and suppresses stale
        // patches within the 1s cooldown.
        dragTs[mod.name + ":enabled"] = Date.now();
        sendControl(mod.name, "enabled", next);
    });

    const name = document.createElement("span");
    name.className = "card-name";
    name.textContent = mod.name;
    title.appendChild(name);

    // Emoji tags (role + curated) shown after the name — same set used by the
    // type picker's chip filter, so visual identity is consistent across views.
    const emoji = emojiTagsForMod(mod);
    if (emoji) {
        const emojiEl = document.createElement("span");
        emojiEl.className = "card-name-emoji";
        emojiEl.textContent = emoji;
        title.appendChild(emojiEl);
    }

    // Flex spacer so the name stays left and everything else groups on the right.
    const spacer = document.createElement("span");
    spacer.className = "card-spacer";
    title.appendChild(spacer);

    // fps/ms toggle on the stats line — global mode, single click cycles all cards
    const stats = document.createElement("span");
    stats.className = "card-stats";
    stats.dataset.mid = mod.name;
    stats.dataset.key = "stats";
    stats.title = formatStatsTitle(mod);
    stats.textContent = formatStats(mod);
    stats.addEventListener("click", () => {
        const idx = TIMING_MODES.indexOf(timingMode);
        timingMode = TIMING_MODES[(idx + 1) % TIMING_MODES.length];
        localStorage.setItem(LS_TIMING, timingMode);
        // Refresh every card's stats line in place — no full re-render needed
        document.querySelectorAll(".card-stats[data-mid]").forEach(s => {
            const m = findModule(s.dataset.mid);
            if (m) { s.textContent = formatStats(m); s.title = formatStatsTitle(m); }
        });
    });
    title.appendChild(stats);

    // Enable checkbox joins the right-hand action cluster, before ✎/×.
    title.appendChild(enabled);

    // Delete / replace buttons for user-managed children (any role a container
    // accepts, minus modules that opted out via userEditable=false). Top-level
    // modules are fixed in main.cpp; code-wired children declare userEditable
    // false or carry a role no container accepts. See isUserEditableChild.
    if (isUserEditableChild(mod, depth)) {
        const actions = createActionButtons(mod);
        title.appendChild(actions);
    }

    // Help link → the module's spec page on GitHub, at the far right of the row.
    // docPath comes from /api/types (relative to docs/moonmodules/); omitted if none.
    const docPath = docPathForType(mod.type);
    if (docPath) {
        const help = document.createElement("a");
        help.className = "card-help";
        help.textContent = "?";
        help.title = "Open module documentation";
        help.target = "_blank";
        help.rel = "noopener";
        help.href = "https://github.com/MoonModules/projectMM/blob/main/docs/moonmodules/" + docPath;
        title.appendChild(help);
    }

    card.appendChild(title);

    // -- Controls --
    // Child-hosting modules deeper in the tree (Layers, Layer, Drivers, Layouts)
    // collapse their own controls so the children are the focus by default.
    // Modules that merely host a code-wired child (Network → Improv) keep their
    // controls expanded — the parent's settings are the main point, the code-wired
    // child is informational. Leaf modules render controls inline (no wrapper).
    // EXCEPTION: a top-level module (depth 0 — the selected root, e.g. System,
    // Network) never collapses its own controls, even though it accepts children
    // (System hosts peripherals). It's the card the user is looking at, so its
    // settings should be visible, not hidden behind a "controls" disclosure.
    const hasVisibleControls = mod.controls && mod.controls.some(c => !c.hidden);
    const wrapInDetails = depth > 0 && acceptsNewChildren(mod) && hasVisibleControls;
    const controlsHost = wrapInDetails ? (() => {
        const d = document.createElement("details");
        d.className = "card-controls-collapse";
        const s = document.createElement("summary");
        s.textContent = "controls";
        d.appendChild(s);
        card.appendChild(d);
        return d;
    })() : card;
    if (mod.status) {
        const row = document.createElement("div");
        row.className = "control-row";
        row.dataset.statusMid = mod.name;
        const label = document.createElement("span");
        label.className = "control-label";
        label.textContent = "status";
        const val = document.createElement("span");
        val.className = "status-value";
        val.dataset.sev = mod.severity || "status";
        val.textContent = mod.status;
        row.appendChild(label);
        row.appendChild(val);
        controlsHost.appendChild(row);
    }

    if (mod.controls) {
        for (const ctrl of mod.controls) {
            if (ctrl.hidden) continue;  // plan-10 hidden flag (still respected)
            const row = createControl(mod.name, mod.type, ctrl);
            if (row) controlsHost.appendChild(row);
        }
    }

    // FirmwareUpdate card hosts the shared install picker. Mount once per
    // card-build. The picker reads SystemModule.firmware (already in
    // /api/state) to filter to OTA-compatible releases. On install, the
    // device fetches the binary via /api/firmware/url — no browser CORS in
    // the data path. See docs/architecture.md § Firmware vs board.
    if (mod.type === "FirmwareUpdateModule") {
        // Opening the Firmware card forces a fresh update check (the badge otherwise refreshes
        // only on the 1 h cache cadence) — so the badge agrees with the picker the user is about
        // to use. Fire-and-forget; best-effort.
        checkFirmwareUpdate(true);
        const ownFirmwareKey = (() => {
            // The `firmware` variant key is this module's own control now (moved here from
            // SystemModule), so read it straight off mod — no cross-module lookup.
            const fwCtrl = (mod.controls || []).find(c => c.name === "firmware");
            return fwCtrl && fwCtrl.value ? fwCtrl.value : null;
        })();
        const mount = document.createElement("div");
        mount.className = "install-picker-host";
        controlsHost.appendChild(mount);
        installPicker.init({
            container: mount,
            ownFirmwareKey,
            // Device already knows its deviceModel (SystemModule) — picker is for
            // releases + firmware compatibility only. Showing a board picker
            // here would invite the user to mis-narrow the firmware list.
            enableBoardPicker: false,
            onInstall: async (_firmware, _manifestUrl, binaryUrl) => {
                const res = await fetch("/api/firmware/url", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({ url: binaryUrl }),
                });
                if (!res.ok) {
                    let msg = `HTTP ${res.status}`;
                    try {
                        const j = await res.json();
                        if (j.error) msg = j.error;
                    } catch (_) { /* non-JSON error response */ }
                    throw new Error(msg);
                }
            },
        });
    }

    // -- Children block + footer --
    // The .card-children wrapper lives inside this card so the parent's border
    // encloses its children; renderModuleTree recurses into it. The "+ add module"
    // footer only appears on parents that accept user-created children — a parent
    // hosting only code-wired children (e.g. Network → Improv) renders the
    // children block but no add button.
    let childrenEl = null;
    if (hasNestedChildren(mod)) {
        childrenEl = document.createElement("div");
        childrenEl.className = "card-children";
        childrenEl.dataset.depth = String(depth + 1);
        card.appendChild(childrenEl);

        if (acceptsNewChildren(mod)) {
            // -- Footer: + add module --
            const footer = document.createElement("div");
            footer.className = "card-footer";
            const addBtn = document.createElement("button");
            addBtn.className = "add-btn";
            addBtn.textContent = "+ add module";
            addBtn.addEventListener("click", () => {
                // Hide the button while the picker is open (the picker takes its
                // place); restore it once the picker is removed (cancel/create/Esc).
                addBtn.style.display = "none";
                openTypePicker(mod, footer);
                const obs = new MutationObserver(() => {
                    if (!footer.querySelector(".type-picker")) {
                        addBtn.style.display = "";
                        obs.disconnect();
                    }
                });
                obs.observe(footer, {childList: true});
            });
            footer.appendChild(addBtn);
            card.appendChild(footer);
        }
    }

    // -- Drag-to-reorder (HTML5 DnD on desktop; touchstart-gated on mobile) --
    // Same gate as the delete/replace buttons: a user-managed child is also
    // reorderable within its parent.
    if (isUserEditableChild(mod, depth)) {
        attachDragHandlers(card, mod);
    }

    return { card, childrenEl };
}

// Wire a button as a press-twice confirm: the first click arms it (adds the
// `armed` class, optional armed label/title), a second click runs `onConfirm`.
// Disarms after 3s or when the pointer leaves. Used by the delete and reboot
// buttons — no browser confirm() popup. `armedText` is optional (delete swaps
// × → ✓; reboot keeps its glyph). The pre-arm title is captured live so a
// title updated elsewhere (e.g. reboot's crashed-state text) restores correctly.
function armPressTwice(btn, onConfirm, opts = {}) {
    let armed = false;
    let disarmTimer = null;
    let savedText = "";
    let savedTitle = "";
    const disarm = () => {
        armed = false;
        btn.classList.remove("armed");
        if (opts.armedText !== undefined) btn.textContent = savedText;
        btn.title = savedTitle;
        if (disarmTimer) { clearTimeout(disarmTimer); disarmTimer = null; }
    };
    btn.addEventListener("click", () => {
        // Disarm before running the action so a stray second click can't fire
        // it twice (e.g. two /api/reboot requests).
        if (armed) { disarm(); onConfirm(); return; }
        armed = true;
        savedText = btn.textContent;
        savedTitle = btn.title;
        btn.classList.add("armed");
        if (opts.armedText !== undefined) btn.textContent = opts.armedText;
        if (opts.armedTitle !== undefined) btn.title = opts.armedTitle;
        disarmTimer = setTimeout(disarm, 3000);
    });
    btn.addEventListener("mouseleave", disarm);
}

// Compact byte formatter — "B" under 1 KB, "KB" otherwise (one decimal under 10 KB).
function fmtBytes(n) {
    if (n < 1024) return n + "B";
    const k = n / 1024;
    return (k < 10 ? k.toFixed(1) : Math.round(k)) + "KB";
}

// Stats line: timing (🕒, fps or µs/ms per the global toggle) + memory
// (🧠 static, plus "+ dynamic" only when the module allocated heap).
// Timing is omitted entirely when the module has no measured loop time.
function formatStats(mod) {
    const us = (mod.loopTimeUs !== undefined) ? mod.loopTimeUs : 0;
    let timing = "";
    if (us > 0) {
        if (timingMode === "fps") {
            const fps = Math.round(1_000_000 / us);
            timing = "🕒 " + (fps >= 1000 ? Math.round(fps / 1000) + "K fps" : fps + " fps");
        } else {
            timing = "🕒 " + (us < 1000 ? us + " µs" : (us / 1000).toFixed(2) + " ms");
        }
    }
    const stat = mod.classSize || 0;
    const dyn = mod.dynamicBytes || 0;
    const mem = "🧠 " + fmtBytes(stat) + (dyn > 0 ? " + " + fmtBytes(dyn) : "");
    // Status chip: emitted by the engine when a module has something to say.
    // Severity picks the emoji — ℹ️ neutral (Eth: 192.168.1.210), ⚠️ degraded
    // (buffer reduced), ❌ error (No network). Tooltip carries the full text.
    const sev = mod.severity || "status";
    const sevEmoji = sev === "error" ? "❌" : sev === "warning" ? "⚠️" : "ℹ️";
    const statusChip = mod.status ? "  " + sevEmoji : "";
    const head = timing ? timing + "   " + mem : mem;
    return head + statusChip;
}
function formatStatsTitle(mod) {
    return "Click to toggle fps/ms";
}

function createActionButtons(mod) {
    const wrap = document.createElement("span");
    wrap.className = "card-actions";

    // Reorder is drag-and-drop only (works on desktop and mobile). The whole
    // card body is the drag source; the controls region excludes itself via
    // the mousedown gate in attachDragHandlers. No up/down buttons, no
    // dedicated drag handle.

    const replaceBtn = document.createElement("button");
    replaceBtn.className = "card-btn";
    replaceBtn.textContent = "✎";
    replaceBtn.title = "Replace with another type";
    replaceBtn.addEventListener("click", () => {
        // Anchor the picker to the card so it drops below the card content,
        // not inside the cramped 26px action-button row.
        openReplacePicker(mod, replaceBtn.closest(".card"));
    });
    wrap.appendChild(replaceBtn);

    // Delete: press × once to arm, again to confirm — see armPressTwice.
    const delBtn = document.createElement("button");
    delBtn.className = "card-btn card-btn-del";
    delBtn.textContent = "×";
    delBtn.title = "Delete";
    armPressTwice(delBtn, () => deleteModule(mod.name),
                  {armedText: "✓", armedTitle: "Click again to delete"});
    wrap.appendChild(delBtn);

    return wrap;
}

function findParent(childName) {
    function walk(node, modules) {
        for (const m of modules) {
            if (m === node) return null;  // shouldn't happen, defensive
            if (m.children && m.children.some(c => c.name === childName)) return m;
            if (m.children) {
                const p = walk(node, m.children);
                if (p) return p;
            }
        }
        return null;
    }
    return walk(null, state.modules);
}

// Whether this module renders any nested children at all (a "+ add module"
// button included if it also accepts new ones via the UI). True whenever the
// module has at least one child today OR is one of the light-pipeline
// containers that users can add to. This lets code-wired children (e.g.
// ImprovProvisioning under Network) render without making the parent UI-addable.
function hasNestedChildren(mod) {
    return (mod.children && mod.children.length > 0) || acceptsNewChildren(mod);
}

// Roles this parent accepts as user-added children, from the device's
// `acceptsChildRoles` (per-type in /api/types — e.g. Layer → "effect,modifier").
// Domain-neutral: the UI no longer hardcodes which module types are containers;
// the device declares it via MoonModule::acceptsChildRoles(). "" → [] (accepts
// none), which is also the default for modules whose type isn't loaded yet.
function rolesAcceptedBy(parentMod) {
    const t = availableTypes.find(t => t.name === parentMod.type);
    const csv = (t && t.acceptsChildRoles) ? t.acceptsChildRoles : "";
    return csv ? csv.split(",") : [];
}

// Whether the "+ add module" affordance applies — derived from acceptsChildRoles
// being non-empty, so there's a single source of truth (no separate list).
function acceptsNewChildren(mod) {
    return rolesAcceptedBy(mod).length > 0;
}

// The set of child roles ANY loaded type accepts — the union of every type's
// acceptsChildRoles. A module is "user-managed as a child" iff its role is in
// this set, which is how the UI decides to show delete/replace/drag without
// hardcoding role names. Code-wired children (ImprovProvisioning,
// — roles no container declares) correctly fall outside it.
function allAcceptedChildRoles() {
    const roles = new Set();
    for (const t of availableTypes) {
        const csv = t.acceptsChildRoles || "";
        if (csv) csv.split(",").forEach(r => roles.add(r));
    }
    return roles;
}

// Whether the UI shows delete / replace / drag for this module. True when it's
// a nested module (depth > 0) whose role is one some container accepts AND it
// hasn't opted out via the device's userEditable=false (e.g. PreviewDriver).
// Replaces the old hardcoded `role === "effect" || "modifier"` gate — now any
// add-accepted role (driver, layout, …) is editable, and the child itself can
// veto via userEditable.
//
// We test mod.role against the UNION of all containers' acceptsChildRoles, not
// against this module's specific parent. That's exact while the role→container
// mapping is 1:1 (effect→Layer, driver→Drivers, layout→Layouts, layer→Layers) —
// a child of an add-accepted role is always under the one container that
// accepts it. If a role ever becomes accepted by more than one container, this
// would need the parent threaded in to scope the check to the actual parent.
function isUserEditableChild(mod, depth) {
    return depth > 0
        && mod.userEditable !== false
        && allAcceptedChildRoles().has(mod.role);
}

// ---------------------------------------------------------------------------
// Control rendering (9 types per ui.md)
// ---------------------------------------------------------------------------

// Look up the factory default for a given module type's control. Returns undefined when
// the type isn't in /api/types yet or the control has no default (display/progress).
function defaultFor(moduleType, ctrlName) {
    if (!moduleType) return undefined;
    const t = availableTypes.find(t => t.name === moduleType);
    if (!t || !t.defaults) return undefined;
    return t.defaults[ctrlName];
}

// The module type's spec-page path (relative to docs/moonmodules/), from /api/types.
// Returns "" when the type isn't loaded yet or declares no doc path.
function docPathForType(moduleType) {
    if (!moduleType) return "";
    const t = availableTypes.find(t => t.name === moduleType);
    return (t && t.docPath) ? t.docPath : "";
}

// Curated emoji string for a live module — its role emoji plus the type's
// `tags` from /api/types, deduplicated, in role-first order. "" if the type
// isn't loaded yet. Used on the card title and in the type picker.
function emojiTagsForMod(mod) {
    if (!mod) return "";
    const t = availableTypes.find(t => t.name === mod.type) || {role: mod.role, tags: ""};
    return emojiTagsFor(t).join("");
}

function createControl(moduleName, moduleType, ctrl) {
    const row = document.createElement("div");
    row.className = "control-row";
    row.dataset.key = ctrl.name;

    const label = document.createElement("label");
    label.className = "control-label";
    label.textContent = ctrl.name;
    row.appendChild(label);

    const key = moduleName + ":" + ctrl.name;
    const def = defaultFor(moduleType, ctrl.name);

    switch (ctrl.type) {
        case "uint8": {
            const input = document.createElement("input");
            input.type = "range";
            input.min = ctrl.min ?? 0;
            input.max = ctrl.max ?? 255;
            input.value = ctrl.value ?? 0;
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            const numInput = document.createElement("input");
            numInput.type = "number";
            numInput.className = "control-value-input";
            numInput.min = input.min;
            numInput.max = input.max;
            numInput.value = input.value;
            input.addEventListener("input", () => {
                dragTs[key] = Date.now();
                numInput.value = input.value;
                debounceSend(key, 150, () => sendControl(moduleName, ctrl.name, parseInt(input.value)));
            });
            numInput.addEventListener("input", () => {
                dragTs[key] = Date.now();   // stamp so a WS push can't revert what's being typed
                const v = Math.max(Number(input.min), Math.min(Number(input.max), parseInt(numInput.value) || 0));
                input.value = v;
                debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, v));
            });
            row.appendChild(input);
            row.appendChild(numInput);
            appendResetButton(row, moduleName, ctrl, def, () => {
                input.value = def;
                numInput.value = def;
            });
            break;
        }
        case "uint16": {
            // Bounded (server sent an explicit max below the type ceiling) →
            // slider, like uint8/int16. Unbounded (max == 65535, the default for
            // port/universe-style values with no natural range) → plain number.
            const uMin = Number(ctrl.min ?? 0);
            const uMax = Number(ctrl.max ?? 65535);
            if (uMax < 65535) {
                const input = document.createElement("input");
                input.type = "range";
                input.min = uMin;
                input.max = uMax;
                input.value = Math.max(uMin, Math.min(uMax, Number(ctrl.value ?? 0)));
                input.dataset.mid = moduleName;
                input.dataset.key = ctrl.name;
                const numInput = document.createElement("input");
                numInput.type = "number";
                numInput.className = "control-value-input";
                numInput.min = uMin;
                numInput.max = uMax;
                numInput.value = input.value;
                input.addEventListener("input", () => {
                    dragTs[key] = Date.now();
                    numInput.value = input.value;
                    debounceSend(key, 150, () => sendControl(moduleName, ctrl.name, parseInt(input.value)));
                });
                numInput.addEventListener("input", () => {
                    dragTs[key] = Date.now();   // stamp so a WS push can't revert what's being typed
                    const v = Math.max(uMin, Math.min(uMax, parseInt(numInput.value) || 0));
                    input.value = v;
                    debounceSend(key, 150, () => sendControl(moduleName, ctrl.name, v));
                });
                row.appendChild(input);
                row.appendChild(numInput);
                appendResetButton(row, moduleName, ctrl, def, () => {
                    input.value = def; numInput.value = def;
                });
            } else {
                const input = document.createElement("input");
                input.type = "number";
                input.value = ctrl.value ?? 0;
                input.dataset.mid = moduleName;
                input.dataset.key = ctrl.name;
                input.addEventListener("input", () => {
                    dragTs[key] = Date.now();
                    // Sanitise: empty/garbage → 0, clamp into the uint16 range so a
                    // NaN or out-of-range value never reaches the device.
                    let v = parseInt(input.value, 10);
                    if (Number.isNaN(v)) v = 0;
                    v = Math.max(0, Math.min(65535, v));
                    debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, v));
                });
                row.appendChild(input);
                appendResetButton(row, moduleName, ctrl, def, () => { input.value = def; });
            }
            break;
        }
        case "pin": {
            // A GPIO pin: plain number input, never a slider (a pin has no range to
            // drag). −1 = unused. ctrl.min/max are the valid-GPIO span used only to
            // clamp the typed value before sending.
            const pMin = Number(ctrl.min ?? -1);
            const pMax = Number(ctrl.max ?? 52);
            const input = document.createElement("input");
            input.type = "number";
            input.min = pMin;
            input.max = pMax;
            input.value = ctrl.value ?? -1;
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            input.addEventListener("input", () => {
                dragTs[key] = Date.now();
                let v = parseInt(input.value, 10);
                if (Number.isNaN(v)) v = -1;
                v = Math.max(pMin, Math.min(pMax, v));
                debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, v));
            });
            row.appendChild(input);
            appendResetButton(row, moduleName, ctrl, def, () => { input.value = def; });
            break;
        }
        case "int16": {
            // ctrl.min/ctrl.max are always present (server sends them). Sentinel
            // values INT16_MIN (-32768) / INT16_MAX (32767) mean "unbounded" —
            // fall back to a ±percentage range.
            const rawMin = Number(ctrl.min ?? -32768);
            const rawMax = Number(ctrl.max ?? 32767);
            const min = rawMin <= -32768 ? -100 : rawMin;
            const max = rawMax >= 32767  ?  200 : rawMax;
            const raw = Number(ctrl.value ?? 0);
            const clamped = Math.max(min, Math.min(max, raw));
            const input = document.createElement("input");
            input.type = "range";
            input.min = min;
            input.max = max;
            input.value = clamped;
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            const numInput = document.createElement("input");
            numInput.type = "number";
            numInput.className = "control-value-input";
            numInput.min = min;
            numInput.max = max;
            numInput.value = input.value;
            input.addEventListener("input", () => {
                dragTs[key] = Date.now();
                numInput.value = input.value;
                debounceSend(key, 150, () => sendControl(moduleName, ctrl.name, parseInt(input.value)));
            });
            numInput.addEventListener("input", () => {
                dragTs[key] = Date.now();   // stamp so a WS push can't revert what's being typed
                const v = Math.max(min, Math.min(max, parseInt(numInput.value) || 0));
                input.value = v;
                debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, v));
            });
            row.appendChild(input);
            row.appendChild(numInput);
            appendResetButton(row, moduleName, ctrl, def, () => {
                input.value = def;
                numInput.value = def;
            });
            break;
        }
        case "bool": {
            // Modern on/off switch: an <input type=checkbox> (kept for the
            // existing change/sync paths) wrapped in a <label> that draws a
            // pill-shaped track + sliding thumb via CSS. The checkbox itself
            // is visually hidden but stays the source of truth.
            const sw = document.createElement("label");
            sw.className = "switch";
            const input = document.createElement("input");
            input.type = "checkbox";
            input.checked = !!ctrl.value;
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            input.addEventListener("change", () => {
                dragTs[key] = Date.now();
                sendControl(moduleName, ctrl.name, input.checked);
            });
            const track = document.createElement("span");
            track.className = "switch-track";
            sw.appendChild(input);
            sw.appendChild(track);
            row.appendChild(sw);
            appendResetButton(row, moduleName, ctrl, def, () => { input.checked = !!def; });
            break;
        }
        case "text": {
            const input = document.createElement("input");
            input.type = "text";
            input.value = ctrl.value ?? "";
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            // ctrl.readonly is a UI hint (the server still accepts writes — the
            // flag exists for values pushed by tooling like MoonDeck or the
            // web installer, where editing in the device UI is by-convention
            // disallowed). `readOnly` lets the user select/copy the text but
            // not modify it; `disabled` would also block copy.
            if (ctrl.readonly) {
                input.readOnly = true;
            } else {
                input.addEventListener("input", () => {
                    dragTs[key] = Date.now();
                    debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, input.value));
                });
            }
            row.appendChild(input);
            break;
        }
        case "textarea": {
            // Multi-line text (e.g. a script source). A resizable <textarea>; the
            // value syncs and debounces exactly like a "text" control.
            const input = document.createElement("textarea");
            input.className = "control-textarea";
            input.value = ctrl.value ?? "";
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            input.rows = 1;            // default compact; CSS height + resize grip control size
            input.spellcheck = false;
            if (ctrl.readonly) {
                input.readOnly = true;
            } else {
                input.addEventListener("input", () => {
                    dragTs[key] = Date.now();
                    debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, input.value));
                });
            }
            row.appendChild(input);
            break;
        }
        case "password": {
            // ctrl.value arrives XOR-obfuscated + base64-encoded (see
            // HttpServerModule PASSWORD_XOR_KEY). Decode it so the input holds
            // the real stored password — masked by the password input, revealed
            // by hold-to-peek. The obfuscation is trivially reversible by design.
            const input = document.createElement("input");
            input.type = "password";
            input.value = decodePassword(ctrl.value);
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            input.addEventListener("input", () => {
                dragTs[key] = Date.now();
                debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, input.value));
            });
            row.appendChild(input);
            // Hold-to-peek button — reveals the stored password.
            const peek = document.createElement("button");
            peek.className = "peek-btn";
            peek.type = "button";
            peek.textContent = "👁";
            peek.title = "Hold to reveal";
            const show = () => { input.type = "text"; };
            const hide = () => { input.type = "password"; };
            peek.addEventListener("mousedown", show);
            peek.addEventListener("mouseup", hide);
            peek.addEventListener("mouseleave", hide);
            peek.addEventListener("touchstart", (e) => { e.preventDefault(); show(); });
            peek.addEventListener("touchend", hide);
            row.appendChild(peek);
            break;
        }
        case "select": {
            const sel = document.createElement("select");
            sel.dataset.mid = moduleName;
            sel.dataset.key = ctrl.name;
            (ctrl.options || []).forEach((opt, i) => {
                const o = document.createElement("option");
                o.value = i;
                o.textContent = opt;
                if (i === ctrl.value) o.selected = true;
                sel.appendChild(o);
            });
            // Protect the dropdown while the user has it open. A native <select>
            // popup stays open for several frames (seconds, if deliberating) while
            // a continuously-refreshed module keeps pushing state over the WS; an
            // unguarded `sel.value = ctrl.value` patch during that window snaps the
            // menu back to the old option and visibly closes it — the user never
            // gets to pick. We mark the select "open" on pointerdown (fires BEFORE
            // the popup opens, unlike focus, which some browsers delay or skip) and
            // clear it on change/blur; updateModuleControls skips any select marked
            // open. pointerdown also stamps the dragTs cooldown as a belt-and-braces
            // fallback for the post-close frames.
            sel.dataset.open = "false";
            const markOpen = () => { sel.dataset.open = "true"; dragTs[key] = Date.now(); };
            sel.addEventListener("pointerdown", markOpen);
            sel.addEventListener("focus", markOpen);
            sel.addEventListener("blur", () => { sel.dataset.open = "false"; });
            sel.addEventListener("change", () => {
                sel.dataset.open = "false";
                dragTs[key] = Date.now();
                sendControl(moduleName, ctrl.name, parseInt(sel.value));
                // No refetch/re-render here: blendMode/opacity-style selects don't
                // change the control SET, and a control that does (a hidden-flag
                // flip) is reconciled in place by syncVisibleControls on the next
                // WS push — so the card (and its expanded state) is preserved.
                // A full refetchState() rebuilt the DOM and collapsed the card.
            });
            row.appendChild(sel);
            appendResetButton(row, moduleName, ctrl, def, () => { sel.value = def; });
            break;
        }
        case "palette": {
            // A colour-palette dropdown where EVERY option shows its own gradient — so the colours
            // are visible before selecting, not just after. A native <select> can't do this
            // (browsers ignore a gradient background on <option>, and the macOS popup is OS-drawn),
            // so this is a custom dropdown: a trigger button (selected swatch + name + caret) that
            // toggles a list of styled rows, each a gradient swatch + name. The value still rides as
            // the option index. Prior art: MoonLight's palette control (same native-select limit).
            const opts = ctrl.options || [];
            const gradientFor = (i) => {
                const cols = (opts[i] || {}).colors || "";
                const stops = cols.split(/\s+/).filter(Boolean).map(h => "#" + h);
                return stops.length ? `linear-gradient(to right, ${stops.join(",")})` : "none";
            };
            const wrap = document.createElement("div");
            wrap.className = "palette-control";
            wrap.dataset.mid = moduleName;
            wrap.dataset.key = ctrl.name;
            wrap.dataset.value = ctrl.value;

            // Trigger: shows the currently-selected palette (swatch + name) and opens the list.
            const trigger = document.createElement("button");
            trigger.type = "button";
            trigger.className = "palette-trigger";
            const triSwatch = document.createElement("span");
            triSwatch.className = "palette-swatch";
            const triName = document.createElement("span");
            triName.className = "palette-name";
            const caret = document.createElement("span");
            caret.className = "palette-caret";
            caret.textContent = "▾";
            const paintTrigger = (i) => {
                triSwatch.style.background = gradientFor(i);
                triName.textContent = (opts[i] || {}).name || String(i);
            };
            paintTrigger(ctrl.value);
            trigger.append(triSwatch, triName, caret);

            // The list of gradient rows, hidden until the trigger is clicked.
            const list = document.createElement("div");
            list.className = "palette-list";
            list.hidden = true;
            opts.forEach((opt, i) => {
                const item = document.createElement("button");
                item.type = "button";
                item.className = "palette-item" + (i === ctrl.value ? " selected" : "");
                item.dataset.idx = i;
                const sw = document.createElement("span");
                sw.className = "palette-swatch";
                sw.style.background = gradientFor(i);
                const nm = document.createElement("span");
                nm.className = "palette-name";
                nm.textContent = opt.name || String(i);
                item.append(sw, nm);
                item.addEventListener("click", () => {
                    wrap.dataset.value = i;
                    paintTrigger(i);
                    list.querySelectorAll(".selected").forEach(x => x.classList.remove("selected"));
                    item.classList.add("selected");
                    closeList();
                    dragTs[key] = Date.now();
                    sendControl(moduleName, ctrl.name, i);
                });
                list.appendChild(item);
            });

            // Open/close, dismissing on outside-click or Escape (the type-picker pattern).
            let onDocClick = null;
            const onKey = (e) => { if (e.key === "Escape") closeList(); };
            const closeList = () => {
                list.hidden = true;
                wrap.dataset.open = "false";
                if (onDocClick) {
                    document.removeEventListener("pointerdown", onDocClick);
                    document.removeEventListener("keydown", onKey);
                    onDocClick = null;
                }
            };
            const openList = () => {
                list.hidden = false;
                wrap.dataset.open = "true";
                dragTs[key] = Date.now();
                // Bring the selected row into view (a long palette list may overflow the popup).
                const sel = list.querySelector(".palette-item.selected");
                if (sel) sel.scrollIntoView({ block: "nearest" });
                onDocClick = (e) => { if (!wrap.contains(e.target)) closeList(); };
                document.addEventListener("pointerdown", onDocClick);
                document.addEventListener("keydown", onKey);
            };
            trigger.addEventListener("click", () => { list.hidden ? openList() : closeList(); });

            wrap.append(trigger, list);
            row.appendChild(wrap);
            // Reset to the default palette like every other persisted control: re-paint the trigger
            // and the selected row to `def` (sendControl is handled by appendResetButton).
            appendResetButton(row, moduleName, ctrl, def, () => {
                wrap.dataset.value = def;
                paintTrigger(def);
                list.querySelectorAll(".selected").forEach(x => x.classList.remove("selected"));
                const r = list.querySelector(`.palette-item[data-idx="${def}"]`);
                if (r) r.classList.add("selected");
            });
            break;
        }
        case "display": {
            // Read-only string. Updates via WS push.
            const span = document.createElement("span");
            span.className = "display";
            span.dataset.mid = moduleName;
            span.dataset.key = ctrl.name;
            span.textContent = ctrl.value ?? "";
            row.appendChild(span);
            break;
        }
        case "display-int": {
            // Read-only signed int with a unit suffix (e.g. "-58 dBm").
            // ctrl.unit is the suffix the device chose at addReadOnlyInt time.
            const span = document.createElement("span");
            span.className = "display";
            span.dataset.mid = moduleName;
            span.dataset.key = ctrl.name;
            span.dataset.kind = "display-int";
            span.dataset.unit = ctrl.unit ?? "";
            span.textContent = fmtDisplayInt(ctrl);
            row.appendChild(span);
            break;
        }
        case "ipv4": {
            // Editable dotted-quad. Wire format is the same string the user
            // types — the device parses + validates server-side and rejects
            // malformed values with 400. Inline validation on the client is
            // a future enhancement; today an invalid value goes to the
            // server and the response surfaces the rejection.
            //
            // Same dragTs + debounceSend pattern as text / password so the
            // ipv4 input participates in stale-WS-push protection: while
            // the user is typing, dragTs[key] gets bumped, and an arriving
            // WS push within the cooldown window won't revert mid-edit
            // (see updateValues + the dragTs check ~line 1260).
            const input = document.createElement("input");
            input.type = "text";
            input.className = "ipv4-input";
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            input.value = ctrl.value ?? "";
            input.placeholder = "0.0.0.0";
            input.maxLength = 15;  // "255.255.255.255" = 15
            input.addEventListener("input", () => {
                dragTs[key] = Date.now();
                debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, input.value));
            });
            row.appendChild(input);
            break;
        }
        case "time": {
            // Read-only seconds, rendered as "Xd Yh Zm Ws"
            const span = document.createElement("span");
            span.className = "display";
            span.dataset.mid = moduleName;
            span.dataset.key = ctrl.name;
            span.dataset.kind = "time";
            span.textContent = fmtTime(ctrl.value ?? 0);
            row.appendChild(span);
            break;
        }
        case "progress": {
            const bar = document.createElement("progress");
            bar.max = ctrl.total ?? 100;
            bar.value = ctrl.value ?? 0;
            bar.dataset.mid = moduleName;
            bar.dataset.key = ctrl.name;
            row.appendChild(bar);
            const lbl = document.createElement("span");
            lbl.className = "control-value";
            lbl.textContent = fmtProgressLabel(ctrl);
            lbl.dataset.mid = moduleName;
            lbl.dataset.key = ctrl.name + ".label";
            row.appendChild(lbl);
            break;
        }
        case "button": {
            const btn = document.createElement("button");
            btn.className = "action-btn";
            btn.textContent = ctrl.label || ctrl.name;
            btn.addEventListener("click", () => sendControl(moduleName, ctrl.name, 1));
            row.appendChild(btn);
            break;
        }
        case "list": {
            // A generic read-only list (ControlType::List). value = array of row
            // summary objects; ctrl.detail = parallel array of detail objects (same
            // order). Render one clickable row per summary; clicking toggles a detail
            // panel below it. Self rows (summary.self === true) get a marker. Fully
            // generic — the engine decides the fields; the UI just renders objects.
            row.classList.add("control-list-row");
            const rows = Array.isArray(ctrl.value) ? ctrl.value : [];
            const details = Array.isArray(ctrl.detail) ? ctrl.detail : [];
            const list = document.createElement("div");
            list.className = "list-control";
            list.dataset.mid = moduleName;
            list.dataset.key = ctrl.name;
            buildListEntries(list, rows, details, new Set());   // initial: nothing expanded
            row.appendChild(list);
            break;
        }
        default:
            // Unknown control type — skip silently. New types may be added engine-side
            // without breaking the UI; they just don't render until handled here.
            return null;
    }

    return row;
}

// Rebuild a List control's entries inside `container` from `rows` (summary objects)
// and `details` (parallel detail objects). `openSet` is a Set<string> of summary
// texts whose detail panels start expanded — pass `new Set()` for a fresh build, or
// the currently-open set on a live re-render so an expanded row stays open. Shared by
// createControl (initial) and updateModuleControls (WS live patch) so the two can't drift.
function buildListEntries(container, rows, details, openSet) {
    container.replaceChildren();
    if (rows.length === 0) {
        const empty = document.createElement("div");
        empty.className = "list-empty";
        empty.textContent = "(none)";
        container.appendChild(empty);
        return;
    }
    rows.forEach((item, i) => {
        const entry = document.createElement("div");
        entry.className = "list-entry" + (item && item.self ? " list-self" : "");
        const summary = document.createElement("div");
        summary.className = "list-summary";
        summary.tabIndex = 0;
        summary.setAttribute("role", "button");
        // Freshness dot (always-visible age at a glance) when the row carries a `*Sec`
        // duration; coloured by ageBucketClass. Generic — no device knowledge here.
        // The age fields (`ageSec`/`cached`) live in the DETAIL object, not the summary,
        // so read the detail for the dot (it also carries `self`); fall back to the
        // summary item when a list has no separate detail.
        const ageClass = rowAgeClass(details[i] ?? item);
        if (ageClass) {
            const dot = document.createElement("span");
            dot.className = "age-dot " + ageClass;
            dot.setAttribute("aria-hidden", "true");
            summary.appendChild(dot);
        }
        const label = document.createElement("span");
        label.textContent = listSummaryText(item);
        summary.appendChild(label);
        const detailPanel = document.createElement("div");
        detailPanel.className = "list-detail";
        detailPanel.hidden = !openSet.has(summary.textContent);
        summary.setAttribute("aria-expanded", String(!detailPanel.hidden));
        fillListDetail(detailPanel, details[i] ?? item);
        const toggle = () => {
            detailPanel.hidden = !detailPanel.hidden;
            summary.setAttribute("aria-expanded", String(!detailPanel.hidden));
        };
        summary.addEventListener("click", toggle);
        summary.addEventListener("keydown", (e) => {
            if (e.key === "Enter" || e.key === " ") { e.preventDefault(); toggle(); }
        });
        entry.append(summary, detailPanel);
        container.appendChild(entry);
    });
}

// Join a list row's scalar fields into a one-line summary (skips the `self` marker
// flag and any nested objects). Generic: the engine names the fields.
function listSummaryText(item) {
    if (!item || typeof item !== "object") return String(item ?? "");
    return Object.entries(item)
        .filter(([k, v]) => k !== "self" && typeof v !== "object")
        .map(([, v]) => v)
        .join("  ·  ");
}

// Render a list row's detail object as read-only key/value rows. Scalars print as-is;
// an array of scalars (e.g. a device's `speaks:["http"]` or `via:["mdns","scan"]`)
// renders as small chips so multi-valued fields like the discovery source are visible
// at a glance. Nested objects are still skipped (no use case yet). Generic — the engine
// names the fields, so a new array field shows up with no UI change here.
function fillListDetail(panel, detail) {
    panel.replaceChildren();
    if (!detail || typeof detail !== "object") return;
    for (const [k, v] of Object.entries(detail)) {
        const isScalarArray = Array.isArray(v) && v.every(e => typeof e !== "object");
        if (typeof v === "object" && !isScalarArray) continue;
        // `cached` and `ageSec` both render as "last seen"; a projectMM device emits
        // exactly one (mutually exclusive in DevicesModule), but skip ageSec when a
        // `cached` key is also present so any other source can't produce two conflicting
        // "last seen" rows. Match the cached branch's render condition, which fires on
        // key EXISTENCE (`k === "cached"`), not truthiness — so gate on the key being
        // present, not on its value. (Robust-to-any-input, generic.)
        if (k === "ageSec" && "cached" in detail) continue;
        const r = document.createElement("div");
        r.className = "list-detail-row";
        const kEl = document.createElement("span");
        // A `*Sec` field is a duration in seconds (e.g. a device's `ageSec`) — show it
        // under a plainer label ("last seen") and as a relative time, not a bare count.
        // `cached` is the sibling: a restored device not yet re-seen live → "last seen:
        // cached" rather than a fake recent time.
        const isDuration = k.endsWith("Sec");
        kEl.className = "list-detail-key";
        kEl.textContent = (k === "ageSec" || k === "cached") ? "last seen" : k;
        const vEl = document.createElement("span");
        vEl.className = "list-detail-val";
        if (k === "cached") {
            vEl.textContent = "cached";
            vEl.classList.add("list-detail-muted");
        } else if (isScalarArray) {
            for (const e of v) {
                const chip = document.createElement("span");
                chip.className = "list-detail-chip";
                chip.textContent = String(e);
                vEl.appendChild(chip);
            }
        } else if (isDuration) {
            vEl.textContent = relativeAge(Number(v));
            const ageClass = ageBucketClass(Number(v));   // tint to match the summary dot
            if (ageClass) vEl.classList.add(ageClass);
        } else if (typeof v === "string" && /^https?:\/\//.test(v)) {
            // A value that is an http(s) URL (e.g. a device's `url`) renders as a link that
            // opens in a new tab — generic, any ListSource detail can surface one. rel
            // guards the opened page from reaching back via window.opener.
            const a = document.createElement("a");
            a.href = v;
            a.textContent = v;
            a.target = "_blank";
            a.rel = "noopener noreferrer";
            a.className = "list-detail-link";
            vEl.appendChild(a);
        } else {
            vEl.textContent = String(v);
        }
        r.append(kEl, vEl);
        panel.appendChild(r);
    }
}

// Freshness bucket for a duration-in-seconds (a `*Sec` field): green < 1 min, orange
// < 1 hour, red beyond. Generic — any duration field a ListSource emits gets the same
// scale; nothing device-specific here. (DevicesModule's ageSec is the first user: a
// device unseen > 24h is aged out of the list entirely, so "red" spans 1h–24h.)
function ageBucketClass(sec) {
    if (!Number.isFinite(sec) || sec < 0) return "";
    if (sec < 60) return "age-fresh";
    if (sec < 3600) return "age-recent";
    return "age-stale";
}

// Find a row's freshness CSS class from its first `*Sec` scalar field, or "" if it has
// none. `self` is always "now" → fresh; a `cached` row (restored, not re-seen) is
// unknown-age → no dot (the detail says "cached"). Generic over field names.
function rowAgeClass(item) {
    if (!item || typeof item !== "object") return "";
    if (item.self) return "age-fresh";
    if (item.cached) return "";
    for (const [k, v] of Object.entries(item)) {
        if (k.endsWith("Sec") && typeof v === "number") return ageBucketClass(v);
    }
    return "";
}

// Render a seconds-ago count as a short relative time ("just now", "2m ago", "3h ago",
// "5d ago"). Snapshot at state-push time — it refreshes when the list re-renders, not
// per second. Mirrors the device-side ageSec (now - lastSeenMs); kept simple on purpose.
function relativeAge(sec) {
    if (!Number.isFinite(sec) || sec < 0) return "—";
    if (sec < 10) return "just now";
    if (sec < 60) return `${sec}s ago`;
    if (sec < 3600) return `${Math.floor(sec / 60)}m ago`;
    if (sec < 86400) return `${Math.floor(sec / 3600)}h ago`;
    return `${Math.floor(sec / 86400)}d ago`;
}

function appendResetButton(row, moduleName, ctrl, def, applyVisually) {
    if (def === undefined || def === null) return;  // type not loaded yet or no default
    const btn = document.createElement("button");
    btn.className = "reset-btn";
    btn.type = "button";
    btn.textContent = "↺";
    btn.title = `Reset to default (${def})`;
    btn.dataset.mid = moduleName;
    btn.dataset.key = ctrl.name + ".reset";
    btn.dataset.def = String(def);
    const eq = controlValuesEqual(ctrl, def);
    btn.classList.toggle("active", !eq);
    btn.addEventListener("click", () => {
        applyVisually();
        sendControl(moduleName, ctrl.name, def);
    });
    row.appendChild(btn);
}

function debounceSend(key, ms, fn) {
    clearTimeout(dragTimers[key]);
    dragTimers[key] = setTimeout(fn, ms);
}

// Password controls arrive XOR-obfuscated + base64-encoded (see
// HttpServerModule PASSWORD_XOR_KEY). This reverses it. The XOR key is a fixed
// shared constant, not a secret — this is obfuscation so the password is not
// plainly readable in a raw /api/state response, not real encryption.
const PW_XOR_KEY = 0x5A;
function decodePassword(encoded) {
    if (!encoded) return "";
    try {
        const bytes = atob(encoded);
        let out = "";
        for (let i = 0; i < bytes.length; i++) {
            out += String.fromCharCode(bytes.charCodeAt(i) ^ PW_XOR_KEY);
        }
        return out;
    } catch {
        return "";
    }
}

function fmtTime(sec) {
    sec = Math.max(0, Math.floor(Number(sec) || 0));
    const d = Math.floor(sec / 86400); sec -= d * 86400;
    const h = Math.floor(sec / 3600);  sec -= h * 3600;
    const m = Math.floor(sec / 60);    sec -= m * 60;
    const parts = [];
    if (d) parts.push(d + "d");
    if (d || h) parts.push(h + "h");
    if (d || h || m) parts.push(m + "m");
    parts.push(sec + "s");
    return parts.join(" ");
}

function fmtProgressLabel(ctrl) {
    const v = Number(ctrl.value) || 0;
    const t = Number(ctrl.total) || 0;
    // bytes === false → a plain count (e.g. a scan position "37 / 254"); otherwise
    // KB (the heap / flash / filesystem gauges, the original use).
    if (ctrl.bytes === false) return v + " / " + t;
    return Math.round(v / 1024) + "KB / " + Math.round(t / 1024) + "KB";
}

// "<value> <unit>" — treats null / undefined / 0 as "unavailable" so the
// UI doesn't render bogus "0 dBm" when the device is in a state where the
// metric isn't meaningful. The device's updateMetrics() writes 0 to rssi /
// txPower in non-WiFi states; the control is hidden in those states, but
// if anyone toggles hidden off (DevTools, future code path) the unit-with-
// zero rendering would still mislead. Real metric values are never zero
// in practice — RSSI is negative, TX power is 0..127 dBm (zero only on
// driver-uninitialised reads).
function fmtDisplayInt(ctrl) {
    const v = ctrl.value;
    const u = ctrl.unit || "";
    if (v === null || v === undefined || v === 0) return "";
    return u ? `${v} ${u}` : String(v);
}

// ---------------------------------------------------------------------------
// 5. State patching (no-rebuild contract)
// ---------------------------------------------------------------------------

function updateValues() {
    if (!state || !state.modules) return;
    // Patch each visible card's controls and stats line; never rebuild the DOM here.
    for (const mod of allModules()) {
        updateModuleControls(mod);
        // refresh the stats line for this module if visible
        const statsEl = document.querySelector(`.card-stats[data-mid="${cssEscape(mod.name)}"]`);
        if (statsEl) { statsEl.textContent = formatStats(mod); statsEl.title = formatStatsTitle(mod); }
        // refresh status row — insert it if status appeared after card build
        let statusRow = document.querySelector(`[data-status-mid="${cssEscape(mod.name)}"]`);
        if (mod.status) {
            if (!statusRow) {
                // Card exists but had no status at build time — insert now before first control.
                const card = document.querySelector(`.card[data-module="${cssEscape(mod.name)}"]`);
                const host = card && (card.querySelector(".card-controls-collapse") || card);
                if (host) {
                    statusRow = document.createElement("div");
                    statusRow.className = "control-row";
                    statusRow.dataset.statusMid = mod.name;
                    const label = document.createElement("span");
                    label.className = "control-label";
                    label.textContent = "status";
                    const val = document.createElement("span");
                    val.className = "status-value";
                    val.dataset.sev = mod.severity || "status";
                    val.textContent = mod.status;
                    statusRow.appendChild(label);
                    statusRow.appendChild(val);
                    // Insert before first .control-row, or append.
                    const firstRow = host.querySelector(".control-row");
                    firstRow ? host.insertBefore(statusRow, firstRow) : host.appendChild(statusRow);
                }
            } else {
                statusRow.style.display = "";
                const val = statusRow.querySelector(".status-value");
                if (val) { val.textContent = mod.status; val.dataset.sev = mod.severity || "status"; }
            }
        } else if (statusRow) {
            statusRow.style.display = "none";
        }
        // refresh enabled toggle (now a styled <button>, not an <input>)
        const enabledEl = document.querySelector(`button.module-enabled[data-mid="${cssEscape(mod.name)}"]`);
        if (enabledEl) {
            const ts = dragTs[mod.name + ":enabled"] || 0;
            if (Date.now() - ts > 1000) {
                const on = (mod.enabled === undefined) ? true : !!mod.enabled;
                enabledEl.dataset.checked = on ? "true" : "false";
                enabledEl.textContent = "⏻";
                enabledEl.classList.toggle("module-enabled--off", !on);
                enabledEl.setAttribute("aria-pressed", on ? "true" : "false");
                const cardEl = document.querySelector(`.card[data-module="${cssEscape(mod.name)}"]`);
                if (cardEl) cardEl.classList.toggle("card--disabled", !on);
            }
        }
    }
    updateStatusBar();
}

function allModules() {
    const out = [];
    function walk(modules) {
        for (const m of modules) {
            out.push(m);
            if (m.children) walk(m.children);
        }
    }
    if (state && state.modules) walk(state.modules);
    return out;
}

// Reconcile a card's control rows when its set of VISIBLE controls changed (a
// `hidden` flag flipped at runtime, e.g. NetworkModule's static-IP fields or
// RmtLedDriver's loopbackRxPin). The value-patch path in updateModuleControls
// can't add or remove rows, so this handles that half. Returns true if it
// changed the DOM. No-op (returns false) on the common frame where nothing moved.
//
// Position-stable by design: it inserts each newly-visible row at its correct
// index among the existing control rows and removes rows that became hidden —
// it does NOT tear down and re-append every row (that would land them after the
// card's child-module block / install-picker mount, never converge, and re-fire
// every WS tick — a render loop that wedges the UI).
function syncVisibleControls(mod) {
    const card = document.querySelector(`.card[data-module="${cssEscape(mod.name)}"]`);
    if (!card) return false;
    // The controls host is THIS card's own collapse wrapper — must be a DIRECT
    // child (`:scope >`), not any descendant: a container card (e.g. Layers) nests
    // its child cards (Layer) inside .card-children, and a plain
    // `card.querySelector(".card-controls-collapse")` would reach down and match
    // the CHILD's wrapper. That made Layers adopt Layer's control rows as its own,
    // so both cards saw a control-set mismatch every WS frame and rebuilt each
    // other's rows in a loop — tearing down (and closing) any open <select>.
    const host = card.querySelector(":scope > .card-controls-collapse") || card;

    const wantNames = mod.controls.filter(c => !c.hidden).map(c => c.name);
    const haveRows = [...host.querySelectorAll(":scope > .control-row[data-key]")];
    const haveNames = haveRows.map(r => r.dataset.key);
    if (wantNames.length === haveNames.length && wantNames.every((n, i) => n === haveNames[i])) {
        return false;  // unchanged — the common case
    }

    // Remove rows whose control is no longer visible.
    const wantSet = new Set(wantNames);
    for (const row of haveRows) {
        if (!wantSet.has(row.dataset.key)) row.remove();
    }
    // Insert each visible control's row at its correct position. The anchor is the
    // first existing control row that should come AFTER this one; null → append
    // before the children block (insertBefore(node, null) appends to host's end,
    // but control rows precede .card-children which lives on the card, not here).
    const visibleControls = mod.controls.filter(c => !c.hidden);
    for (let i = 0; i < visibleControls.length; i++) {
        const name = visibleControls[i].name;
        if (host.querySelector(`:scope > .control-row[data-key="${cssEscape(name)}"]`)) continue;
        const row = createControl(mod.name, mod.type, visibleControls[i]);
        if (!row) continue;
        // Anchor: the rendered row of the next visible control that already exists.
        let anchor = null;
        for (let j = i + 1; j < visibleControls.length && !anchor; j++) {
            anchor = host.querySelector(`:scope > .control-row[data-key="${cssEscape(visibleControls[j].name)}"]`);
        }
        // No later control row exists yet → keep this row above the card's
        // children block / install-picker mount / footer (which live in the host
        // when host===card), so controls never render below the children.
        if (!anchor) {
            anchor = host.querySelector(":scope > .card-children")
                  || host.querySelector(":scope > .install-picker-host")
                  || host.querySelector(":scope > .card-footer");
        }
        host.insertBefore(row, anchor);
    }
    return true;
}

function updateModuleControls(mod) {
    if (!mod.controls) return;

    // Conditional controls: a module can flip a control's `hidden` flag at runtime
    // (e.g. RmtLedDriver reveals loopbackRxPin while the test is on, NetworkModule
    // reveals static-IP fields). The value-patch loop below only updates controls
    // already in the DOM — it can't add or remove one. So first detect whether the
    // set of VISIBLE controls drifted from what's rendered, and if so re-render
    // this card's control rows. Cheap: only fires on the rare frame where a hidden
    // flag actually changed.
    if (syncVisibleControls(mod)) return;  // re-rendered — values are fresh, skip patch

    for (const ctrl of mod.controls) {
        const mid = cssEscape(mod.name);
        const k = cssEscape(ctrl.name);
        const dragKey = mod.name + ":" + ctrl.name;
        const ts = dragTs[dragKey] || 0;
        const userActive = Date.now() - ts < 1000;

        // One guard for every editable control: while the user is mid-edit (a
        // keystroke / drag within the last second, tracked by dragTs), don't let a
        // WS state push overwrite the field with the value it had before the edit
        // landed. The read-only types (display/display-int/time/progress) and the
        // composite `list` aren't in this set — they always reflect the latest push.
        if (userActive && EDITABLE_CONTROL_TYPES.has(ctrl.type)) continue;

        switch (ctrl.type) {
            case "uint8":
            case "uint16":
            case "int16":
            case "pin": {   // pin is a plain number input (no slider sibling); patches the same way
                const input = document.querySelector(`input[data-mid="${mid}"][data-key="${k}"]`);
                if (input && Number(input.value) !== Number(ctrl.value)) {
                    input.value = ctrl.value ?? 0;
                    const val = input.nextElementSibling;
                    if (val && val.classList.contains("control-value-input")) val.value = ctrl.value ?? 0;
                }
                break;
            }
            case "bool": {
                const input = document.querySelector(`input[data-mid="${mid}"][data-key="${k}"]`);
                if (input && input.checked !== !!ctrl.value) input.checked = !!ctrl.value;
                break;
            }
            case "text": {
                const input = document.querySelector(`input[type="text"][data-mid="${mid}"][data-key="${k}"]`);
                if (input && input.value !== (ctrl.value ?? "")) input.value = ctrl.value ?? "";
                break;
            }
            case "textarea": {
                const input = document.querySelector(`textarea[data-mid="${mid}"][data-key="${k}"]`);
                // Don't clobber the box while the user is typing in it.
                if (input && document.activeElement !== input && input.value !== (ctrl.value ?? "")) input.value = ctrl.value ?? "";
                break;
            }
            case "password": {
                // The peek button flips the input to type="text", so match either.
                const input = document.querySelector(`input[data-mid="${mid}"][data-key="${k}"]`);
                const decoded = decodePassword(ctrl.value);
                if (input && input.value !== decoded) input.value = decoded;
                break;
            }
            case "select": {
                const sel = document.querySelector(`select[data-mid="${mid}"][data-key="${k}"]`);
                // Never overwrite a select the user currently has OPEN (popup
                // showing) or focused. data-open is set on pointerdown/focus and
                // cleared on change/blur — more reliable than document.activeElement,
                // which is ambiguous while a native popup is up (the popup is a
                // separate OS layer on macOS). The 1s dragTs cooldown is the
                // additional fallback for the frames right after the popup closes.
                if (sel && sel.dataset.open !== "true" && sel !== document.activeElement &&
                    Number(sel.value) !== Number(ctrl.value)) sel.value = ctrl.value;
                break;
            }
            case "palette": {
                // Custom dropdown: patch the trigger (swatch + name) and the selected row, but not
                // while the user has the list open (data-open === "true").
                const wrap = document.querySelector(`.palette-control[data-mid="${mid}"][data-key="${k}"]`);
                if (wrap && wrap.dataset.open !== "true" && Number(wrap.dataset.value) !== Number(ctrl.value)) {
                    wrap.dataset.value = ctrl.value;
                    const cols = ((ctrl.options || [])[ctrl.value] || {}).colors || "";
                    const stops = cols.split(/\s+/).filter(Boolean).map(h => "#" + h);
                    const grad = stops.length ? `linear-gradient(to right, ${stops.join(",")})` : "none";
                    const triSwatch = wrap.querySelector(".palette-trigger .palette-swatch");
                    if (triSwatch) triSwatch.style.background = grad;
                    const triName = wrap.querySelector(".palette-trigger .palette-name");
                    if (triName) triName.textContent = ((ctrl.options || [])[ctrl.value] || {}).name || String(ctrl.value);
                    wrap.querySelectorAll(".palette-item.selected").forEach(x => x.classList.remove("selected"));
                    const row = wrap.querySelector(`.palette-item[data-idx="${ctrl.value}"]`);
                    if (row) row.classList.add("selected");
                }
                break;
            }
            case "display": {
                const span = document.querySelector(`span.display[data-mid="${mid}"][data-key="${k}"]`);
                if (span) span.textContent = ctrl.value ?? "";
                break;
            }
            case "display-int": {
                const span = document.querySelector(`span.display[data-mid="${mid}"][data-key="${k}"]`);
                if (span) {
                    // Re-cache the unit in case the device changed it (it
                    // shouldn't, but the WS path is the authority).
                    span.dataset.unit = ctrl.unit ?? span.dataset.unit ?? "";
                    span.textContent = fmtDisplayInt(ctrl);
                }
                break;
            }
            case "ipv4": {
                // Guarded by the shared userActive check above (same as text).
                const input = document.querySelector(`input.ipv4-input[data-mid="${mid}"][data-key="${k}"]`);
                if (input && input.value !== (ctrl.value ?? "")) input.value = ctrl.value ?? "";
                break;
            }
            case "time": {
                const span = document.querySelector(`span.display[data-mid="${mid}"][data-key="${k}"]`);
                if (span) span.textContent = fmtTime(ctrl.value ?? 0);
                break;
            }
            case "progress": {
                const bar = document.querySelector(`progress[data-mid="${mid}"][data-key="${k}"]`);
                if (bar) {
                    bar.value = ctrl.value ?? 0;
                    bar.max = ctrl.total ?? 100;
                }
                const lbl = document.querySelector(`span.control-value[data-mid="${mid}"][data-key="${k}.label"]`);
                if (lbl) lbl.textContent = fmtProgressLabel(ctrl);
                break;
            }
            case "list": {
                // Rows change wholesale between scans, so rebuild the list's entries
                // in place rather than patching individual fields. Preserves which
                // detail panels were open by summary text (best-effort) so a live
                // refresh doesn't collapse a row the user just expanded.
                const list = document.querySelector(`div.list-control[data-mid="${mid}"][data-key="${k}"]`);
                if (!list) break;
                // Preserve which detail panels are currently open (by summary text) so
                // a live refresh doesn't collapse a row the user just expanded.
                const open = new Set(
                    [...list.querySelectorAll(".list-entry")]
                        .filter(e => { const d = e.querySelector(".list-detail"); return d && !d.hidden; })
                        .map(e => e.querySelector(".list-summary")?.textContent));
                const rows = Array.isArray(ctrl.value) ? ctrl.value : [];
                const details = Array.isArray(ctrl.detail) ? ctrl.detail : [];
                buildListEntries(list, rows, details, open);
                break;
            }
        }
        // Reset-button state may change as the value drifts in/out of default.
        // Defaults live in availableTypes (populated from /api/types) keyed by module type.
        const def = defaultFor(mod.type, ctrl.name);
        if (def !== undefined && def !== null) {
            const btn = document.querySelector(`button.reset-btn[data-mid="${mid}"][data-key="${k}.reset"]`);
            if (btn) {
                const eq = controlValuesEqual(ctrl, def);
                btn.classList.toggle("active", !eq);
            }
        }
    }
}

// Per-type equality for reset-button highlighting. bool→boolish, ipv4/text→
// string compare, everything else → numeric. Centralised so the rules can't
// drift between createControl and updateModuleControls.
function controlValuesEqual(ctrl, def) {
    if (ctrl.type === "bool") return !!ctrl.value === !!def;
    if (ctrl.type === "ipv4" || ctrl.type === "text" || ctrl.type === "textarea" || ctrl.type === "password") {
        return String(ctrl.value ?? "") === String(def ?? "");
    }
    return Number(ctrl.value) === Number(def);
}

function cssEscape(s) {
    // Minimal CSS attribute selector escape. Module/control names are alphanumeric
    // in practice, so this is defensive.
    return String(s).replace(/(["\\])/g, "\\$1");
}

// ---------------------------------------------------------------------------
// 6. Type picker
// ---------------------------------------------------------------------------

// Role → emoji. The role part of the MoonLight emoji-key system
// (https://moonmodules.org/MoonLight/moonlight/overview/#emoji-key):
// 🔥 effect · 💎 modifier · 🚥 layout · ☸️ driver · 🥞 layer (projectMM
// addition — every Layer instance, child of the Layers container). The role
// tag is derived here, not duplicated in every module's tags() string — one
// source of truth in the UI saves repeating the same character in ~30 module
// headers and a few bytes per type in /api/types. Each module's tags() then
// only carries its categorical origin (🐙 WLED · 💫 MoonLight · ⚡️ FastLED)
// and any feature extras (audio: ♫ FFT · ♪ volume · moving-head: 🚨 colour ·
// 🗼 movement). The dimensional emoji (📏 1D · 🟦 2D · 🧊 3D) is derived from
// the type's `dim` field. All three are merged in emojiTagsFor().
const ROLE_EMOJI = {
    effect:     "🔥",
    driver:     "☸️",
    modifier:   "💎",
    layout:     "🚥",
    layer:      "🥞",
    peripheral: "🛰️",
    generic:    "⚙️",
};

// Dim int → emoji. Only effects carry `dim` (1/2/3); other modules have dim == 0
// and contribute nothing here. Same MoonLight key. Keeps emojiTagsFor() the
// single place that assembles the chip set per type.
const DIM_EMOJI = {
    1: "📏",
    2: "🟦",
    3: "🧊",
};

// Split a string into grapheme clusters so multi-codepoint emoji (e.g. 🌫️,
// which is base char + variation selector) stay whole. Falls back to a plain
// code-point split if Intl.Segmenter is unavailable.
const _graphemeSeg = (typeof Intl !== "undefined" && Intl.Segmenter)
    ? new Intl.Segmenter(undefined, {granularity: "grapheme"})
    : null;
function graphemes(s) {
    if (!s) return [];
    if (_graphemeSeg) return [..._graphemeSeg.segment(s)].map(seg => seg.segment);
    return [...s];
}

// All emoji for a type: role first, then dimensional (effects only), then each
// curated tag emoji from tags(). Deduplicated, order preserved.
function emojiTagsFor(t) {
    const out = [];
    const seen = new Set();
    const push = (ch) => { if (ch && !seen.has(ch)) { seen.add(ch); out.push(ch); } };
    push(ROLE_EMOJI[t.role]);
    push(DIM_EMOJI[t.dim]);
    for (const ch of graphemes(t.tags || "")) push(ch);
    return out;
}

// The type picker serves two modes:
//  - add (default): pick a type to create as a child of parentMod.
//  - replace: pick a type to swap parentMod for, at the same position.
// They differ only in the role filter and the commit action; the search box,
// list, and keyboard nav are shared.
function openTypePicker(parentMod, anchorEl) {
    openPicker(anchorEl, {
        roles: rolesAcceptedBy(parentMod),
        actionLabel: "create",
        commit: (type) => addModule(type, parentMod.name)
    });
}

// Replace mode: filter to the target module's own role (effect ↔ effect), and
// pre-select the module's CURRENT type so the cursor lands on it (not the first row).
function openReplacePicker(targetMod, anchorEl) {
    openPicker(anchorEl, {
        roles: [targetMod.role],
        actionLabel: "replace",
        currentType: targetMod.type,
        commit: (type) => replaceModule(targetMod.name, type)
    });
}

function openPicker(anchorEl, opts) {
    // Close any existing picker
    document.querySelectorAll(".type-picker").forEach(p => p.remove());

    const filtered = availableTypes.filter(t => opts.roles.includes(t.role));

    const picker = document.createElement("div");
    picker.className = "type-picker";

    const search = document.createElement("input");
    search.type = "text";
    search.placeholder = "search…";
    search.className = "type-picker-search";
    picker.appendChild(search);

    // Emoji chip filter row — every distinct emoji across the role-filtered types.
    // Toggling chips narrows the list (AND: a type must carry all active chips).
    const activeChips = new Set();
    const chipRow = document.createElement("div");
    chipRow.className = "type-picker-chips";
    const chipEmoji = [];
    const chipSeen = new Set();
    for (const t of filtered) {
        for (const ch of emojiTagsFor(t)) {
            if (!chipSeen.has(ch)) { chipSeen.add(ch); chipEmoji.push(ch); }
        }
    }
    for (const emoji of chipEmoji) {
        const chip = document.createElement("button");
        chip.className = "type-picker-chip";
        chip.textContent = emoji;
        chip.addEventListener("click", () => {
            if (activeChips.has(emoji)) { activeChips.delete(emoji); chip.classList.remove("active"); }
            else { activeChips.add(emoji); chip.classList.add("active"); }
            refresh();
        });
        chipRow.appendChild(chip);
    }
    if (chipEmoji.length > 0) picker.appendChild(chipRow);

    const list = document.createElement("div");
    list.className = "type-picker-list";
    picker.appendChild(list);

    const actions = document.createElement("div");
    actions.className = "type-picker-actions";
    const cancelBtn = document.createElement("button");
    cancelBtn.textContent = "cancel";
    cancelBtn.addEventListener("click", () => picker.remove());
    const createBtn = document.createElement("button");
    createBtn.className = "create";
    createBtn.textContent = opts.actionLabel;
    createBtn.disabled = true;
    actions.appendChild(cancelBtn);
    actions.appendChild(createBtn);
    picker.appendChild(actions);

    let selectedType = null;

    // Types matching the search box AND all active emoji chips. The query matches
    // against both the raw typeName ("RainbowEffect") and the displayName
    // ("Rainbow") supplied by /api/types so typing either form finds the row.
    function currentMatches() {
        const q = search.value.toLowerCase();
        return filtered.filter(t => {
            if (q) {
                const raw = t.name.toLowerCase();
                const disp = (t.displayName || t.name).toLowerCase();
                if (!raw.includes(q) && !disp.includes(q)) return false;
            }
            if (activeChips.size > 0) {
                const has = new Set(emojiTagsFor(t));
                for (const chip of activeChips) if (!has.has(chip)) return false;
            }
            return true;
        });
    }

    function refresh() {
        const matches = currentMatches();
        list.innerHTML = "";
        // Highlight the module's current type if it's in the list (replace mode lands the cursor
        // on what's already there); otherwise the first row.
        let selIdx = opts.currentType ? matches.findIndex(t => t.name === opts.currentType) : -1;
        if (selIdx < 0) selIdx = 0;
        matches.forEach((t, i) => {
            const item = document.createElement("div");
            item.className = "type-picker-item" + (i === selIdx ? " selected" : "");
            const emoji = document.createElement("span");
            emoji.className = "type-picker-item-emoji";
            emoji.textContent = emojiTagsFor(t).join("");
            item.appendChild(emoji);
            // Show the factory-stripped name ("Rainbow") not the typeName
            // ("RainbowEffect"); the role text on the right already conveys
            // "effect", so repeating it in the name would just be noise.
            item.appendChild(document.createTextNode(t.displayName || t.name));
            const role = document.createElement("span");
            role.className = "role";
            role.textContent = t.role;
            item.appendChild(role);
            item.addEventListener("click", () => {
                list.querySelectorAll(".selected").forEach(x => x.classList.remove("selected"));
                item.classList.add("selected");
                selectedType = t.name;
                createBtn.disabled = false;
            });
            item.addEventListener("dblclick", () => {
                opts.commit(t.name);
                picker.remove();
            });
            list.appendChild(item);
        });
        selectedType = matches.length > 0 ? matches[selIdx].name : null;
        createBtn.disabled = !selectedType;
        // Scroll the pre-selected row into view (it may be below the fold for a long list).
        const selEl = list.querySelector(".type-picker-item.selected");
        if (selEl) selEl.scrollIntoView({ block: "nearest" });
    }

    search.addEventListener("input", refresh);
    search.addEventListener("keydown", (e) => {
        const items = Array.from(list.querySelectorAll(".type-picker-item"));
        const sel = list.querySelector(".selected");
        const idx = items.indexOf(sel);
        if (e.key === "ArrowDown") {
            e.preventDefault();
            if (idx < items.length - 1) {
                sel?.classList.remove("selected");
                items[idx + 1].classList.add("selected");
                selectedType = filteredAt(idx + 1)?.name;
            }
        } else if (e.key === "ArrowUp") {
            e.preventDefault();
            if (idx > 0) {
                sel?.classList.remove("selected");
                items[idx - 1].classList.add("selected");
                selectedType = filteredAt(idx - 1)?.name;
            }
        } else if (e.key === "Enter") {
            e.preventDefault();
            if (selectedType) {
                opts.commit(selectedType);
                picker.remove();
            }
        } else if (e.key === "Escape") {
            picker.remove();
        }
    });

    function filteredAt(i) {
        return currentMatches()[i];
    }

    createBtn.addEventListener("click", () => {
        if (selectedType) {
            opts.commit(selectedType);
            picker.remove();
        }
    });

    anchorEl.appendChild(picker);
    refresh();
    search.focus();
}

// ---------------------------------------------------------------------------
// 7. Drag-to-reorder (HTML5 DnD on desktop; touchstart-gated on mobile)
// ---------------------------------------------------------------------------

function attachDragHandlers(card, mod) {
    card.draggable = true;

    // Why we toggle `draggable` on mousedown instead of vetoing in dragstart:
    // HTML5 dragstart's `e.target` is always the draggable element (the card),
    // not the deepest element under the mouse — so closest(".control-row")
    // never matches. The reliable signal is the *mousedown* target. Disable
    // drag at mousedown when the grab landed on a control, re-enable on
    // mouseup so the next click on the card body can still drag.
    const gate = (e) => {
        card.draggable = !e.target.closest(".control-row, .card-controls-collapse > summary");
    };
    card.addEventListener("mousedown", gate, true);   // capture: runs before the input
    card.addEventListener("touchstart", gate, {capture: true, passive: true});

    card.addEventListener("dragstart", (e) => {
        // Innermost card wins — without stopPropagation a nested child's
        // dragstart would bubble to the parent and the parent's listener
        // would overwrite dataTransfer with its own name.
        e.stopPropagation();
        e.dataTransfer.effectAllowed = "move";
        e.dataTransfer.setData("text/plain", mod.name);
        card.classList.add("dragging");
    });
    card.addEventListener("dragend", () => {
        card.classList.remove("dragging");
        document.querySelectorAll(".drag-over").forEach(c => c.classList.remove("drag-over"));
    });
    card.addEventListener("dragover", (e) => {
        // Only allow drop on a true sibling — same .card-children container.
        // Cards now nest, so equal data-depth is no longer enough: two effects
        // under different Layers share a depth but aren't siblings.
        const src = document.querySelector(".card.dragging");
        if (!src || src === card) return;
        if (src.parentElement === card.parentElement &&
            card.parentElement &&
            card.parentElement.classList.contains("card-children")) {
            e.preventDefault();
            card.classList.add("drag-over");
        }
    });
    card.addEventListener("dragleave", () => {
        card.classList.remove("drag-over");
    });
    card.addEventListener("drop", (e) => {
        e.preventDefault();
        // Innermost card wins — without stopPropagation the drop bubbles to every
        // ancestor card that also has a drop handler, firing a SECOND move onto
        // the grandparent's child list (e.g. dropping onto Mirror also dropped
        // onto the Layer card → move into Layers, index 0 → undoing the first
        // move). Same reason dragstart stops propagation above.
        e.stopPropagation();
        card.classList.remove("drag-over");
        const srcName = e.dataTransfer.getData("text/plain");
        if (!srcName || srcName === mod.name) return;
        // Insert semantics (not swap): the dropped item takes the target row's
        // slot and the others shift to fill — the standard reorderable-list
        // behaviour (Finder, Trello, VS Code, SortableJS). Because we drop ONTO a
        // row (not into a between-rows gap), the landing is the target's absolute
        // index: dragging down lands after the target, dragging up lands before
        // it. That's consistent ("take the target's slot"), just not always-before.
        //
        // Compute target absolute index within parent's children. Identify the
        // drop-target by name, not by object identity — state is replaced on
        // every WS push, so `mod` captured at render time is stale within ~1s.
        const parent = findParent(mod.name);
        if (!parent) return;
        const targetIdx = (parent.children || []).findIndex(c => c.name === mod.name);
        if (targetIdx < 0) return;
        moveModuleTo(srcName, targetIdx);
    });
}

// ---------------------------------------------------------------------------
// 8. Status bar wiring
// ---------------------------------------------------------------------------

function setupStatusBarButtons() {
    document.getElementById("preview-reset")?.addEventListener("click", () => {
        preview.resetCamera();
    });

    // Reboot: press once to arm, again to confirm — see armPressTwice. The glyph
    // stays (no armedText); only the title changes.
    const rebootBtn = document.getElementById("reboot-btn");
    if (rebootBtn) {
        armPressTwice(rebootBtn, rebootDevice, {armedTitle: "Click again to reboot"});
    }
    document.getElementById("theme-toggle")?.addEventListener("click", () => {
        theme = (theme === "dark") ? "light" : "dark";
        localStorage.setItem(LS_THEME, theme);
        applyTheme(theme);
        // Repaint the preview to the new theme's background — a live preview would
        // pick it up on its next frame, but an idle one (no incoming frames) needs
        // a nudge so the canvas doesn't keep the previous theme's clear colour.
        preview.redraw();
    });

    // Hamburger: toggles the side nav. On wide screens it collapses/expands the
    // static column; on narrow screens (<820px) the same class drives a slide-in
    // drawer over an overlay (CSS handles the responsive difference).
    document.getElementById("nav-toggle")?.addEventListener("click", () => {
        document.body.classList.toggle("nav-open");
    });
    document.getElementById("nav-overlay")?.addEventListener("click", closeNavDrawer);
    document.addEventListener("keydown", (e) => {
        if (e.key === "Escape") closeNavDrawer();
    });
}

// Close the side nav. On wide screens this collapses the column; on narrow
// screens it dismisses the slide-in drawer + overlay.
function closeNavDrawer() {
    document.body.classList.remove("nav-open");
}

function applyTheme(t) {
    document.body.dataset.theme = t;
    const btn = document.getElementById("theme-toggle");
    if (btn) btn.textContent = (t === "dark") ? "☀" : "🌙";
}

function updateStatusBar() {
    if (!state || !state.modules) return;
    const sys = state.modules.find(m => m.type === "SystemModule")
             || state.modules.find(m => m.name === "System");
    if (!sys) return;
    const ctrls = sys.controls || [];

    // Device name → header span + document.title
    const nameCtrl = ctrls.find(c => c.name === "deviceName");
    const nameSpan = document.getElementById("device-name");
    if (nameCtrl && nameSpan && nameCtrl.value) {
        nameSpan.textContent = nameCtrl.value;
        if (document.title !== "projectMM — " + nameCtrl.value) {
            document.title = "projectMM — " + nameCtrl.value;
        }
    }

    // System stats: "uptime · 🧠 NNK · 🧱 NNKB"
    // 🧠 = internal-RAM free (heap progress total−used); 🧱 = largest contiguous
    // internal-RAM block (the maxBlock control, already maxInternalAllocBlock).
    // Both matter: free can be ample while fragmentation leaves no single block
    // big enough for the next allocation.
    const uptimeCtrl = ctrls.find(c => c.name === "uptime");
    const heapCtrl = ctrls.find(c => c.name === "heap");
    const blockCtrl = ctrls.find(c => c.name === "maxBlock");
    const statsEl = document.getElementById("sys-stats");
    if (statsEl) {
        const parts = [];
        if (uptimeCtrl) parts.push(uptimeCtrl.value);
        if (heapCtrl && heapCtrl.value !== undefined && heapCtrl.total) {
            const freeKb = Math.round((heapCtrl.total - heapCtrl.value) / 1024);
            parts.push("🧠 " + freeKb + "K");
        }
        // Skip on desktop, where the platform stub reports "0KB" (no real
        // block measurement / unlimited heap) — same reason heap free above
        // only shows when the heap progress control is present.
        if (blockCtrl && blockCtrl.value && blockCtrl.value !== "0KB") {
            parts.push("🧱 " + blockCtrl.value);
        }
        statsEl.textContent = parts.join(" · ");
    }

    // Hide reboot button on desktop builds — platform::reboot() just exits the process,
    // which is not useful from the UI and can be mistaken for a crash.
    const chipCtrl = ctrls.find(c => c.name === "chip");
    const rebootBtn = document.getElementById("reboot-btn");
    if (rebootBtn && chipCtrl) {
        rebootBtn.hidden = chipCtrl.value === "desktop";
    }

    // bootReason → crashed-state styling on reboot button
    const reasonCtrl = ctrls.find(c => c.name === "bootReason");
    if (rebootBtn && reasonCtrl) {
        const crashed = ["PANIC", "INT_WDT", "TASK_WDT", "BROWNOUT"].includes(reasonCtrl.value);
        rebootBtn.dataset.crashed = crashed ? "true" : "false";
        if (crashed) rebootBtn.title = "Last boot: " + reasonCtrl.value + " (click to reboot)";
        else rebootBtn.title = "Reboot device";
    }

    // Cache-first update check: instant from the localStorage cache, background-fetches only
    // when stale (>1 h). Fire-and-forget — best-effort, never blocks the status-bar render.
    checkFirmwareUpdate(false);
}

// ---------------------------------------------------------------------------
// 8b. Firmware-update badge
// ---------------------------------------------------------------------------
// Browser-side "a newer firmware is out" check, modelled on ESP32-sveltekit's
// UpdateIndicator (the upstream firmware lineage) — our own code. The device fetches
// nothing; the browser compares the running version (FirmwareUpdateModule.version, pure
// semver) against GitHub releases and, when newer AND a compatible .bin exists, shows the
// status-bar badge. Two channels:
//   - STABLE: a device compares against the newest stable release (the /latest endpoint
//     excludes prereleases). Applies to every device.
//   - DEV (latest): a device already on a prerelease build (-dev.<N>) ALSO compares against
//     the moving `latest` release, so a stale latest build is nudged to the newest latest.
//     The `latest` release's tag is "latest" (not a semver), so its version is read from the
//     per-firmware manifest (manifest-<firmware>.json carries "version", e.g. 2.1.0-dev.7).
// A stable update wins over a dev update. Cached in localStorage (1 h TTL) so it doesn't
// slow page load; a fresh check is forced when the Firmware card opens. Best-effort: any
// failure hides the badge, never throws.

const RELEASES_API = "https://api.github.com/repos/MoonModules/projectMM/releases";
const UPDATE_TTL_MS = 60 * 60 * 1000;                     // 1 h — best-effort, well under GitHub's rate limit
const PICKER_RELEASE_KEY = "projectMM.picker.releaseTag"; // install-picker restores from this on init

function safeLocalGet(key) { try { return localStorage.getItem(key); } catch (_) { return null; } }
function safeLocalSet(key, v) { try { localStorage.setItem(key, v); } catch (_) { /* ignore */ } }

// In-flight fetches keyed by cache slot. updateStatusBar() calls checkFirmwareUpdate(false)
// every WS tick (1 Hz); on a cold cache they'd each start a duplicate releases/latest request
// before the first writes the cache. Share the pending promise so concurrent callers reuse it.
const inFlightFetches = {};

// A cached JSON fetch: returns the parsed body, re-fetching only when the cache is older than
// the TTL or `force` is set, and serving stale on a fetch failure. `key` is the cache slot.
async function cachedJson(url, key, force) {
    if (!force) {
        const raw = safeLocalGet(key);
        if (raw) {
            try {
                const obj = JSON.parse(raw);
                if (Date.now() - obj.ts < UPDATE_TTL_MS) return obj.data;
            } catch (_) { /* fall through to fetch */ }
        }
    }
    // Coalesce concurrent fetches for the same slot onto one request.
    if (inFlightFetches[key]) return inFlightFetches[key];
    const p = (async () => {
        try {
            const res = await fetch(url, { headers: { accept: "application/json" } });
            if (!res.ok) throw new Error(`HTTP ${res.status}`);
            const data = await res.json();
            safeLocalSet(key, JSON.stringify({ ts: Date.now(), data }));
            return data;
        } catch (e) {
            // console.debug, not warn: an update check failing is routine and not
            // actionable (the device may simply be offline, or GitHub rate-limited),
            // so keep it out of the default console — debug is hidden unless the user
            // opts into verbose. Both callers hit api.github.com, which sends
            // Access-Control-Allow-Origin and so reads fine from the device origin;
            // the failure path here is for the no-network / rate-limit case.
            console.debug("[update] fetch failed:", url, e && e.message ? e.message : e);
            const raw = safeLocalGet(key);                   // serve stale on failure
            if (raw) {
                try {
                    const obj = JSON.parse(raw);
                    // Refresh the timestamp so the per-tick check doesn't re-attempt a
                    // failing fetch every second — back off until the next TTL window.
                    safeLocalSet(key, JSON.stringify({ ts: Date.now(), data: obj.data }));
                    return obj.data;
                } catch (_) { /* none */ }
            }
            // No stale entry to serve: NEGATIVE-CACHE the failure (data:null) with a
            // fresh timestamp so the TTL guard above suppresses the next attempt for
            // the back-off window. Without this, every status-bar render (≈4×/s on
            // each WS push) re-runs the failing fetch — an error storm in the console
            // whenever the device is offline. A null cache hit returns "no update".
            safeLocalSet(key, JSON.stringify({ ts: Date.now(), data: null }));
            return null;
        } finally {
            delete inFlightFetches[key];                     // clear once settled, ok or not
        }
    })();
    inFlightFetches[key] = p;
    return p;
}

// Read the device's running version + firmware-variant key off the FirmwareUpdateModule.
function deviceFirmwareInfo() {
    if (!state || !state.modules) return null;
    const fw = findModule("Firmware") || (state.modules.find(m => m.type === "FirmwareUpdateModule"));
    if (!fw) return null;
    const ctrls = fw.controls || [];
    const version = (ctrls.find(c => c.name === "version") || {}).value;
    const firmware = (ctrls.find(c => c.name === "firmware") || {}).value;
    return version ? { version, firmware } : null;
}

// Light the badge for an available update. `tag` is the release the picker should pre-select
// (a vX.Y.Z stable tag, or "latest"); `label` is what the badge shows.
function showUpdateBadge(badge, tag, label) {
    badge.textContent = `⬆ ${label}`;
    badge.title = `Firmware update available: ${label} — open Firmware to install`;
    badge.dataset.tag = tag;
    badge.hidden = false;
}

// Is there a newer STABLE release than the device's version, with a compatible .bin?
// Returns the stable tag (e.g. "v2.1.0") or null. /latest excludes prereleases.
async function stableUpdate(dev, force) {
    const rel = await cachedJson(`${RELEASES_API}/latest`, "projectMM.update.latest.v1", force);
    if (!rel || !rel.tag_name) return null;
    const assetNames = (rel.assets || []).map(a => a.name);
    const hasBinary = !dev.firmware ||
        assetNames.some(n => n === `firmware-${dev.firmware}-${rel.tag_name}.bin`);
    return (isNewer(rel.tag_name, dev.version) && hasBinary) ? rel.tag_name : null;
}

// For a device already on a -dev build: is the moving `latest` release newer? Returns its
// version string (e.g. "2.1.0-dev.7") or null. The latest release's tag is "latest"; its
// version is published as the release `name` (release.yml), which the GitHub API exposes
// CORS-readably — unlike the manifest-*.json asset, whose release-asset URL redirects to
// release-assets.githubusercontent.com (no CORS header), so the device-hosted UI can't read it.
// We also require the matching firmware .bin asset so the badge never points at a build the
// device can't install.
async function devUpdate(dev, force) {
    if (!dev.firmware) return null;                          // can't match an asset without the key
    const rel = await cachedJson(`${RELEASES_API}/tags/latest`, "projectMM.update.dev.v1", force);
    const v = rel && rel.name;
    if (!v) return null;
    // Assets are versioned, not tagged: the `latest` release ships
    // firmware-<fw>-v<version>.bin (release.yml stages PREFIX="firmware-...-v$V").
    const hasBinary = (rel.assets || []).some(a => a.name === `firmware-${dev.firmware}-v${v}.bin`);
    return (hasBinary && isNewer(v, dev.version)) ? v : null;
}

// Show/hide the badge. `force` bypasses the cache (used when the Firmware card opens).
// Stable update takes precedence; a -dev device additionally checks the latest channel.
async function checkFirmwareUpdate(force) {
    const badge = document.getElementById("fw-update-badge");
    if (!badge) return;
    const dev = deviceFirmwareInfo();
    if (!dev) { badge.hidden = true; return; }

    const stableTag = await stableUpdate(dev, force);
    if (stableTag) { showUpdateBadge(badge, stableTag, stableTag); return; }

    // Only a prerelease (-dev…) build follows the moving latest channel; a stable device is
    // not nudged toward an unreleased build.
    const onPrerelease = (parse(dev.version)?.prerelease.length || 0) > 0;
    if (onPrerelease) {
        const devVer = await devUpdate(dev, force);
        if (devVer) { showUpdateBadge(badge, "latest", `latest (${devVer})`); return; }
    }
    badge.hidden = true;
}

// Badge click → pre-select the new release in the picker (it restores from PICKER_RELEASE_KEY
// on init) and open the Firmware card, so the user lands one click from Install.
function setupUpdateBadge() {
    const badge = document.getElementById("fw-update-badge");
    if (!badge) return;
    badge.addEventListener("click", () => {
        if (badge.dataset.tag) safeLocalSet(PICKER_RELEASE_KEY, badge.dataset.tag);
        selectModule("Firmware");
    });
}

// ---------------------------------------------------------------------------
// 9. Boot
// ---------------------------------------------------------------------------

document.addEventListener("DOMContentLoaded", init);
