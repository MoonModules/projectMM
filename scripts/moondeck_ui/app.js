// MoonDeck UI

const logEl = document.getElementById("log");
const viewFrame = document.getElementById("view-frame");
const MOONDECK_MD = "/api/help";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

let scripts = [];
let boards = [];
let scenarios = [];
let state = { board: "", port: "", devices: [], scenario: "" }; // scenario is shared across all needs_scenario cards (PC + Live stay in sync intentionally)

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

async function init() {
    const resp = await fetch("/api/scripts");
    const data = await resp.json();
    scripts = data.scripts;
    boards = data.boards;

    const stateResp = await fetch("/api/state");
    state = await stateResp.json();

    // Migrate legacy persisted state: old saves had `env: "esp32"` (a chip
    // family). The dropdown now holds firmware-board keys
    // (`esp32` / `esp32-eth` / `esp32-eth-wifi` / `esp32s3-n16r8`). If the
    // saved board isn't in the new list, drop it so the default selection
    // (first board) wins.
    if (!boards.includes(state.board)) state.board = "";

    const scenResp = await fetch("/api/scenarios");
    const scenData = await scenResp.json();
    scenarios = scenData.scenarios || [];

    renderBoardSelect();
    renderScripts();
    try { renderDevices(); } catch (e) { console.error("renderDevices:", e); }
    await updateRunningState();
    refreshPorts();
    setupTabs();
    setupPaneTabs();
}

async function updateRunningState() {
    try {
        const resp = await fetch("/api/running");
        const running = await resp.json();
        for (const [scriptId, isRunning] of Object.entries(running)) {
            const btn = document.querySelector(`.run-btn[data-id="${scriptId}"]`);
            if (!btn) continue;
            const dot = document.querySelector(`.status-dot[data-id="${scriptId}"]`);
            if (isRunning && !btn.classList.contains("running")) {
                btn.classList.add("running");
                btn.textContent = "Stop";
                if (dot) dot.className = "status-dot running";
            } else if (!isRunning && btn.classList.contains("running")) {
                btn.classList.remove("running");
                btn.textContent = "Run";
                if (dot) dot.className = "status-dot";
            }
        }
    } catch {
        // ignore — non-critical
    }
}

// Poll running state every 5 seconds
setInterval(updateRunningState, 5000);

// ---------------------------------------------------------------------------
// Tabs (sidebar)
// ---------------------------------------------------------------------------

function setupTabs() {
    // ?tab=<name> URL param overrides saved state (used by screenshot automation)
    const urlTab = new URLSearchParams(location.search).get("tab");
    const activeTab = urlTab || state.tab;
    if (activeTab) {
        document.querySelectorAll(".tab").forEach(b => b.classList.remove("active"));
        document.querySelectorAll(".tab-content").forEach(s => s.classList.remove("active"));
        const btn = document.querySelector(`.tab[data-tab="${activeTab}"]`);
        const content = document.getElementById("tab-" + activeTab);
        if (btn && content) {
            btn.classList.add("active");
            content.classList.add("active");
        }
    }

    document.querySelectorAll(".tab").forEach(btn => {
        btn.addEventListener("click", () => {
            document.querySelectorAll(".tab").forEach(b => b.classList.remove("active"));
            document.querySelectorAll(".tab-content").forEach(s => s.classList.remove("active"));
            btn.classList.add("active");
            document.getElementById("tab-" + btn.dataset.tab).classList.add("active");
            state.tab = btn.dataset.tab;
            saveState();
        });
    });
}

// ---------------------------------------------------------------------------
// Pane tabs (main area: Log / View)
// ---------------------------------------------------------------------------

function setupPaneTabs() {
    document.querySelectorAll(".pane-tab").forEach(btn => {
        btn.addEventListener("click", () => {
            switchPane(btn.dataset.pane);
        });
    });
}

const viewNav = document.getElementById("view-nav");
const clearLogBtn = document.getElementById("clear-log");

function switchPane(pane) {
    document.querySelectorAll(".pane-tab").forEach(b => b.classList.remove("active"));
    document.querySelectorAll(".pane-content").forEach(p => p.classList.remove("active"));
    document.querySelector(`.pane-tab[data-pane="${pane}"]`).classList.add("active");
    document.getElementById("pane-" + pane).classList.add("active");
    viewNav.hidden = (pane !== "view");
    clearLogBtn.hidden = (pane !== "log");
}

