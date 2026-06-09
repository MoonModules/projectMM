// projectMM Web UI — all logic in one hand-maintained file per CLAUDE.md.
// Loaded as <script type="module"> so it can import the shared install-picker
// component used by both the device UI (here, OTA flash) and the GitHub Pages
// installer (first flash via Web Serial). Module loading is deferred by
// default; entry-point is the WS init at the bottom — no ordering surprises.
import { installPicker } from "/install-picker.js";
import { preview } from "/preview3d.js";

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
    try {
        const resp = await fetch("/api/state");
        state = await resp.json();
        // Pending-board handoff from the web installer. The installer page
        // (docs/install/devices.js → Inject button) opens us with
        // `?board=<name>` when the orchestrator couldn't push the board
        // itself (HTTPS Pages → HTTP device blocked by mixed-content, OR
        // an Improv-less firmware variant). We consume it once, fetch the
        // boards.json entry from Pages, fan out every `controls.*` field
        // via the standard `/api/control` write, then strip the param via
        // history.replaceState. Fire-and-forget — the WS state push driven
        // by sendControl re-renders the affected fields.
        consumePendingBoardParam();
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
    preview.setupShrink();
}

async function sendControl(moduleName, controlName, value) {
    // Best-effort by design — failures are not retried here (see
    // consumePendingBoardParam's "No retry" contract). The query param is
    // single-shot. Non-ok responses + network errors are logged to console
    // so a user with devtools open can see what went wrong (e.g. a board-
    // injected control value that the device-side validator rejected).
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

// Public Pages URL of the board catalog. The installer page also serves
// this at `./boards.json` (same-origin from the installer's perspective),
// but the device UI is on a different origin (the device's HTTP server),
// so we fetch the canonical Pages copy. CORS: GitHub Pages static assets
// ship `Access-Control-Allow-Origin: *`, so a cross-origin fetch from
// http://<device>/ to https://ewowi.github.io succeeds without proxying.
// Hardcoded rather than configurable: the catalog is project-global, not
// per-installation. During local development, flip this to
// `http://localhost:8000/boards.json` and re-flash; preview_installer.py
// sends the same `Access-Control-Allow-Origin: *` header production does.
const BOARDS_JSON_URL = "https://ewowi.github.io/projectMM/install/boards.json";

// Consume an installer-emitted `?board=<name>` query param: look the name
// up in boards.json on Pages, then push every field under `controls.<mod>.<ctrl>`
// via the standard `/api/control` channel so each module's validation runs.
// Strip the query param BEFORE the network round-trip so a mid-fetch
// refresh doesn't double-push. No retry — failures (network, rejection)
// leave the device unchanged; user re-runs the Inject button or sets the
// fields manually via MoonDeck.
//
// See docs/moonmodules/core/BoardModule.md (Injection paths) for the
// boards.json schema and the full handoff sequence.
async function consumePendingBoardParam() {
    const params = new URLSearchParams(location.search);
    const pendingBoard = params.get("board");
    if (!pendingBoard) return;
    // Strip the param up-front so a refresh during the async fetch doesn't
    // re-trigger the injection.
    params.delete("board");
    const qs = params.toString();
    history.replaceState({}, "",
        location.pathname + (qs ? "?" + qs : "") + location.hash);

    let entry;
    try {
        const res = await fetch(BOARDS_JSON_URL);
        if (!res.ok) throw new Error(`boards.json HTTP ${res.status}`);
        const catalog = await res.json();
        entry = Array.isArray(catalog)
            ? catalog.find(b => b && b.name === pendingBoard)
            : null;
    } catch (e) {
        // Network down, Pages outage, or boards.json missing the entry.
        // Surface in the console so a user with devtools open can see why
        // injection didn't take; don't show a modal since the rest of the
        // UI is operational and most users just want to use the device.
        console.warn("[board-inject] failed to fetch boards.json:", e);
        return;
    }
    if (!entry || !entry.controls) {
        console.warn(`[board-inject] no controls for board "${pendingBoard}"`);
        return;
    }
    // entry.controls shape: { "<ModuleName>": { "<controlName>": <value>, ... }, ... }
    // e.g. { "Board": { "board": "Olimex ESP32-Gateway Rev G" } }
    // Iterate sequentially so each FilesystemModule debounced-save sees the
    // full set; parallel fires could race the dirty flag on the same module.
    for (const [moduleName, controls] of Object.entries(entry.controls)) {
        if (!controls || typeof controls !== "object") continue;
        for (const [controlName, value] of Object.entries(controls)) {
            await sendControl(moduleName, controlName, value);
        }
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
    // Modules whose primary purpose is hosting user-added children (Layers,
    // Layer, Drivers, Layouts) collapse their own controls so the children
    // are the focus by default. Modules that merely host a code-wired child
    // (Network → Improv) keep their controls expanded — the parent's settings
    // are the main point, the code-wired child is informational. Leaf modules
    // render controls inline (no wrapper at all).
    const hasVisibleControls = mod.controls && mod.controls.some(c => !c.hidden);
    const wrapInDetails = acceptsNewChildren(mod) && hasVisibleControls;
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
        const ownFirmwareKey = (() => {
            if (!state || !state.modules) return null;
            // Look up by stable type first; the name fallback is a
            // belt-and-braces safety net (mod.name is user-editable in
            // principle, so it's not load-bearing for this lookup).
            const sys = state.modules.find(m => m.type === "SystemModule")
                     || state.modules.find(m => m.name === "System");
            const fwCtrl = sys && (sys.controls || []).find(c => c.name === "firmware");
            return fwCtrl && fwCtrl.value ? fwCtrl.value : null;
        })();
        const mount = document.createElement("div");
        mount.className = "install-picker-host";
        controlsHost.appendChild(mount);
        installPicker.init({
            container: mount,
            ownFirmwareKey,
            // Device already knows its board (BoardModule) — picker is for
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
// hardcoding role names. Code-wired children (ImprovProvisioning, BoardModule
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
            const input = document.createElement("input");
            input.type = "number";
            input.value = ctrl.value ?? 0;
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            input.addEventListener("input", () => {
                dragTs[key] = Date.now();
                debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, parseInt(input.value)));
            });
            row.appendChild(input);
            appendResetButton(row, moduleName, ctrl, def, () => { input.value = def; });
            break;
        }
        case "int16": {
            // ctrl.min/ctrl.max are always present (server sends them). Sentinel
            // values INT16_MIN (-32768) / INT16_MAX (32767) mean "unbounded" —
            // fall back to the percentage range used by Layer start/end controls.
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
            sel.addEventListener("change", async () => {
                dragTs[key] = Date.now();
                await sendControl(moduleName, ctrl.name, parseInt(sel.value));
                // Server may rebuild this module's controls (dynamic onBuildControls); refetch
                setTimeout(refetchState, 200);
            });
            row.appendChild(sel);
            appendResetButton(row, moduleName, ctrl, def, () => { sel.value = def; });
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
            input.size = 15;
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
        default:
            // Unknown control type — skip silently. New types may be added engine-side
            // without breaking the UI; they just don't render until handled here.
            return null;
    }

    return row;
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

function updateModuleControls(mod) {
    if (!mod.controls) return;
    for (const ctrl of mod.controls) {
        const mid = cssEscape(mod.name);
        const k = cssEscape(ctrl.name);
        const dragKey = mod.name + ":" + ctrl.name;
        const ts = dragTs[dragKey] || 0;
        const userActive = Date.now() - ts < 1000;

        switch (ctrl.type) {
            case "uint8":
            case "uint16":
            case "int16": {
                if (userActive) break;
                const input = document.querySelector(`input[data-mid="${mid}"][data-key="${k}"]`);
                if (input && Number(input.value) !== Number(ctrl.value)) {
                    input.value = ctrl.value ?? 0;
                    const val = input.nextElementSibling;
                    if (val && val.classList.contains("control-value-input")) val.value = ctrl.value ?? 0;
                }
                break;
            }
            case "bool": {
                if (userActive) break;
                const input = document.querySelector(`input[data-mid="${mid}"][data-key="${k}"]`);
                if (input && input.checked !== !!ctrl.value) input.checked = !!ctrl.value;
                break;
            }
            case "text": {
                if (userActive) break;
                const input = document.querySelector(`input[type="text"][data-mid="${mid}"][data-key="${k}"]`);
                if (input && input.value !== (ctrl.value ?? "")) input.value = ctrl.value ?? "";
                break;
            }
            case "password": {
                if (userActive) break;
                // The peek button flips the input to type="text", so match either.
                const input = document.querySelector(`input[data-mid="${mid}"][data-key="${k}"]`);
                const decoded = decodePassword(ctrl.value);
                if (input && input.value !== decoded) input.value = decoded;
                break;
            }
            case "select": {
                if (userActive) break;
                const sel = document.querySelector(`select[data-mid="${mid}"][data-key="${k}"]`);
                if (sel && Number(sel.value) !== Number(ctrl.value)) sel.value = ctrl.value;
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
                const input = document.querySelector(`input.ipv4-input[data-mid="${mid}"][data-key="${k}"]`);
                // Don't clobber an input the user is currently editing —
                // matches how text inputs handle WS pushes elsewhere.
                if (input && document.activeElement !== input) {
                    input.value = ctrl.value ?? "";
                }
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
    if (ctrl.type === "ipv4" || ctrl.type === "text" || ctrl.type === "password") {
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

// Replace mode: filter to the target module's own role (effect ↔ effect).
function openReplacePicker(targetMod, anchorEl) {
    openPicker(anchorEl, {
        roles: [targetMod.role],
        actionLabel: "replace",
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
        matches.forEach((t, i) => {
            const item = document.createElement("div");
            item.className = "type-picker-item" + (i === 0 ? " selected" : "");
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
        selectedType = matches.length > 0 ? matches[0].name : null;
        createBtn.disabled = !selectedType;
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
}

// ---------------------------------------------------------------------------
// 9. Boot
// ---------------------------------------------------------------------------

document.addEventListener("DOMContentLoaded", init);
