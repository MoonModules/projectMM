// projectMM v3 — MoonModule-driven UI

let state = null;
let selectedModule = null;
let interacting = false;
let treeBuilt = false;

async function fetchState() {
    if (interacting) return;
    try {
        const resp = await fetch("/api/state");
        state = await resp.json();
        if (!treeBuilt) {
            renderTree();
            treeBuilt = true;
        } else {
            updateTree();
        }
        if (selectedModule) renderDetail();
    } catch (e) {}
}

function renderTree() {
    const tree = document.getElementById("module-tree");
    tree.innerHTML = "";

    if (!state) return;

    // Layer 0 section
    if (state.layers && state.effects) {
        const label = document.createElement("div");
        label.className = "section-label";
        label.textContent = "Layer 0";
        tree.appendChild(label);

        // Effect dropdown
        const layer = state.layers[0];
        const wrapper = document.createElement("div");
        wrapper.className = "layer-select";
        wrapper.innerHTML = '<span class="layer-label">Effect:</span>';
        const select = document.createElement("select");
        select.className = "effect-select";
        select.id = "effect-select-0";
        for (const effect of state.effects) {
            const opt = document.createElement("option");
            opt.value = effect.index;
            opt.textContent = effect.name;
            if (layer.effects && layer.effects.includes(effect.name)) opt.selected = true;
            select.appendChild(opt);
        }
        select.addEventListener("change", async () => {
            interacting = true;
            await fetch(`/api/effect/0/${select.value}`, { method: "POST" });
            interacting = false;
            treeBuilt = false;
            setTimeout(fetchState, 100);
        });
        wrapper.appendChild(select);
        tree.appendChild(wrapper);

        // Active modifiers display
        const modDisplay = document.createElement("div");
        modDisplay.className = "layer-label";
        modDisplay.id = "active-modifiers";
        modDisplay.style.padding = "4px 10px";
        if (layer.modifiers && layer.modifiers.length > 0) {
            modDisplay.textContent = "Active: " + layer.modifiers.join(" \u2192 ");
        }
        tree.appendChild(modDisplay);

        // Add modifier buttons
        if (state.modifiers) {
            const modWrapper = document.createElement("div");
            modWrapper.className = "layer-select";
            modWrapper.innerHTML = '<span class="layer-label">Add mod:</span>';
            for (const mod of state.modifiers) {
                const btn = document.createElement("button");
                btn.className = "mod-btn";
                btn.textContent = "+" + mod.name;
                btn.addEventListener("click", async () => {
                    await fetch(`/api/modifier/add/0/${mod.index}`, { method: "POST" });
                    treeBuilt = false;
                    setTimeout(fetchState, 100);
                });
                modWrapper.appendChild(btn);
            }
            const clearBtn = document.createElement("button");
            clearBtn.className = "mod-btn clear";
            clearBtn.textContent = "Clear";
            clearBtn.addEventListener("click", async () => {
                await fetch("/api/modifier/remove/0", { method: "POST" });
                treeBuilt = false;
                setTimeout(fetchState, 100);
            });
            modWrapper.appendChild(clearBtn);
            tree.appendChild(modWrapper);
        }
    }

    renderModuleSection(tree, "Effects", state.effects, "effect");
    renderModuleSection(tree, "Modifiers", state.modifiers, "modifier");
    renderModuleSection(tree, "Layouts", state.layouts, "layout");
    renderModuleSection(tree, "Drivers", state.drivers, "driver");
}

function updateTree() {
    // Update only dynamic parts without rebuilding DOM
    if (!state || !state.layers) return;
    const layer = state.layers[0];

    // Update active modifiers text
    const modDisplay = document.getElementById("active-modifiers");
    if (modDisplay) {
        modDisplay.textContent = (layer.modifiers && layer.modifiers.length > 0)
            ? "Active: " + layer.modifiers.join(" \u2192 ")
            : "";
    }
}

function renderModuleSection(tree, title, modules, type) {
    if (!modules || modules.length === 0) return;

    const label = document.createElement("div");
    label.className = "section-label";
    label.textContent = title;
    tree.appendChild(label);

    for (const mod of modules) {
        const item = document.createElement("div");
        item.className = `module-item ${type}`;
        if (selectedModule && selectedModule.type === type &&
            selectedModule.index === mod.index) {
            item.classList.add("active");
        }
        item.textContent = mod.name;
        item.addEventListener("click", () => {
            selectedModule = { type, index: mod.index };
            treeBuilt = false;
            renderTree();
            renderDetail();
        });
        tree.appendChild(item);
    }
}

function renderDetail() {
    const detail = document.getElementById("module-detail");

    if (!selectedModule || !state) {
        detail.innerHTML = '<p class="placeholder">Select a module to view its controls.</p>';
        return;
    }

    const allModules = {
        effect: state.effects,
        modifier: state.modifiers,
        layout: state.layouts,
        driver: state.drivers,
    };

    const mods = allModules[selectedModule.type];
    const mod = mods ? mods.find(m => m.index === selectedModule.index) : null;

    if (!mod) {
        detail.innerHTML = '<p class="placeholder">Module not found.</p>';
        return;
    }

    let html = `<h2>${mod.name}</h2>`;

    if (mod.controls.length === 0) {
        html += '<p class="placeholder">No controls.</p>';
    }

    for (let i = 0; i < mod.controls.length; i++) {
        const ctrl = mod.controls[i];
        const apiPath = `${selectedModule.type}/${selectedModule.index}`;
        html += `<div class="control-group">`;
        html += `<label>${ctrl.name}</label>`;

        if (ctrl.type === 0) {
            html += `<input type="range" min="${ctrl.min}" max="${ctrl.max}" value="${ctrl.value}" `;
            html += `data-path="${apiPath}" data-control="${i}" `;
            html += `onmousedown="interacting=true" onmouseup="sliderDone(this)" ontouchstart="interacting=true" ontouchend="sliderDone(this)" `;
            html += `oninput="updateSliderPreview(this)">`;
            html += `<span class="value-display">${ctrl.value}</span>`;
        } else if (ctrl.type === 1) {
            html += `<label class="toggle"><input type="checkbox" ${ctrl.value ? "checked" : ""} `;
            html += `data-path="${apiPath}" data-control="${i}" `;
            html += `onchange="updateBool(this)"> ${ctrl.value ? "on" : "off"}</label>`;
        } else if (ctrl.type === 2) {
            html += `<input type="text" value="${ctrl.value || ''}" `;
            html += `data-path="${apiPath}" data-control="${i}" `;
            html += `onchange="updateText(this)">`;
        }

        html += `</div>`;
    }

    detail.innerHTML = html;
}

function updateSliderPreview(el) {
    el.nextElementSibling.textContent = el.value;
}

async function sliderDone(el) {
    interacting = false;
    await fetch(`/api/control/${el.dataset.path}/${el.dataset.control}`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ value: parseInt(el.value) })
    });
}

async function updateBool(el) {
    await fetch(`/api/control/${el.dataset.path}/${el.dataset.control}`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ value: el.checked })
    });
}

async function updateText(el) {
    await fetch(`/api/control/${el.dataset.path}/${el.dataset.control}`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ value: el.value })
    });
}

fetchState();
setInterval(fetchState, 1000);
