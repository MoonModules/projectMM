// mmv3 Web UI

let state = null;
let selectedModule = null;
let ws = null;
const dragTimers = {};  // per-control debounce timers
const dragTs = {};      // per-control last-drag timestamp

// ---------------------------------------------------------------------------
// WebSocket
// ---------------------------------------------------------------------------

function connectWs() {
    const url = `ws://${location.host}/ws`;
    ws = new WebSocket(url);

    ws.onopen = () => {
        document.getElementById("ws-dot").className = "ws-dot connected";
    };

    ws.onclose = () => {
        document.getElementById("ws-dot").className = "ws-dot disconnected";
        setTimeout(connectWs, 2000);
    };

    ws.onerror = () => ws.close();

    ws.onmessage = (e) => {
        try {
            const data = JSON.parse(e.data);
            state = data;
            updateValues();
        } catch {
            // ignore malformed messages
        }
    };
}

// ---------------------------------------------------------------------------
// Initial load
// ---------------------------------------------------------------------------

async function init() {
    try {
        const resp = await fetch("/api/state");
        state = await resp.json();
        renderNav();
        if (state.modules && state.modules.length > 0) {
            selectModule(state.modules[0].name);
        }
    } catch (err) {
        document.getElementById("main").textContent = "Error: " + err.message;
    }
    connectWs();
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

function renderNav() {
    const nav = document.getElementById("nav");
    nav.innerHTML = "";
    if (!state || !state.modules) return;

    for (const mod of state.modules) {
        const item = document.createElement("div");
        item.className = "nav-item" + (mod.name === selectedModule ? " active" : "");
        item.textContent = mod.name;
        item.addEventListener("click", () => selectModule(mod.name));
        nav.appendChild(item);
    }
}

function selectModule(name) {
    selectedModule = name;
    renderNav();
    renderCards();
}

// ---------------------------------------------------------------------------
// Module cards
// ---------------------------------------------------------------------------

function renderCards() {
    const main = document.getElementById("main");
    main.innerHTML = "";
    if (!state || !state.modules) return;

    const mod = state.modules.find(m => m.name === selectedModule);
    if (!mod) return;

    // Render parent card if it has controls
    if (mod.controls && mod.controls.length > 0) {
        main.appendChild(createCard(mod));
    }

    // Render children
    if (mod.children) {
        for (const child of mod.children) {
            main.appendChild(createCard(child));
        }
    }
}

function createCard(mod) {
    const card = document.createElement("div");
    card.className = "card";
    card.dataset.module = mod.name;

    const title = document.createElement("div");
    title.className = "card-title";
    title.textContent = mod.name;
    card.appendChild(title);

    if (mod.controls) {
        for (const ctrl of mod.controls) {
            card.appendChild(createControl(mod.name, ctrl));
        }
    }
    return card;
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

function createControl(moduleName, ctrl) {
    const row = document.createElement("div");
    row.className = "control-row";
    row.dataset.module = moduleName;
    row.dataset.control = ctrl.name;

    const label = document.createElement("label");
    label.textContent = ctrl.name;
    row.appendChild(label);

    const key = `${moduleName}.${ctrl.name}`;

    if (ctrl.type === "uint8" || ctrl.type === "uint16") {
        const input = document.createElement("input");
        input.type = "range";
        input.min = ctrl.min !== undefined ? ctrl.min : 0;
        input.max = ctrl.max !== undefined ? ctrl.max : (ctrl.type === "uint8" ? 255 : 65535);
        input.value = ctrl.value;
        input.dataset.key = key;

        const valSpan = document.createElement("span");
        valSpan.className = "control-value";
        valSpan.textContent = ctrl.value;
        valSpan.dataset.key = key;

        input.addEventListener("input", () => {
            valSpan.textContent = input.value;
            dragTs[key] = Date.now();
            clearTimeout(dragTimers[key]);
            dragTimers[key] = setTimeout(() => {
                sendControl(moduleName, ctrl.name, parseInt(input.value));
            }, 150);
        });

        row.appendChild(input);
        row.appendChild(valSpan);

    } else if (ctrl.type === "bool") {
        const input = document.createElement("input");
        input.type = "checkbox";
        input.checked = ctrl.value;
        input.dataset.key = key;

        input.addEventListener("change", () => {
            sendControl(moduleName, ctrl.name, input.checked);
        });

        row.appendChild(input);

    } else if (ctrl.type === "text") {
        const input = document.createElement("input");
        input.type = "text";
        input.value = ctrl.value || "";
        input.dataset.key = key;

        input.addEventListener("input", () => {
            dragTs[key] = Date.now();
            clearTimeout(dragTimers[key]);
            dragTimers[key] = setTimeout(() => {
                sendControl(moduleName, ctrl.name, input.value);
            }, 500);
        });

        row.appendChild(input);
    }

    return row;
}

// ---------------------------------------------------------------------------
// State updates (from WebSocket)
// ---------------------------------------------------------------------------

function updateValues() {
    if (!state || !state.modules) return;

    for (const mod of state.modules) {
        updateModuleControls(mod);
        if (mod.children) {
            for (const child of mod.children) {
                updateModuleControls(child);
            }
        }
    }
}

function updateModuleControls(mod) {
    if (!mod.controls) return;

    for (const ctrl of mod.controls) {
        const key = `${mod.name}.${ctrl.name}`;

        // Don't overwrite if user is actively dragging (1-second cooldown)
        if (dragTs[key] && Date.now() - dragTs[key] < 1000) continue;

        // Update slider
        const slider = document.querySelector(`input[data-key="${key}"][type="range"]`);
        if (slider && parseInt(slider.value) !== ctrl.value) {
            slider.value = ctrl.value;
            const valSpan = document.querySelector(`span.control-value[data-key="${key}"]`);
            if (valSpan) valSpan.textContent = ctrl.value;
        }

        // Update checkbox
        const checkbox = document.querySelector(`input[data-key="${key}"][type="checkbox"]`);
        if (checkbox && checkbox.checked !== ctrl.value) {
            checkbox.checked = ctrl.value;
        }

        // Update text
        const text = document.querySelector(`input[data-key="${key}"][type="text"]`);
        if (text && text.value !== ctrl.value) {
            text.value = ctrl.value || "";
        }
    }
}

// ---------------------------------------------------------------------------
// Send control value
// ---------------------------------------------------------------------------

async function sendControl(moduleName, controlName, value) {
    try {
        await fetch("/api/control", {
            method: "POST",
            headers: {"Content-Type": "application/json"},
            body: JSON.stringify({module: moduleName, control: controlName, value: value})
        });
    } catch {
        // ignore — server may be busy
    }
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------

init();
