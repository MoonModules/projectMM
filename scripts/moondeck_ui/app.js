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
            if (isRunning) {
                const btn = document.querySelector(`.run-btn[data-id="${scriptId}"]`);
                if (btn) {
                    btn.classList.add("running");
                    btn.textContent = "Stop";
                }
            }
        }
    } catch (e) {
        // ignore — non-critical
    }
}

// ---------------------------------------------------------------------------
// Tabs (sidebar)
// ---------------------------------------------------------------------------

function setupTabs() {
    // Restore saved tab
    if (state.tab) {
        document.querySelectorAll(".tab").forEach(b => b.classList.remove("active"));
        document.querySelectorAll(".tab-content").forEach(s => s.classList.remove("active"));
        const btn = document.querySelector(`.tab[data-tab="${state.tab}"]`);
        if (btn) {
            btn.classList.add("active");
            document.getElementById("tab-" + state.tab).classList.add("active");
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

    const params = {};
    if (script.needs_env) params.env = state.env;
    if (script.needs_port) params.port = state.port;

    // Switch to log pane and show output
    switchPane("log");
    btn.classList.add("running");
    btn.textContent = script.long_running ? "Stop" : "...";
    appendLog(`\n--- ${script.label} ---\n`);

    const dot = document.querySelector(`.status-dot[data-id="${script.id}"]`);
    if (dot) { dot.className = "status-dot running"; }

    function resetBtn(exitCode) {
        btn.classList.remove("running");
        btn.textContent = "Run";
        if (dot) {
            dot.className = exitCode === 0 ? "status-dot pass" : "status-dot fail";
        }
    }

    try {
        const resp = await fetch("/api/run/" + script.id, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(params),
        });
        const result = await resp.json();

        if (result.error) {
            appendLog("Error: " + result.error + "\n");
            resetBtn(1);
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
            } catch {
                resetBtn(0);
            }
        });

        evtSource.onerror = () => {
            evtSource.close();
            resetBtn(1);
        };
    } catch (err) {
        appendLog("Failed: " + err.message + "\n");
        resetBtn(1);
    }
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
    state.devices = data.devices;
    await saveState();
    renderDevices();
    appendLog("Found " + data.devices.length + " device(s)\n");
});

function renderDevices() {
    const el = document.getElementById("device-list");
    if (!state.devices || state.devices.length === 0) {
        el.textContent = "No devices discovered yet.";
        return;
    }
    el.innerHTML = "";
    for (const device of state.devices) {
        const div = document.createElement("div");
        div.className = "device-item";
        div.textContent = device.name || device.ip;
        div.title = device.ip;
        div.addEventListener("click", () => {
            showInView("http://" + device.ip);
        });
        el.appendChild(div);
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
