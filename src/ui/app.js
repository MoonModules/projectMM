// projectMM Web UI — all logic in one hand-maintained file per CLAUDE.md.
//
// Sections (top to bottom):
//   1. State + storage
//   2. WebSocket (with keepalive, visibility pause, bfcache, exponential backoff)
//   3. REST helpers + module mutations
//   4. Render pipeline: render() → renderNav() → renderCards() → createCard() → createControl()
//   5. State patching (no-rebuild contract): updateValues() + updateModuleControls()
//   6. Type picker
//   7. Drag-to-reorder (HTML5 DnD; desktop only — mobile uses ↑/↓ buttons)
//   8. 3D WebGL preview (sticky + scroll-shrink, sparse vertex buffer, frame cache)
//   9. Status bar wiring (device name, sys stats, theme, reboot)
//  10. Boot
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
            renderPreviewFrame(e.data);
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
        const savedSel = lsRead(LS_SELECTED, "mm.selectedModule", null);
        if (state.modules && state.modules.length > 0) {
            const exists = savedSel && state.modules.some(m => m.name === savedSel);
            selectModule(exists ? savedSel : state.modules[0].name);
        }
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
    initWebGL();
    setupPreviewShrink();
}

async function sendControl(moduleName, controlName, value) {
    try {
        await fetch("/api/control", {
            method: "POST",
            headers: {"Content-Type": "application/json"},
            body: JSON.stringify({module: moduleName, control: controlName, value: value})
        });
    } catch { /* server may be busy */ }
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

// move to absolute index (0..siblings.length-1). Called from up/down buttons and drag-drop.
async function moveModuleTo(name, toIndex) {
    await fetch("/api/modules/" + encodeURIComponent(name) + "/move", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify({to: toIndex})
    });
    refetchState();
}

async function rebootDevice() {
    if (!confirm("Reboot device?")) return;
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
    // Side nav deferred to 1.x; main column shows the selected module + children
}

function selectModule(name) {
    selectedModule = name;
    localStorage.setItem(LS_SELECTED, name);
    renderCards();
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

    // Plan-11: render all top-level modules in one column. The "selected module"
    // concept survives for localStorage purposes but the UI shows everything.
    for (const mod of state.modules) {
        renderModuleTree(mod, main, 0);
    }
}

