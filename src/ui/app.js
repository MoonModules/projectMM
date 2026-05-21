// projectMM Web UI

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

    ws.binaryType = "arraybuffer";
    ws.onmessage = (e) => {
        if (e.data instanceof ArrayBuffer) {
            renderPreviewFrame(e.data);
            return;
        }
        try {
            const data = JSON.parse(e.data);
            state = data;
            updateValues();
            updateDeviceName();
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
        updateDeviceName();
        renderNav();
        if (state.modules && state.modules.length > 0) {
            // Restore the previously-selected module if it still exists; otherwise the first.
            const saved = localStorage.getItem("mm.selectedModule");
            const exists = saved && state.modules.some(m => m.name === saved);
            selectModule(exists ? saved : state.modules[0].name);
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
    localStorage.setItem("mm.selectedModule", name);
    renderNav();
    renderCards();
}

function updateDeviceName() {
    if (!state || !state.modules) return;
    const sys = state.modules.find(m => m.name === "System");
    if (!sys || !sys.controls) return;
    const nameCtrl = sys.controls.find(c => c.name === "deviceName");
    if (!nameCtrl || !nameCtrl.value) return;
    const deviceName = nameCtrl.value;
    document.title = deviceName + " — projectMM";
    const topBar = document.getElementById("device-name");
    if (topBar) topBar.textContent = deviceName;
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

    const enabledToggle = document.createElement("input");
    enabledToggle.type = "checkbox";
    enabledToggle.checked = mod.enabled !== false;
    enabledToggle.className = "module-enabled";
    enabledToggle.addEventListener("change", () => {
        sendControl(mod.name, "enabled", enabledToggle.checked);
    });
    title.appendChild(enabledToggle);

    const nameSpan = document.createElement("span");
    nameSpan.textContent = mod.name;
    title.appendChild(nameSpan);

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

    if (ctrl.type === "uint8") {
        const input = document.createElement("input");
        input.type = "range";
        input.min = ctrl.min !== undefined ? ctrl.min : 0;
        input.max = ctrl.max !== undefined ? ctrl.max : 255;
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

    } else if (ctrl.type === "uint16") {
        const input = document.createElement("input");
        input.type = "number";
        input.min = ctrl.min !== undefined ? ctrl.min : 0;
        input.max = ctrl.max !== undefined ? ctrl.max : 65535;
        input.value = ctrl.value;
        input.dataset.key = key;

        input.addEventListener("input", () => {
            dragTs[key] = Date.now();
            clearTimeout(dragTimers[key]);
            dragTimers[key] = setTimeout(() => {
                sendControl(moduleName, ctrl.name, parseInt(input.value));
            }, 500);
        });

        row.appendChild(input);

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

    } else if (ctrl.type === "display") {
        const span = document.createElement("span");
        span.className = "control-display";
        span.textContent = ctrl.value || "";
        span.dataset.key = key;
        row.appendChild(span);

    } else if (ctrl.type === "select") {
        const select = document.createElement("select");
        select.dataset.key = key;
        if (ctrl.options) {
            for (let o = 0; o < ctrl.options.length; o++) {
                const opt = document.createElement("option");
                opt.value = o;
                opt.textContent = ctrl.options[o];
                if (o === ctrl.value) opt.selected = true;
                select.appendChild(opt);
            }
        }
        select.addEventListener("change", async () => {
            await sendControl(moduleName, ctrl.name, parseInt(select.value));
            // Server rebuilds controls (dynamic onBuildControls) — re-fetch and re-render
            setTimeout(async () => {
                const resp = await fetch("/api/state");
                state = await resp.json();
                renderCards();
            }, 200);
        });
        row.appendChild(select);

    } else if (ctrl.type === "progress") {
        const bar = document.createElement("progress");
        bar.max = ctrl.total || 100;
        bar.value = ctrl.value || 0;
        bar.dataset.key = key;
        const pct = ctrl.total > 0 ? Math.round(ctrl.value * 100 / ctrl.total) : 0;
        const valSpan = document.createElement("span");
        valSpan.className = "control-value";
        valSpan.textContent = `${Math.round(ctrl.value/1024)}KB / ${Math.round(ctrl.total/1024)}KB (${pct}%)`;
        valSpan.dataset.key = key + ".label";
        row.appendChild(bar);
        row.appendChild(valSpan);
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

        // Update number input
        const numInput = document.querySelector(`input[data-key="${key}"][type="number"]`);
        if (numInput && parseInt(numInput.value) !== ctrl.value) {
            numInput.value = ctrl.value;
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

        // Update display (read-only)
        const display = document.querySelector(`span.control-display[data-key="${key}"]`);
        if (display && display.textContent !== String(ctrl.value || "")) {
            display.textContent = ctrl.value || "";
        }

        // Update select
        const select = document.querySelector(`select[data-key="${key}"]`);
        if (select && parseInt(select.value) !== ctrl.value) {
            select.value = ctrl.value;
        }

        // Update progress
        const bar = document.querySelector(`progress[data-key="${key}"]`);
        if (bar) {
            bar.value = ctrl.value || 0;
            bar.max = ctrl.total || 100;
            const lbl = document.querySelector(`span[data-key="${key}.label"]`);
            if (lbl) {
                const pct = ctrl.total > 0 ? Math.round(ctrl.value * 100 / ctrl.total) : 0;
                lbl.textContent = `${Math.round(ctrl.value/1024)}KB / ${Math.round(ctrl.total/1024)}KB (${pct}%)`;
            }
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
// 3D WebGL Preview
// ---------------------------------------------------------------------------

let gl = null;
let glProgram = null;
let glBuffer = null;
let camTheta = 0.5, camPhi = 0.4, camDist = 2.5;

function initWebGL() {
    const canvas = document.getElementById("preview");
    if (!canvas) return;
    gl = canvas.getContext("webgl");
    if (!gl) return;

    // Shaders
    const vsrc = `
        attribute vec3 aPos;
        attribute vec3 aCol;
        varying vec3 vCol;
        uniform mat4 uMVP;
        uniform float uPointSize;
        void main() {
            vCol = aCol;
            gl_Position = uMVP * vec4(aPos, 1.0);
            gl_PointSize = uPointSize;
        }
    `;
    const fsrc = `
        precision mediump float;
        varying vec3 vCol;
        void main() {
            float d = length(gl_PointCoord - vec2(0.5));
            if (d > 0.5) discard;
            float alpha = smoothstep(0.5, 0.35, d);
            gl_FragColor = vec4(vCol, alpha);
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

    // Mouse orbit controls
    const canvas2 = document.getElementById("preview");
    let dragging = false, lastX = 0, lastY = 0;
    canvas2.addEventListener("mousedown", (e) => { dragging = true; lastX = e.clientX; lastY = e.clientY; });
    canvas2.addEventListener("mousemove", (e) => {
        if (!dragging) return;
        camTheta += (e.clientX - lastX) * 0.01;
        camPhi = Math.max(-1.5, Math.min(1.5, camPhi + (e.clientY - lastY) * 0.01));
        lastX = e.clientX; lastY = e.clientY;
    });
    canvas2.addEventListener("mouseup", () => { dragging = false; });
    canvas2.addEventListener("mouseleave", () => { dragging = false; });
    canvas2.addEventListener("wheel", (e) => {
        camDist = Math.max(0.5, Math.min(10, camDist + e.deltaY * 0.005));
        e.preventDefault();
    }, {passive: false});
}

function renderPreviewFrame(buf) {
    if (!gl) initWebGL();
    if (!gl) return;

    if (buf.byteLength < 7) return;
    const view = new DataView(buf);
    if (view.getUint8(0) !== 0x02) return;
    const w = view.getUint16(1, true);
    const h = view.getUint16(3, true);
    const d = view.getUint16(5, true);
    if (w === 0 || h === 0 || d === 0) return;
    const count = w * h * d;
    if (buf.byteLength < 7 + count * 3) return;
    const pixels = new Uint8Array(buf, 7);

    // Build vertex data: [x,y,z,r,g,b] per point
    const maxDim = Math.max(w, h, d);
    const verts = new Float32Array(count * 6);
    let vi = 0;
    for (let iz = 0; iz < d; iz++) {
        for (let iy = 0; iy < h; iy++) {
            for (let ix = 0; ix < w; ix++) {
                const idx = (iz * h * w + iy * w + ix) * 3;
                verts[vi++] = (ix / maxDim) - 0.5 * w / maxDim;
                verts[vi++] = (iy / maxDim) - 0.5 * h / maxDim;
                verts[vi++] = (iz / maxDim) - 0.5 * d / maxDim;
                verts[vi++] = pixels[idx] / 255;
                verts[vi++] = pixels[idx + 1] / 255;
                verts[vi++] = pixels[idx + 2] / 255;
            }
        }
    }

    // Resize canvas to CSS size
    const canvas = document.getElementById("preview");
    canvas.width = canvas.clientWidth;
    canvas.height = canvas.clientHeight;
    gl.viewport(0, 0, canvas.width, canvas.height);

    // Camera
    const cx = camDist * Math.cos(camPhi) * Math.sin(camTheta);
    const cy = camDist * Math.sin(camPhi);
    const cz = camDist * Math.cos(camPhi) * Math.cos(camTheta);
    const mvp = buildMVP(cx, cy, cz, canvas.width / canvas.height);

    gl.clearColor(0.05, 0.07, 0.09, 1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

    gl.bindBuffer(gl.ARRAY_BUFFER, glBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, verts, gl.DYNAMIC_DRAW);

    const aPos = gl.getAttribLocation(glProgram, "aPos");
    const aCol = gl.getAttribLocation(glProgram, "aCol");
    gl.enableVertexAttribArray(aPos);
    gl.enableVertexAttribArray(aCol);
    gl.vertexAttribPointer(aPos, 3, gl.FLOAT, false, 24, 0);
    gl.vertexAttribPointer(aCol, 3, gl.FLOAT, false, 24, 12);

    const uMVP = gl.getUniformLocation(glProgram, "uMVP");
    gl.uniformMatrix4fv(uMVP, false, mvp);

    const pointSize = Math.max(2, canvas.width * 0.5 / maxDim);
    gl.uniform1f(gl.getUniformLocation(glProgram, "uPointSize"), pointSize);

    gl.drawArrays(gl.POINTS, 0, count);
}

function buildMVP(ex, ey, ez, aspect) {
    // View matrix (look-at from eye to origin)
    const fLen = Math.sqrt(ex*ex + ey*ey + ez*ez);
    const fx = -ex/fLen, fy = -ey/fLen, fz = -ez/fLen;
    // Right = normalize(forward x up)
    let rx = fy*0 - fz*0, ry = fz*0 - fx*0, rz = fx*0 - fy*0;
    // Use (0,1,0) as up
    rx = fy*0 - fz*1; ry = fz*0 - fx*0; rz = fx*1 - fy*0;
    // Actually: right = cross(forward, (0,1,0))
    rx = fz; ry = 0; rz = -fx;
    let rLen = Math.sqrt(rx*rx + ry*ry + rz*rz) || 1;
    rx /= rLen; ry /= rLen; rz /= rLen;
    // Up = cross(right, forward)
    const ux = ry*fz - rz*fy, uy = rz*fx - rx*fz, uz = rx*fy - ry*fx;

    // View matrix (column-major)
    const view = [
        rx, ux, -fx, 0,
        ry, uy, -fy, 0,
        rz, uz, -fz, 0,
        -(rx*ex+ry*ey+rz*ez), -(ux*ex+uy*ey+uz*ez), (fx*ex+fy*ey+fz*ez), 1
    ];

    // Perspective matrix
    const near = 0.1, far = 50, fov = 0.8;
    const f = 1 / Math.tan(fov / 2);
    const proj = [
        f/aspect, 0, 0, 0,
        0, f, 0, 0,
        0, 0, (far+near)/(near-far), -1,
        0, 0, 2*far*near/(near-far), 0
    ];

    // Multiply proj * view
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

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------

init();
