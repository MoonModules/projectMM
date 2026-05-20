// MoonDeck UI

const logEl = document.getElementById("log");
const viewFrame = document.getElementById("view-frame");
const MOONDECK_MD = "/api/help";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

let scripts = [];
let envs = [];
let state = { env: "", port: "", devices: [] };

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

async function init() {
    const resp = await fetch("/api/scripts");
    const data = await resp.json();
    scripts = data.scripts;
    envs = data.envs;

    const stateResp = await fetch("/api/state");
    state = await stateResp.json();

    renderEnvSelect();
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
    // Restore saved tab
    if (state.tab) {
        document.querySelectorAll(".tab").forEach(b => b.classList.remove("active"));
        document.querySelectorAll(".tab-content").forEach(s => s.classList.remove("active"));
        const btn = document.querySelector(`.tab[data-tab="${state.tab}"]`);
        const content = document.getElementById("tab-" + state.tab);
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

function switchPane(pane) {
    document.querySelectorAll(".pane-tab").forEach(b => b.classList.remove("active"));
    document.querySelectorAll(".pane-content").forEach(p => p.classList.remove("active"));
    document.querySelector(`.pane-tab[data-pane="${pane}"]`).classList.add("active");
    document.getElementById("pane-" + pane).classList.add("active");
}

function showInView(url) {
    viewFrame.src = url;
    switchPane("view");
}

// ---------------------------------------------------------------------------
// Script cards
// ---------------------------------------------------------------------------

function renderScripts() {
    const containers = {
        pc: document.getElementById("scripts-pc"),
        esp32: document.getElementById("scripts-esp32"),
        live: document.getElementById("scripts-live"),
    };

    for (const [tab, container] of Object.entries(containers)) {
        container.innerHTML = "";
        const tabScripts = scripts.filter(s => s.tab === tab);

        let lastGroup = "";
        for (const script of tabScripts) {
            if (script.group !== lastGroup) {
                lastGroup = script.group;
                const header = document.createElement("div");
                header.className = "group-header";
                header.textContent = script.group;
                container.appendChild(header);
            }
            const card = document.createElement("div");
            card.className = "script-card";
            card.innerHTML = `
                <span class="status-dot" data-id="${script.id}"></span>
                <span class="label">${script.label}</span>
                <button class="help-btn" title="Help">?</button>
                <button class="run-btn" data-id="${script.id}">Run</button>
            `;

            card.querySelector(".help-btn").addEventListener("click", () => {
                showInView(MOONDECK_MD + "?" + script.help);
            });

            card.querySelector(".run-btn").addEventListener("click", (e) => {
                runScript(script, e.target);
            });

            container.appendChild(card);
        }
    }
}

// ---------------------------------------------------------------------------
// Script execution
// ---------------------------------------------------------------------------

async function runScript(script, btn) {
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
    if (script.needs_env) params.env = state.env;
    if (script.needs_port) params.port = state.port;

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

function renderEnvSelect() {
    const select = document.getElementById("env-select");
    select.innerHTML = "";
    for (const env of envs) {
        const opt = document.createElement("option");
        opt.value = env;
        opt.textContent = env;
        if (env === state.env) opt.selected = true;
        select.appendChild(opt);
    }
    select.addEventListener("change", async () => {
        state.env = select.value;
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

function appendLog(text) {
    logEl.textContent += text;
    logEl.scrollTop = logEl.scrollHeight;
}

document.getElementById("clear-log").addEventListener("click", () => {
    logEl.textContent = "";
});

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------

init();