function renderModuleTree(mod, parentEl, depth) {
    parentEl.appendChild(createCard(mod, depth));
    if (mod.children && mod.children.length > 0) {
        for (const child of mod.children) {
            renderModuleTree(child, parentEl, depth + 1);
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

    const enabled = document.createElement("input");
    enabled.type = "checkbox";
    enabled.className = "module-enabled";
    enabled.checked = (mod.enabled === undefined) ? true : !!mod.enabled;
    enabled.dataset.mid = mod.name;
    enabled.dataset.key = "enabled";
    enabled.addEventListener("change", () => {
        sendControl(mod.name, "enabled", enabled.checked);
    });
    title.appendChild(enabled);

    const name = document.createElement("span");
    name.className = "card-name";
    name.textContent = mod.name;
    title.appendChild(name);

    // fps/ms toggle on the stats line — global mode, single click cycles all cards
    const stats = document.createElement("span");
    stats.className = "card-stats";
    stats.dataset.mid = mod.name;
    stats.dataset.key = "stats";
    stats.title = "Click to toggle fps/ms";
    stats.textContent = formatStats(mod);
    stats.addEventListener("click", () => {
        const idx = TIMING_MODES.indexOf(timingMode);
        timingMode = TIMING_MODES[(idx + 1) % TIMING_MODES.length];
        localStorage.setItem(LS_TIMING, timingMode);
        // Refresh every card's stats line in place — no full re-render needed
        document.querySelectorAll(".card-stats[data-mid]").forEach(s => {
            const m = findModule(s.dataset.mid);
            if (m) s.textContent = formatStats(m);
        });
    });
    title.appendChild(stats);

    // Action buttons for reorderable children (Effect/Modifier roles).
    // Top-level modules aren't reorderable in this iteration (fixed in main.cpp).
    if (depth > 0 && (mod.role === "effect" || mod.role === "modifier")) {
        const actions = createActionButtons(mod);
        title.appendChild(actions);
    }
    card.appendChild(title);

    // -- Controls --
    if (mod.controls) {
        for (const ctrl of mod.controls) {
            if (ctrl.hidden) continue;  // plan-10 hidden flag (still respected)
            const row = createControl(mod.name, mod.type, ctrl);
            if (row) card.appendChild(row);
        }
    }

    // -- Footer: + add child (only on parents that accept children) --
    if (acceptsChildren(mod)) {
        const footer = document.createElement("div");
        footer.className = "card-footer";
        const addBtn = document.createElement("button");
        addBtn.className = "add-btn";
        addBtn.textContent = "+ add child";
        addBtn.addEventListener("click", () => openTypePicker(mod, footer));
        footer.appendChild(addBtn);
        card.appendChild(footer);
    }

    // -- Drag-to-reorder (desktop HTML5; mobile naturally falls through to ↑/↓) --
    if (depth > 0 && (mod.role === "effect" || mod.role === "modifier")) {
        attachDragHandlers(card, mod);
    }

    return card;
}

function formatStats(mod) {
    const us = (mod.loopTimeUs !== undefined) ? mod.loopTimeUs : 0;
    if (us === 0) return "—";
    if (timingMode === "fps") {
        const fps = us > 0 ? Math.round(1_000_000 / us) : 0;
        return fps >= 1000 ? (fps / 1000).toFixed(1) + "K fps" : fps + " fps";
    }
    return us < 1000 ? us + " µs" : (us / 1000).toFixed(2) + " ms";
}

function createActionButtons(mod) {
    const wrap = document.createElement("span");
    wrap.className = "card-actions";

    // Find current index within parent's reorderable siblings to enable/disable buttons.
    // Identify by name, not object identity — WS state pushes replace `state` and the
    // `mod` captured at render time becomes stale within ~1s, breaking indexOf.
    const parent = findParent(mod.name);
    const sibs = parent ? (parent.children || []).filter(c => c.role === mod.role) : [];
    const idx = sibs.findIndex(c => c.name === mod.name);
    const absIdx = parent ? (parent.children || []).findIndex(c => c.name === mod.name) : -1;

    const upBtn = document.createElement("button");
    upBtn.className = "card-btn";
    upBtn.textContent = "↑";
    upBtn.title = "Move up";
    upBtn.disabled = (idx <= 0);
    upBtn.addEventListener("click", () => moveModuleTo(mod.name, Math.max(0, absIdx - 1)));
    wrap.appendChild(upBtn);

    const downBtn = document.createElement("button");
    downBtn.className = "card-btn";
    downBtn.textContent = "↓";
    downBtn.title = "Move down";
    downBtn.disabled = (idx < 0 || idx >= sibs.length - 1);
    downBtn.addEventListener("click", () => {
        const total = parent ? (parent.children || []).length : 0;
        moveModuleTo(mod.name, Math.min(total - 1, absIdx + 1));
    });
    wrap.appendChild(downBtn);

    const delBtn = document.createElement("button");
    delBtn.className = "card-btn card-btn-del";
    delBtn.textContent = "×";
    delBtn.title = "Delete";
    delBtn.addEventListener("click", () => {
        if (confirm(`Delete ${mod.name}?`)) deleteModule(mod.name);
    });
    wrap.appendChild(delBtn);

    // Drag handle (HTML5 drag-source; the .card itself is the draggable element)
    const handle = document.createElement("span");
    handle.className = "drag-handle";
    handle.textContent = "☰";
    handle.title = "Drag to reorder";
    wrap.appendChild(handle);

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

function acceptsChildren(mod) {
    // role-based: Layer → effect+modifier, DriverGroup → driver, LayoutGroup → layout.
    // Mapped in JS, not in engine, so no backend allowedChildRoles field needed.
    // Keyed on mod.type (stable factory key) — mod.name is editable per instance.
    return mod.type === "Layer" || mod.type === "DriverGroup" || mod.type === "LayoutGroup";
}

function rolesAcceptedBy(parentMod) {
    if (parentMod.type === "Layer")       return ["effect", "modifier"];
    if (parentMod.type === "DriverGroup") return ["driver"];
    if (parentMod.type === "LayoutGroup") return ["layout"];
    return [];
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
            const valSpan = document.createElement("span");
            valSpan.className = "control-value";
            valSpan.textContent = input.value;
            input.addEventListener("input", () => {
                dragTs[key] = Date.now();
                valSpan.textContent = input.value;
                debounceSend(key, 150, () => sendControl(moduleName, ctrl.name, parseInt(input.value)));
            });
            row.appendChild(input);
            row.appendChild(valSpan);
            appendResetButton(row, moduleName, ctrl, def, () => {
                input.value = def;
                valSpan.textContent = def;
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
        case "bool": {
            const input = document.createElement("input");
            input.type = "checkbox";
            input.checked = !!ctrl.value;
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            input.addEventListener("change", () => {
                dragTs[key] = Date.now();
                sendControl(moduleName, ctrl.name, input.checked);
            });
            row.appendChild(input);
            appendResetButton(row, moduleName, ctrl, def, () => { input.checked = !!def; });
            break;
        }
        case "text": {
            const input = document.createElement("input");
            input.type = "text";
            input.value = ctrl.value ?? "";
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            input.addEventListener("input", () => {
                dragTs[key] = Date.now();
                debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, input.value));
            });
            row.appendChild(input);
            break;
        }
        case "password": {
            const input = document.createElement("input");
            input.type = "password";
            input.placeholder = ctrl.value ? "•".repeat(8) : "";
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            input.addEventListener("input", () => {
                dragTs[key] = Date.now();
                debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, input.value));
            });
            row.appendChild(input);
            // Hold-to-peek button
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
    const eq = (ctrl.type === "bool") ? (!!ctrl.value === !!def)
                                       : (Number(ctrl.value) === Number(def));
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
        if (statsEl) statsEl.textContent = formatStats(mod);
        // refresh enabled checkbox
        const enabledEl = document.querySelector(`input.module-enabled[data-mid="${cssEscape(mod.name)}"]`);
        if (enabledEl) {
            const ts = dragTs[mod.name + ":enabled"] || 0;
            if (Date.now() - ts > 1000) {
                enabledEl.checked = (mod.enabled === undefined) ? true : !!mod.enabled;
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
            case "uint16": {
                if (userActive) break;
                const input = document.querySelector(`input[data-mid="${mid}"][data-key="${k}"]`);
                if (input && Number(input.value) !== Number(ctrl.value)) {
                    input.value = ctrl.value ?? 0;
                    const val = input.nextElementSibling;
                    if (val && val.classList.contains("control-value")) val.textContent = ctrl.value;
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
                const eq = (ctrl.type === "bool") ? (!!ctrl.value === !!def)
                                                   : (Number(ctrl.value) === Number(def));
                btn.classList.toggle("active", !eq);
            }
        }
    }
}

function cssEscape(s) {
    // Minimal CSS attribute selector escape. Module/control names are alphanumeric
    // in practice, so this is defensive.
    return String(s).replace(/(["\\])/g, "\\$1");
}

// ---------------------------------------------------------------------------
// 6. Type picker
// ---------------------------------------------------------------------------

function openTypePicker(parentMod, anchorEl) {
    // Close any existing picker
    document.querySelectorAll(".type-picker").forEach(p => p.remove());

    const accepted = rolesAcceptedBy(parentMod);
    const filtered = availableTypes.filter(t => accepted.includes(t.role));

    const picker = document.createElement("div");
    picker.className = "type-picker";

    const search = document.createElement("input");
    search.type = "text";
    search.placeholder = "search…";
    search.className = "type-picker-search";
    picker.appendChild(search);

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
    createBtn.textContent = "create";
    createBtn.disabled = true;
    actions.appendChild(cancelBtn);
    actions.appendChild(createBtn);
    picker.appendChild(actions);

    let selectedType = null;

    function refresh() {
        const q = search.value.toLowerCase();
        const matches = filtered.filter(t => !q || t.name.toLowerCase().includes(q));
        list.innerHTML = "";
        matches.forEach((t, i) => {
            const item = document.createElement("div");
            item.className = "type-picker-item" + (i === 0 ? " selected" : "");
            item.textContent = t.name;
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
                addModule(t.name, parentMod.name);
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
                addModule(selectedType, parentMod.name);
                picker.remove();
            }
        } else if (e.key === "Escape") {
            picker.remove();
        }
    });

    function filteredAt(i) {
        const q = search.value.toLowerCase();
        return filtered.filter(t => !q || t.name.toLowerCase().includes(q))[i];
    }

    createBtn.addEventListener("click", () => {
        if (selectedType) {
            addModule(selectedType, parentMod.name);
            picker.remove();
        }
    });

    anchorEl.appendChild(picker);
    refresh();
    search.focus();
}

// ---------------------------------------------------------------------------
// 7. Drag-to-reorder (desktop HTML5; mobile falls through to ↑/↓ buttons)
// ---------------------------------------------------------------------------

function attachDragHandlers(card, mod) {
    card.draggable = true;

    card.addEventListener("dragstart", (e) => {
        e.dataTransfer.effectAllowed = "move";
        e.dataTransfer.setData("text/plain", mod.name);
        card.classList.add("dragging");
    });
    card.addEventListener("dragend", () => {
        card.classList.remove("dragging");
        document.querySelectorAll(".drag-over").forEach(c => c.classList.remove("drag-over"));
    });
    card.addEventListener("dragover", (e) => {
        // Only allow drop on a sibling of the same role (= same card depth)
        const src = document.querySelector(".card.dragging");
        if (!src || src === card) return;
        if (src.dataset.depth === card.dataset.depth) {
            e.preventDefault();
            card.classList.add("drag-over");
        }
    });
    card.addEventListener("dragleave", () => {
        card.classList.remove("drag-over");
    });
    card.addEventListener("drop", (e) => {
        e.preventDefault();
        card.classList.remove("drag-over");
        const srcName = e.dataTransfer.getData("text/plain");
        if (!srcName || srcName === mod.name) return;
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
// 8. 3D WebGL preview
// ---------------------------------------------------------------------------

let gl = null;
let glProgram = null;
let glBuffer = null;
let camTheta = 0.5, camPhi = 0.4, camDist = 2.5;
let lastVerts = null;        // cached vertex array for orbit-without-server-frame
let lastVertCount = 0;
let lastMaxDim = 1;

function initWebGL() {
    const canvas = document.getElementById("preview");
    if (!canvas) return;
    gl = canvas.getContext("webgl");
    if (!gl) return;

    const vsrc = `
        attribute vec3 aPos;
        attribute vec3 aCol;
        varying vec3 vCol;
        uniform mat4 uMVP;
        uniform float uPointSize;
        void main() {
            vCol = aCol;
            gl_Position = uMVP * vec4(aPos, 1.0);
            // Depth-corrected point size — closer LEDs render larger
            gl_PointSize = uPointSize / gl_Position.w;
        }
    `;
    const fsrc = `
        precision mediump float;
        varying vec3 vCol;
        void main() {
            float d = length(gl_PointCoord - vec2(0.5));
            if (d > 0.25) discard;
            // Soft circular falloff
            float a = 1.0 - smoothstep(0.10, 0.25, d);
            gl_FragColor = vec4(vCol * a, a);
        }
    `;

    const vs = gl.createShader(gl.VERTEX_SHADER);
    gl.shaderSource(vs, vsrc); gl.compileShader(vs);
    const fs = gl.createShader(gl.FRAGMENT_SHADER);
    gl.shaderSource(fs, fsrc); gl.compileShader(fs);
    glProgram = gl.createProgram();
    gl.attachShader(glProgram, vs); gl.attachShader(glProgram, fs);
    gl.linkProgram(glProgram); gl.useProgram(glProgram);

    glBuffer = gl.createBuffer();
    gl.enable(gl.DEPTH_TEST);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);

    // Orbit controls (mouse + touch)
    let dragging = false, lastX = 0, lastY = 0;
    canvas.addEventListener("mousedown", (e) => { dragging = true; lastX = e.clientX; lastY = e.clientY; });
    canvas.addEventListener("mousemove", (e) => {
        if (!dragging) return;
        camTheta += (e.clientX - lastX) * 0.01;
        camPhi = Math.max(-1.5, Math.min(1.5, camPhi + (e.clientY - lastY) * 0.01));
        lastX = e.clientX; lastY = e.clientY;
        redrawCached();
    });
    canvas.addEventListener("mouseup",    () => { dragging = false; });
    canvas.addEventListener("mouseleave", () => { dragging = false; });
    canvas.addEventListener("wheel", (e) => {
        camDist = Math.max(0.5, Math.min(10, camDist + e.deltaY * 0.005));
        e.preventDefault();
        redrawCached();
    }, {passive: false});

    // Touch orbit
    canvas.addEventListener("touchstart", (e) => {
        if (e.touches.length !== 1) return;
        dragging = true;
        lastX = e.touches[0].clientX; lastY = e.touches[0].clientY;
    });
    canvas.addEventListener("touchmove", (e) => {
        if (!dragging || e.touches.length !== 1) return;
        const t = e.touches[0];
        camTheta += (t.clientX - lastX) * 0.01;
        camPhi = Math.max(-1.5, Math.min(1.5, camPhi + (t.clientY - lastY) * 0.01));
        lastX = t.clientX; lastY = t.clientY;
        redrawCached();
        e.preventDefault();
    }, {passive: false});
    canvas.addEventListener("touchend", () => { dragging = false; });
}

// Read the Preview module's "decompress" control from the latest state push.
function previewDecompressOn() {
    if (!state || !state.modules) return false;
    let found = false;
    (function walk(mods) {
        for (const m of mods) {
            if (m.type === "PreviewDriver" || m.name === "Preview") {
                const c = (m.controls || []).find(c => c.name === "decompress");
                if (c) found = !!c.value;
            }
            if (m.children) walk(m.children);
        }
    })(state.modules);
    return found;
}

function renderPreviewFrame(buf) {
    if (!gl) initWebGL();
    if (!gl) return;

    // 13-byte header: [0x02][dw16][dh16][dd16][ow16][oh16][od16], little-endian.
    // dw/dh/dd = dimensions of the data in this frame; ow/oh/od = original grid.
    if (buf.byteLength < 13) return;
    const view = new DataView(buf);
    if (view.getUint8(0) !== 0x02) return;
    const dw = view.getUint16(1, true);
    const dh = view.getUint16(3, true);
    const dd = view.getUint16(5, true);
    const ow = view.getUint16(7, true);
    const oh = view.getUint16(9, true);
    const od = view.getUint16(11, true);
    if (dw === 0 || dh === 0 || dd === 0) return;
    const total = dw * dh * dd;
    if (buf.byteLength < 13 + total * 3) return;
    const pixels = new Uint8Array(buf, 13);

    // Decompress: when on, reconstruct the original grid by block-replicating
    // each received voxel across its stride×stride×stride original cells, so the
    // preview shows the same voxel count as the real layout. When off, render the
    // downsampled cloud directly.
    const decompress = previewDecompressOn()
        && ow >= dw && oh >= dh && od >= dd
        && (ow > dw || oh > dh || od > dd);
    const w = decompress ? ow : dw;
    const h = decompress ? oh : dh;
    const d = decompress ? od : dd;

    const colorAt = decompress
        // map original cell (ix,iy,iz) → nearest downsampled voxel
        ? (ix, iy, iz) => {
              const sx = Math.min(dw - 1, (ix * dw / ow) | 0);
              const sy = Math.min(dh - 1, (iy * dh / oh) | 0);
              const sz = Math.min(dd - 1, (iz * dd / od) | 0);
              return (sz * dh * dw + sy * dw + sx) * 3;
          }
        : (ix, iy, iz) => (iz * dh * dw + iy * dw + ix) * 3;

    // Sparse vertex buffer — skip black voxels. Halves GPU work for typical effects
    // and avoids drawing invisible points underneath the lit ones.
    let nonBlack = 0;
    for (let iz = 0; iz < d; iz++) {
        for (let iy = 0; iy < h; iy++) {
            for (let ix = 0; ix < w; ix++) {
                const o = colorAt(ix, iy, iz);
                if (pixels[o] | pixels[o + 1] | pixels[o + 2]) nonBlack++;
            }
        }
    }
    const maxDim = Math.max(w, h, d);
    const verts = new Float32Array(nonBlack * 6);
    let vi = 0;
    for (let iz = 0; iz < d; iz++) {
        for (let iy = 0; iy < h; iy++) {
            for (let ix = 0; ix < w; ix++) {
                const idx = colorAt(ix, iy, iz);
                const r = pixels[idx], g = pixels[idx + 1], b = pixels[idx + 2];
                if (!(r | g | b)) continue;
                verts[vi++] = (ix / maxDim) - 0.5 * w / maxDim;
                verts[vi++] = (iy / maxDim) - 0.5 * h / maxDim;
                verts[vi++] = (iz / maxDim) - 0.5 * d / maxDim;
                verts[vi++] = r / 255;
                verts[vi++] = g / 255;
                verts[vi++] = b / 255;
            }
        }
    }

    lastVerts = verts;
    lastVertCount = nonBlack;
    lastMaxDim = maxDim;
    drawVerts();
}

function redrawCached() {
    if (lastVerts) drawVerts();
}

function drawVerts() {
    if (!gl || !lastVerts) return;
    const canvas = document.getElementById("preview");
    canvas.width = canvas.clientWidth;
    canvas.height = canvas.clientHeight;
    gl.viewport(0, 0, canvas.width, canvas.height);

    const cx = camDist * Math.cos(camPhi) * Math.sin(camTheta);
    const cy = camDist * Math.sin(camPhi);
    const cz = camDist * Math.cos(camPhi) * Math.cos(camTheta);
    const mvp = buildMVP(cx, cy, cz, canvas.width / Math.max(1, canvas.height));

    gl.clearColor(0.05, 0.07, 0.09, 1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

    gl.bindBuffer(gl.ARRAY_BUFFER, glBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, lastVerts, gl.DYNAMIC_DRAW);

    const aPos = gl.getAttribLocation(glProgram, "aPos");
    const aCol = gl.getAttribLocation(glProgram, "aCol");
    gl.enableVertexAttribArray(aPos);
    gl.enableVertexAttribArray(aCol);
    gl.vertexAttribPointer(aPos, 3, gl.FLOAT, false, 24, 0);
    gl.vertexAttribPointer(aCol, 3, gl.FLOAT, false, 24, 12);

    gl.uniformMatrix4fv(gl.getUniformLocation(glProgram, "uMVP"), false, mvp);
    const pointSize = Math.max(2, canvas.width * 0.5 / lastMaxDim);
    gl.uniform1f(gl.getUniformLocation(glProgram, "uPointSize"), pointSize);

    gl.drawArrays(gl.POINTS, 0, lastVertCount);
}

function buildMVP(ex, ey, ez, aspect) {
    const fLen = Math.sqrt(ex*ex + ey*ey + ez*ez) || 1;
    const fx = -ex/fLen, fy = -ey/fLen, fz = -ez/fLen;
    // Right = cross(forward, (0,1,0))
    let rx = fz, ry = 0, rz = -fx;
    const rLen = Math.sqrt(rx*rx + ry*ry + rz*rz) || 1;
    rx /= rLen; ry /= rLen; rz /= rLen;
    // Up = cross(right, forward)
    const ux = ry*fz - rz*fy, uy = rz*fx - rx*fz, uz = rx*fy - ry*fx;

    const view = [
        rx, ux, -fx, 0,
        ry, uy, -fy, 0,
        rz, uz, -fz, 0,
        -(rx*ex+ry*ey+rz*ez), -(ux*ex+uy*ey+uz*ez), (fx*ex+fy*ey+fz*ez), 1
    ];

    const near = 0.1, far = 50, fov = 0.8;
    const f = 1 / Math.tan(fov / 2);
    const proj = [
        f/aspect, 0, 0, 0,
        0, f, 0, 0,
        0, 0, (far+near)/(near-far), -1,
        0, 0, 2*far*near/(near-far), 0
    ];

    const m = new Float32Array(16);
    for (let i = 0; i < 4; i++) {
        for (let j = 0; j < 4; j++) {
            m[j*4+i] = 0;
            for (let k = 0; k < 4; k++) {
                m[j*4+i] += proj[k*4+i] * view[j*4+k];
            }
        }
    }
    return m;
}

// Scroll-shrink preview: 0..1 ratio over 0..300px of main scroll.
function setupPreviewShrink() {
    const canvas = document.getElementById("preview");
    if (!canvas) return;
    let naturalHeight = null;
    let ticking = false;
    const SHRINK_OVER = 300;
    function apply() {
        ticking = false;
        if (naturalHeight === null) {
            naturalHeight = canvas.clientHeight || (window.innerHeight * 0.5);
        }
        const r = Math.min(1, Math.max(0, window.scrollY / SHRINK_OVER));
        const h = naturalHeight * (1 - r * 0.5);
        canvas.style.height = Math.round(h) + "px";
        canvas.style.maxHeight = Math.round(h) + "px";
        if (lastVerts) redrawCached();
    }
    window.addEventListener("scroll", () => {
        if (!ticking) {
            requestAnimationFrame(apply);
            ticking = true;
        }
    }, {passive: true});
    window.addEventListener("resize", () => { naturalHeight = null; apply(); });
}

// ---------------------------------------------------------------------------
// 9. Status bar wiring
// ---------------------------------------------------------------------------

function setupStatusBarButtons() {
    document.getElementById("ws-reconnect")?.addEventListener("click", () => {
        try { ws?.close(); } catch {}
        wsRetryMs = 500;
        connectWs();
    });
    document.getElementById("reboot-btn")?.addEventListener("click", rebootDevice);
    document.getElementById("theme-toggle")?.addEventListener("click", () => {
        theme = (theme === "dark") ? "light" : "dark";
        localStorage.setItem(LS_THEME, theme);
        applyTheme(theme);
    });
}

function applyTheme(t) {
    document.body.dataset.theme = t;
    const btn = document.getElementById("theme-toggle");
    if (btn) btn.textContent = (t === "dark") ? "☀" : "🌙";
}

function updateStatusBar() {
    if (!state || !state.modules) return;
    const sys = state.modules.find(m => m.name === "System");
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

    // System stats: "uptime · NN KB heap"
    const uptimeCtrl = ctrls.find(c => c.name === "uptime");
    const heapCtrl = ctrls.find(c => c.name === "heap");
    const statsEl = document.getElementById("sys-stats");
    if (statsEl) {
        const parts = [];
        if (uptimeCtrl) parts.push(uptimeCtrl.value);
        if (heapCtrl && heapCtrl.value !== undefined && heapCtrl.total) {
            const freeKb = Math.round((heapCtrl.total - heapCtrl.value) / 1024);
            parts.push(freeKb + "K free");
        }
        statsEl.textContent = parts.join(" · ");
    }

    // bootReason → crashed-state styling on reboot button
    const reasonCtrl = ctrls.find(c => c.name === "bootReason");
    const rebootBtn = document.getElementById("reboot-btn");
    if (rebootBtn && reasonCtrl) {
        const crashed = ["PANIC", "INT_WDT", "TASK_WDT", "BROWNOUT"].includes(reasonCtrl.value);
        rebootBtn.dataset.crashed = crashed ? "true" : "false";
        if (crashed) rebootBtn.title = "Last boot: " + reasonCtrl.value + " (click to reboot)";
        else rebootBtn.title = "Reboot device";
    }
}

// ---------------------------------------------------------------------------
// 10. Boot
// ---------------------------------------------------------------------------

document.addEventListener("DOMContentLoaded", init);