function viewNavAction(fn) {
    try { fn(viewFrame.contentWindow); } catch (_) {}
}
document.getElementById("view-back").addEventListener("click", () => viewNavAction(w => w.history.back()));
document.getElementById("view-forward").addEventListener("click", () => viewNavAction(w => w.history.forward()));
document.getElementById("view-refresh").addEventListener("click", () => {
    if (viewFrame.src) viewFrame.src = viewFrame.src;
});

function showInView(url) {
    viewFrame.src = url;
    switchPane("view");
}

window.addEventListener("message", (e) => {
    if (e.source !== viewFrame.contentWindow) return;  // only accept from our iframe
    if (e.data?.type === "moondeck-nav" && typeof e.data.url === "string"
            && e.data.url.startsWith("/api/")) {
        showInView(e.data.url);
    }
});

// ---------------------------------------------------------------------------
// Script cards
// ---------------------------------------------------------------------------

function renderScripts() {
    const containers = {
        pc: document.getElementById("scripts-pc"),
        esp32: document.getElementById("scripts-esp32"),
        live: document.getElementById("scripts-live"),
    };
    // The esp32 tab splits its scripts across three containers so the
    // dropdowns (Firmware, Port) can sit *between* the script groups they
    // belong to. Setup → top, Build → after the Firmware dropdown, Flash +
    // run → after the Port dropdown (the main container). Every other tab
    // uses a single container.
    const esp32SetupContainer = document.getElementById("scripts-esp32-setup");
    const esp32BuildContainer = document.getElementById("scripts-esp32-build");

    for (const [tab, container] of Object.entries(containers)) {
        container.innerHTML = "";
        if (tab === "esp32") {
            esp32SetupContainer.innerHTML = "";
            esp32BuildContainer.innerHTML = "";
        }
        const tabScripts = scripts.filter(s => s.tab === tab);

        // Per-container last-group tracking: each target keeps its own
        // header state so we don't suppress a header just because the same
        // group name appeared in the *other* container.
        const lastGroupByTarget = new WeakMap();
        for (const script of tabScripts) {
            let target = container;
            if (tab === "esp32") {
                if (script.group === "setup") target = esp32SetupContainer;
                else if (script.group === "build") target = esp32BuildContainer;
            }
            const lastGroup = lastGroupByTarget.get(target) || "";
            if (script.group !== lastGroup) {
                lastGroupByTarget.set(target, script.group);
                const header = document.createElement("div");
                header.className = "group-header";
                header.textContent = script.group;
                target.appendChild(header);
            }
            const card = document.createElement("div");
            const hasExtras = script.needs_scenario || (script.flags && script.flags.length > 0);
            card.className = "script-card" + (hasExtras ? " script-card--has-select" : "");
            card.innerHTML = `
                <div class="card-row">
                    <span class="status-dot" data-id="${script.id}"></span>
                    <span class="label">${script.label}</span>
                    <button class="help-btn" title="Help">?</button>
                    <button class="run-btn" data-id="${script.id}">Run</button>
                </div>
                ${script.needs_scenario ? `<select class="scenario-select"></select>` : ""}
                ${script.flags && script.flags.length > 0 ? `<div class="flag-row"></div>` : ""}
            `;

            if (script.needs_scenario) {
                const sel = card.querySelector(".scenario-select");
                const allOpt = document.createElement("option");
                allOpt.value = "";
                allOpt.textContent = "all";
                sel.appendChild(allOpt);
                for (const name of scenarios) {
                    const opt = document.createElement("option");
                    opt.value = name;
                    opt.textContent = name;
                    if (name === state.scenario) opt.selected = true;
                    sel.appendChild(opt);
                }
                sel.addEventListener("change", () => { state.scenario = sel.value; saveState(); });
            }

            if (script.flags && script.flags.length > 0) {
                const flagRow = card.querySelector(".flag-row");
                for (const flag of script.flags) {
                    const stateKey = `flag_${script.id}_${flag.id}`;
                    const checked = stateKey in state ? state[stateKey] : flag.default;
                    const label = document.createElement("label");
                    label.className = "flag-label";
                    const cb = document.createElement("input");
                    cb.type = "checkbox";
                    cb.checked = checked;
                    state[stateKey] = checked;
                    cb.addEventListener("change", () => {
                        state[stateKey] = cb.checked;
                        saveState();
                    });
                    label.appendChild(cb);
                    label.append(" " + flag.label);
                    flagRow.appendChild(label);
                }
            }

            card.querySelector(".help-btn").addEventListener("click", () => {
                showInView(MOONDECK_MD + "?" + script.help);
            });

            card.querySelector(".run-btn").addEventListener("click", (e) => {
                runScript(script, e.target);
            });

            target.appendChild(card);
        }
    }
}

