// MoonDeck UI

const logEl = document.getElementById("log");
const viewFrame = document.getElementById("view-frame");
const MOONDECK_MD = "/api/help";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

let scripts = [];
let boards = [];
let scenarios = [];   // [{name, module, also}]
let testModules = []; // ["CamelCaseName", ...]
let state = { board: "", port: "", devices: [], scenario: "", module: "" }; // scenario + module shared across all needs_scenario / needs_module cards

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

    const modResp = await fetch("/api/test-modules");
    const modData = await modResp.json();
    testModules = modData.modules || [];

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

// Build a module-filter row: <select> with "all modules" + every test module,
// plus a Tests button that opens the per-module unit-test list. Used both for
// the shared row above a group and the per-card row inside a script-card.
// `onChange(mod)` fires after state.module is saved so the caller can refresh
// dependent scenario dropdowns.
function buildModuleRow({ rowClass, onChange }) {
    const row = document.createElement("div");
    row.className = rowClass;
    row.innerHTML = `
        <select class="module-select" title="Filter by module"></select>
        <button class="tests-btn" title="Show the selected module's unit tests">Tests</button>
    `;
    const modSel = row.querySelector(".module-select");
    const testsBtn = row.querySelector(".tests-btn");
    const allOpt = document.createElement("option");
    allOpt.value = "";
    allOpt.textContent = "all modules";
    modSel.appendChild(allOpt);
    for (const name of testModules) {
        const opt = document.createElement("option");
        opt.value = name;
        opt.textContent = name;
        if (name === state.module) opt.selected = true;
        modSel.appendChild(opt);
    }
    // Tests button is meaningful only for a specific module — same shape as
    // Steps for scenarios. Disabled when "all modules" is selected.
    const syncTestsBtn = () => { testsBtn.disabled = !modSel.value; };
    syncTestsBtn();
    modSel.addEventListener("change", () => {
        state.module = modSel.value;
        saveState();
        syncTestsBtn();
        if (onChange) onChange(state.module);
    });
    testsBtn.addEventListener("click", () => {
        if (modSel.value) showInView("/api/unit-tests/" + encodeURIComponent(modSel.value));
    });
    return row;
}

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
        // Per-(tab,group) shared module row: when >=2 cards in the same group
        // declare needs_module, we render ONE module dropdown at the top of the
        // group and skip the per-card module rows. groupModSelects[key] holds
        // the scenario-select repopulate callbacks the shared dropdown drives.
        const groupModSelects = {};
        const groupKey = (script, target) => `${tab}::${script.group}::${target === container ? "main" : "side"}`;
        const sharedModuleGroup = (script, target) => {
            const k = groupKey(script, target);
            return (groupModSelects[k] && groupModSelects[k].count >= 2) ? k : null;
        };

        // First pass: count needs_module cards per (tab,group,target) so we
        // know whether to render a shared module row when the group opens.
        for (const script of tabScripts) {
            let target = container;
            if (tab === "esp32") {
                if (script.group === "setup") target = esp32SetupContainer;
                else if (script.group === "build") target = esp32BuildContainer;
            }
            if (!script.needs_module) continue;
            const k = groupKey(script, target);
            if (!groupModSelects[k]) groupModSelects[k] = { count: 0, repopulators: [] };
            groupModSelects[k].count++;
        }

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
                // If this group has a shared module dropdown, render it now —
                // right under the header, before any card in the group.
                const sharedKey = sharedModuleGroup(script, target);
                if (sharedKey) {
                    const row = buildModuleRow({
                        rowClass: "shared-module-row",
                        onChange: (mod) => {
                            // Re-populate every scenario select wired to this group.
                            for (const repop of groupModSelects[sharedKey].repopulators) {
                                repop(mod);
                            }
                        },
                    });
                    target.appendChild(row);
                }
            }

            const usesSharedModule = !!sharedModuleGroup(script, target);
            const renderOwnModuleRow = script.needs_module && !usesSharedModule;
            const card = document.createElement("div");
            const hasExtras = script.needs_scenario || renderOwnModuleRow || (script.flags && script.flags.length > 0);
            card.className = "script-card" + (hasExtras ? " script-card--has-select" : "");
            card.innerHTML = `
                <div class="card-row">
                    <span class="status-dot" data-id="${script.id}"></span>
                    <span class="label">${script.label}</span>
                    <button class="help-btn" title="Help">?</button>
                    <button class="run-btn" data-id="${script.id}">Run</button>
                </div>
                ${script.needs_scenario ? `<div class="scenario-row">
                    <select class="scenario-select"></select>
                    <button class="steps-btn" title="Show the selected scenario's steps">Steps</button>
                </div>` : ""}
                ${script.flags && script.flags.length > 0 ? `<div class="flag-row"></div>` : ""}
            `;

            // Helper: which scenarios apply to the currently-selected module?
            const filterScenariosByModule = (mod) => {
                if (!mod) return scenarios;
                return scenarios.filter(s => s.module === mod || (s.also || []).includes(mod));
            };

            // Per-card module dropdown — only when this card isn't covered by
            // a shared module row above its group. Inserted between the card-row
            // and the scenario-row (if any) using the same builder the shared
            // row uses, so the two cases stay visually and behaviourally identical.
            if (renderOwnModuleRow) {
                const row = buildModuleRow({
                    rowClass: "module-row",
                    onChange: (mod) => {
                        const scenSel = card.querySelector(".scenario-select");
                        if (scenSel) repopulateScenarioSelect(scenSel, mod);
                    },
                });
                card.insertBefore(row, card.children[1] || null);
            }

            const repopulateScenarioSelect = (sel, mod) => {
                const prev = sel.value;
                sel.innerHTML = "";
                const allOpt = document.createElement("option");
                allOpt.value = "";
                allOpt.textContent = "all";
                sel.appendChild(allOpt);
                let kept = false;
                for (const s of filterScenariosByModule(mod)) {
                    const opt = document.createElement("option");
                    opt.value = s.name;
                    opt.textContent = s.name;
                    if (s.name === prev) { opt.selected = true; kept = true; }
                    else if (s.name === state.scenario && !prev) { opt.selected = true; kept = true; }
                    sel.appendChild(opt);
                }
                if (!kept) {
                    // The previously-selected scenario no longer matches the module —
                    // fall back to "all" and persist so the next render is consistent.
                    state.scenario = "";
                    saveState();
                    sel.value = "";
                }
                sel.dispatchEvent(new Event("change"));
            };

            if (script.needs_scenario) {
                const sel = card.querySelector(".scenario-select");
                const stepsBtn = card.querySelector(".steps-btn");
                repopulateScenarioSelect(sel, script.needs_module ? state.module : "");
                // Steps button is meaningful only for a specific scenario — the "all"
                // (empty) value has no single steps file to show.
                const syncStepsBtn = () => { stepsBtn.disabled = !sel.value; };
                syncStepsBtn();
                sel.addEventListener("change", () => {
                    state.scenario = sel.value;
                    saveState();
                    syncStepsBtn();
                });
                stepsBtn.addEventListener("click", () => {
                    if (sel.value) showInView("/api/scenarios/" + encodeURIComponent(sel.value));
                });
                // Register with the shared module dropdown (if any) so changing it
                // re-populates this card's scenario list.
                const sharedKey = sharedModuleGroup(script, target);
                if (sharedKey) {
                    groupModSelects[sharedKey].repopulators.push((mod) => {
                        repopulateScenarioSelect(sel, mod);
                    });
                }
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
    // Any running script can be stopped — long_running ones are the typical case
    // (run_desktop, monitor_esp32, …) but a foreground script that's taking longer
    // than expected (e.g. a full unit-test run) should also be killable mid-flight.
    if (btn.classList.contains("running")) {
        appendLog(`\n--- Stopping ${script.label} ---\n`);
        await fetch("/api/kill/" + script.id, { method: "POST" });
        btn.classList.remove("running");
        btn.textContent = "Run";
        const dot = document.querySelector(`.status-dot[data-id="${script.id}"]`);
        if (dot) { dot.className = "status-dot"; }
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
    if (script.needs_module) params.module = state.module;
    for (const flag of (script.flags || [])) {
        const stateKey = `flag_${script.id}_${flag.id}`;
        params[`flag_${flag.id}`] = stateKey in state ? state[stateKey] : flag.default;
    }

    // Switch to log pane and show output
    switchPane("log");
    btn.classList.add("running");
    // Show Stop on every running script so the user can interrupt a slow run.
    btn.textContent = "Stop";
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
    // Update existing devices found in scan. Live-readable fields
    // (deviceName, firmware, deduced board) win from the probe; user-set or
    // flash-tracked fields (board when not deducible, last_port) survive.
    for (const found of data.devices) {
        if (existingByIp[found.ip]) {
            const e = existingByIp[found.ip];
            e.online = true;
            e.deviceName = found.deviceName || e.deviceName || "";
            e.firmware = found.firmware || e.firmware || "";
            // Probe's deduced board wins when set; otherwise keep the user-set value.
            if (found.board) e.board = found.board;
            else if (!e.board) e.board = "";
            // last_port stays as-is (probe doesn't supply it).
            delete e.modules;  // legacy field removed; drop on next refresh.
        } else {
            existing.push({ ...found, online: true, selected: false, last_port: "" });
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
        // Label: <name> · <ip> · fw:<firmware>. Board is rendered separately
        // as a picker (below) since some firmwares run on multiple boards.
        // last_port lives in the tooltip — it's flash-history, not identity.
        const parts = [];
        if (device.deviceName) parts.push(device.deviceName);
        parts.push(device.ip);
        if (device.firmware) parts.push(`fw:${device.firmware}`);
        text.textContent = parts.join(" · ");
        const tooltipLines = [device.ip];
        if (device.last_port) tooltipLines.push(`last flashed via ${device.last_port}`);
        text.title = tooltipLines.join("\n");
        text.style.cursor = "pointer";
        text.addEventListener("click", (e) => {
            if (e.target === text) showInView("http://" + device.ip);
        });

        // Board picker — hardcoded list of known boards. Auto-deduced for eth
        // firmwares (probe sets device.board); user-set for `esp32` since the
        // firmware can run on multiple boards and the device can't tell us.
        // Future phase: drive the dropdown options from a structured catalog
        // when board presets exist; for now this short list is enough.
        const boardPicker = document.createElement("select");
        boardPicker.className = "device-board";
        boardPicker.title = "Physical board (pick when firmware can't tell us)";
        const boardOptions = [
            ["", "(unknown board)"],
            ["olimex-esp32-gateway-rev-g", "Olimex ESP32-Gateway Rev G"],
            ["lolin-d32", "LOLIN D32"],
            ["generic-esp32", "Generic ESP32 DevKit"],
        ];
        for (const [val, lbl] of boardOptions) {
            const opt = document.createElement("option");
            opt.value = val;
            opt.textContent = lbl;
            if ((device.board || "") === val) opt.selected = true;
            boardPicker.appendChild(opt);
        }
        boardPicker.addEventListener("change", () => {
            device.board = boardPicker.value;
            saveState();
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
        label.appendChild(boardPicker);
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