// ---------------------------------------------------------------------------
// Script execution
// ---------------------------------------------------------------------------

async function runScript(script, btn) {
    // Destructive actions (erase flash, future reset-to-defaults) require a
    // confirmation step — native browser confirm() to match MoonDeck's
    // vanilla style. Done before the long-running / live-tab branches so an
    // already-running destructive script (none exist today, future-proofing)
    // can still be stopped without re-confirming.
    if (script.destructive && !btn.classList.contains("running")) {
        if (!confirm(`${script.label} is destructive — are you sure?`)) return;
    }
    // Long-running scripts toggle between Run and Stop
    if (btn.classList.contains("running")) {
        if (script.long_running) {
            appendLog(`\n--- Stopping ${script.label} ---\n`);
            await fetch("/api/kill/" + script.id, { method: "POST" });
            btn.classList.remove("running");
            btn.textContent = "Run";
            const dot = document.querySelector(`.status-dot[data-id="${script.id}"]`);
            if (dot) { dot.className = "status-dot"; }
        }
        return;
    }

    // Live tab scripts: run against each selected device
    if (script.tab === "live" && script.needs_device) {
        const devices = (state.devices || []).filter(d => d.selected);
        if (devices.length === 0) {
            switchPane("log");
            appendLog("\n--- No devices selected. Use Discover and check devices first. ---\n");
            return;
        }
        let allPassed = true;
        for (const device of devices) {
            const ok = await runScriptOnce(script, btn, { host: device.ip });
            if (!ok) allPassed = false;
        }
        return;
    }

    await runScriptOnce(script, btn, {});
}

async function runScriptOnce(script, btn, extraParams) {
    const params = { ...extraParams };
    if (script.needs_board) params.board = state.board;
    if (script.needs_port) params.port = state.port;
    if (script.needs_scenario) params.scenario = state.scenario;
    for (const flag of (script.flags || [])) {
        const stateKey = `flag_${script.id}_${flag.id}`;
        params[`flag_${flag.id}`] = stateKey in state ? state[stateKey] : flag.default;
    }

    // Switch to log pane and show output
    switchPane("log");
    btn.classList.add("running");
    btn.textContent = script.long_running ? "Stop" : "...";
    const hostLabel = params.host ? ` - ${params.host}` : "";
    appendLog(`\n--- ${script.label}${hostLabel} ---\n`);

    const dot = document.querySelector(`.status-dot[data-id="${script.id}"]`);
    if (dot) { dot.className = "status-dot running"; }

    function resetBtn(exitCode) {
        btn.classList.remove("running");
        btn.textContent = "Run";
        if (dot) {
            dot.className = exitCode === 0 ? "status-dot pass" : "status-dot fail";
        }
    }

    return new Promise((resolve) => {
        fetch("/api/run/" + script.id, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(params),
        }).then(r => r.json()).then(result => {
            if (result.error) {
                appendLog("Error: " + result.error + "\n");
                resetBtn(1);
                resolve(false);
                return;
            }

            const evtSource = new EventSource("/api/stream/" + script.id);

            evtSource.onmessage = (e) => {
                appendLog(JSON.parse(e.data) + "\n");
            };

            evtSource.addEventListener("done", (e) => {
                evtSource.close();
                try {
                    const data = JSON.parse(e.data);
                    resetBtn(data.exitCode);
                    // The script may launch a detached process (e.g. run_desktop
                    // spawns projectMM and exits). Refresh the running state now
                    // instead of waiting up to 5s for the poll, so the button
                    // flips back to "Stop" without a visible blink.
                    if (script.long_running) updateRunningState();
                    resolve(data.exitCode === 0);
                } catch {
                    resetBtn(1);
                    resolve(false);
                }
            });

            evtSource.onerror = () => {
                evtSource.close();
                resetBtn(1);
                resolve(false);
            };
        }).catch(err => {
            appendLog("Failed: " + err.message + "\n");
            resetBtn(1);
            resolve(false);
        });
    });
}

// ---------------------------------------------------------------------------
// ESP32 controls
// ---------------------------------------------------------------------------

function renderBoardSelect() {
    const select = document.getElementById("board-select");
    select.innerHTML = "";
    // If no board persisted (fresh state, or legacy state migrated away),
    // default to the first option so Build / etc. always have a valid
    // --board argument to forward.
    if (!state.board && boards.length > 0) state.board = boards[0];
    for (const board of boards) {
        const opt = document.createElement("option");
        opt.value = board;
        opt.textContent = board;
        if (board === state.board) opt.selected = true;
        select.appendChild(opt);
    }
    select.addEventListener("change", async () => {
        state.board = select.value;
        await saveState();
    });
}

async function refreshPorts() {
    const resp = await fetch("/api/ports");
    const data = await resp.json();
    const select = document.getElementById("port-select");
    const current = state.port;
    select.innerHTML = '<option value="">--</option>';
    for (const port of data.ports) {
        const opt = document.createElement("option");
        opt.value = port;
        opt.textContent = port;
        if (port === current) opt.selected = true;
        select.appendChild(opt);
    }
    select.addEventListener("change", async () => {
        state.port = select.value;
        await saveState();
    });
}

document.getElementById("refresh-ports").addEventListener("click", refreshPorts);

// ---------------------------------------------------------------------------
// Live tab
// ---------------------------------------------------------------------------

document.getElementById("discover-btn").addEventListener("click", async () => {
    appendLog("\n--- Discovering devices ---\n");
    switchPane("log");
    const resp = await fetch("/api/discover", { method: "POST" });
    const data = await resp.json();
    appendLog("Scanned subnet: " + (data.subnet || "?") + ".*\n");
    // Merge: keep existing devices, add new ones, update online status
    const existing = state.devices || [];
    const existingByIp = Object.fromEntries(existing.map(d => [d.ip, d]));
    const foundIps = new Set(data.devices.map(d => d.ip));
    // Update existing devices found in scan
    for (const found of data.devices) {
        if (existingByIp[found.ip]) {
            existingByIp[found.ip].online = true;
            existingByIp[found.ip].modules = found.modules;
        } else {
            existing.push({ ...found, online: true, selected: false });
        }
    }
    // Mark not-found existing devices as offline
    for (const d of existing) {
        if (!foundIps.has(d.ip)) d.online = false;
    }
    state.devices = existing;
    await saveState();
    renderDevices();
    const newCount = data.devices.filter(d => !existingByIp[d.ip]).length;
    appendLog(`Found ${data.devices.length} device(s)` + (newCount ? `, ${newCount} new` : "") + "\n");
});

document.getElementById("refresh-devices-btn")?.addEventListener("click", async () => {
    if (!state.devices || state.devices.length === 0) return;
    appendLog("\n--- Refreshing device status ---\n");
    const resp = await fetch("/api/refresh", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ devices: state.devices }),
    });
    const data = await resp.json();
    const onlineIps = new Set(data.online.map(d => d.ip));
    for (const device of state.devices) {
        device.online = onlineIps.has(device.ip);
    }
    await saveState();
    renderDevices();
    const onCount = state.devices.filter(d => d.online).length;
    appendLog(`${onCount}/${state.devices.length} online\n`);
});

function renderDevices() {
    const el = document.getElementById("device-list");
    if (!state.devices || state.devices.length === 0) {
        el.textContent = "No devices discovered yet.";
        return;
    }
    el.innerHTML = "";
    for (const device of state.devices) {
        const label = document.createElement("label");
        label.className = "device-item";

        const dot = document.createElement("span");
        dot.className = "device-status " + (device.online !== false ? "online" : "offline");
        dot.title = device.online !== false ? "online" : "offline";

        const cb = document.createElement("input");
        cb.type = "checkbox";
        cb.checked = !!device.selected;
        cb.addEventListener("change", () => {
            device.selected = cb.checked;
            saveState();
        });

        const text = document.createElement("span");
        text.textContent = `${device.ip} (${device.modules} modules)`;
        text.title = device.ip;
        text.style.cursor = "pointer";
        text.addEventListener("click", (e) => {
            if (e.target === text) showInView("http://" + device.ip);
        });

        const removeBtn = document.createElement("button");
        removeBtn.className = "device-remove";
        removeBtn.textContent = "x";
        removeBtn.title = "Remove device";
        removeBtn.addEventListener("click", (e) => {
            e.preventDefault();
            state.devices = state.devices.filter(d => d.ip !== device.ip);
            saveState();
            renderDevices();
        });

        label.appendChild(dot);
        label.appendChild(cb);
        label.appendChild(text);
        label.appendChild(removeBtn);
        el.appendChild(label);
    }
}

// ---------------------------------------------------------------------------
// State persistence
// ---------------------------------------------------------------------------

async function saveState() {
    await fetch("/api/state", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(state),
    });
}

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------

// http(s):// and file:// links anywhere in script output get linkified.
// Everything else stays as plain textContent — script stdout is treated
// as untrusted, so we never set innerHTML on it; instead we split on URLs
// and append text nodes + <a> nodes, both of which encode HTML-special
// chars automatically.
const URL_RE = /\b((?:https?|file):\/\/[^\s<>"]+)/g;

// `MOONDECK_VIEW: <url>` (relative or absolute) — a marker scripts can
// emit to route the URL straight into the View pane after a one-tick
// delay, AND render a clickable "Open in View pane → <url>" link in the
// log so the user knows what just happened. Same-origin routing without
// relying on the user clicking through.
const VIEW_MARKER_RE = /^MOONDECK_VIEW:\s*(\S+)\s*$/;

function appendLog(text) {
    // Marker shortcut: if this chunk is a single MOONDECK_VIEW line, swap
    // it for an explanatory clickable link and auto-open in the View pane.
    // Strip trailing newlines before matching so the marker survives the
    // "+ \n" the SSE stream tacks on per line.
    const stripped = text.replace(/\n+$/, "");
    const markerMatch = VIEW_MARKER_RE.exec(stripped);
    if (markerMatch) {
        let safeUrl = null;
        try {
            // Use document.baseURI as base so relative paths like /api/history-report resolve.
            const parsed = new URL(markerMatch[1], document.baseURI);
            if (parsed.protocol === "http:" || parsed.protocol === "https:") {
                safeUrl = parsed.href;
            }
        } catch (_) { /* invalid URL — skip */ }
        if (safeUrl) {
            const a = document.createElement("a");
            a.href = safeUrl;
            a.textContent = "Open in View pane → " + safeUrl;
            a.addEventListener("click", (ev) => { ev.preventDefault(); showInView(safeUrl); });
            logEl.appendChild(a);
            logEl.appendChild(document.createTextNode("\n"));
            logEl.scrollTop = logEl.scrollHeight;
            // Defer the actual View-pane switch so the log row renders first
            // (otherwise the user can't see what was just produced when they
            // tab back to Log).
            setTimeout(() => showInView(safeUrl), 50);
        } else {
            logEl.appendChild(document.createTextNode(stripped + "\n"));
            logEl.scrollTop = logEl.scrollHeight;
        }
        return;
    }
    // Fast path: no URL in this chunk, plain append.
    if (!URL_RE.test(text)) {
        logEl.appendChild(document.createTextNode(text));
        logEl.scrollTop = logEl.scrollHeight;
        return;
    }
    URL_RE.lastIndex = 0;
    let lastIdx = 0;
    let m;
    while ((m = URL_RE.exec(text)) !== null) {
        if (m.index > lastIdx) {
            logEl.appendChild(document.createTextNode(text.slice(lastIdx, m.index)));
        }
        const url = m[1];
        const a = document.createElement("a");
        a.href = url;
        // Same-origin URLs (file:// AND http://localhost-the-MoonDeck-server)
        // open inside the View pane — that's a "show me the rendered content
        // MoonDeck just produced" gesture, not "leave MoonDeck for an external
        // site." Cross-origin http(s):// opens in a new tab, the normal
        // external-link behaviour.
        const isMoonDeckUrl =
            url.startsWith("file://") ||
            url.startsWith(location.origin + "/") ||
            url.startsWith(location.origin) && url.length === location.origin.length;
        if (isMoonDeckUrl) {
            a.addEventListener("click", (ev) => {
                ev.preventDefault();
                showInView(url);
            });
        } else {
            a.target = "_blank";
            a.rel = "noopener";
        }
        a.textContent = url;
        logEl.appendChild(a);
        lastIdx = m.index + url.length;
    }
    if (lastIdx < text.length) {
        logEl.appendChild(document.createTextNode(text.slice(lastIdx)));
    }
    logEl.scrollTop = logEl.scrollHeight;
}

document.getElementById("clear-log").addEventListener("click", () => {
    logEl.textContent = "";
});

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------

init();
