// 3D WebGL preview — renders the light pipeline's output as an orbit-able point
// cloud. Extracted from app.js as a self-contained module (same pattern as
// install-picker.js): app.js wires it at three points only —
//   preview.init()            once, after the canvas exists
//   preview.setupLayout()     once, for docked-split ↔ floating-PiP responsiveness
//   preview.onBinaryMessage(buf)  per WebSocket binary frame
// It owns its own GL context, camera, and geometry; it talks to the rest of the
// app only through the DOM (#preview canvas, --bg-0 theme colour) and
// localStorage (mm_cam). No app.js state crosses the boundary.

let gl = null;
let glProgram = null;
let glBuffer = null;
let glLocs = null;          // cached attrib/uniform locations
let glLoopRunning = false;  // continuous rAF render loop active
// Parse the persisted camera, tolerating a malformed/corrupt value so a bad
// localStorage entry can't throw during module init. Falls back to defaults.
const _cam = (() => {
    try {
        const c = JSON.parse(localStorage.getItem("mm_cam") || "null");
        if (c && typeof c.t === "number" && typeof c.p === "number" && typeof c.d === "number") return c;
    } catch { /* corrupt value — ignore, use defaults */ }
    return null;
})();
let camTheta    = _cam ? _cam.t : Math.PI;
let camPhi      = _cam ? _cam.p : 0.4;
let camDist     = _cam ? _cam.d : 2.5;
let camAutoFit  = !_cam;   // fit on first frame when no saved position
function saveCam() { localStorage.setItem("mm_cam", JSON.stringify({t: camTheta, p: camPhi, d: camDist})); }
let lastVerts = null;        // cached vertex array for orbit-without-server-frame
let lastVertCount = 0;
let lastMaxDim = 1;
let vertsBuf = null;         // reused worst-case Float32Array; grows but never shrinks
// True-shape preview geometry, set from the 0x03 coordinate table and reused
// across 0x02 colour frames (positions change only on a layout/LUT rebuild).
let previewCoords_ = null;   // Float32Array[count*3], normalised + box-centred positions
let previewCoordCount_ = 0;
let previewMaxDim_ = 1;
let previewBox_ = null;      // {x,y,z} bounding-box extent for camera auto-fit

function initWebGL() {
    const canvas = document.getElementById("preview");
    if (!canvas) return;
    gl = canvas.getContext("webgl", {alpha: false});
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
            if (d > 0.5) discard;
            float a = 1.0 - smoothstep(0.25, 0.5, d);
            // Gamma 0.7 lifts mid-greys so dim effects stay readable in the preview; not sRGB-correct
            vec3 bright = pow(vCol, vec3(0.7));
            gl_FragColor = vec4(bright * a, a);
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
    glLocs = {
        aPos:      gl.getAttribLocation(glProgram,  "aPos"),
        aCol:      gl.getAttribLocation(glProgram,  "aCol"),
        uMVP:      gl.getUniformLocation(glProgram, "uMVP"),
        uPointSize:gl.getUniformLocation(glProgram, "uPointSize"),
    };
    gl.enable(gl.DEPTH_TEST);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);

    // Orbit controls (mouse + touch)
    let dragging = false, lastX = 0, lastY = 0;
    canvas.addEventListener("mousedown", (e) => { dragging = true; lastX = e.clientX; lastY = e.clientY; });
    canvas.addEventListener("mousemove", (e) => {
        if (!dragging) return;
        camTheta += (e.clientX - lastX) * 0.01;
        camPhi = Math.max(-1.5, Math.min(1.5, camPhi - (e.clientY - lastY) * 0.01));
        lastX = e.clientX; lastY = e.clientY;
        redrawCached();
    });
    canvas.addEventListener("mouseup",    () => { dragging = false; saveCam(); });
    canvas.addEventListener("mouseleave", () => { dragging = false; saveCam(); });
    canvas.addEventListener("wheel", (e) => {
        camDist = Math.max(0.5, Math.min(10, camDist + e.deltaY * 0.005));
        e.preventDefault();
        redrawCached();
        saveCam();
    }, {passive: false});

    // Touch: single-finger orbit, two-finger pinch zoom. touch-action: none on
    // #preview keeps the browser's own scroll/zoom from firing first.
    let pinchDist = 0;
    const touchDistance = (a, b) => Math.hypot(a.clientX - b.clientX, a.clientY - b.clientY);

    canvas.addEventListener("touchstart", (e) => {
        if (e.touches.length === 1) {
            dragging = true;
            lastX = e.touches[0].clientX; lastY = e.touches[0].clientY;
        } else if (e.touches.length >= 2) {
            dragging = false;  // hand off from orbit to pinch
            pinchDist = touchDistance(e.touches[0], e.touches[1]);
            e.preventDefault();
        }
    }, {passive: false});
    canvas.addEventListener("touchmove", (e) => {
        if (e.touches.length === 1 && dragging) {
            const t = e.touches[0];
            camTheta += (t.clientX - lastX) * 0.01;
            camPhi = Math.max(-1.5, Math.min(1.5, camPhi - (t.clientY - lastY) * 0.01));
            lastX = t.clientX; lastY = t.clientY;
            redrawCached();
            e.preventDefault();
        } else if (e.touches.length >= 2 && pinchDist > 0) {
            // Pinch zoom: ratio of finger-distance change scales camDist
            // (fingers apart → zoom in / camDist down). Guard against the
            // degenerate case where both fingers report identical coords
            // (d === 0): division would produce Infinity and snap camDist
            // to its clamp boundary. Skip the update and let the next move
            // produce a sane d.
            const d = touchDistance(e.touches[0], e.touches[1]);
            if (d > 0) {
                const ratio = pinchDist / d;
                camDist = Math.max(0.5, Math.min(10, camDist * ratio));
                pinchDist = d;
                redrawCached();
            }
            e.preventDefault();
        }
    }, {passive: false});
    canvas.addEventListener("touchend", (e) => {
        if (e.touches.length === 0) { dragging = false; pinchDist = 0; saveCam(); }
        // 2→1 touches: stay in pinch (let user finish lifting); pinchDist stays
        // valid for the remaining finger? No — drop pinch, but don't start
        // orbit either, to avoid a jump when one finger lifts.
        else if (e.touches.length === 1) { pinchDist = 0; dragging = false; saveCam(); }
    });

}

// Responsive layout: docked split-pane on wide screens, a draggable floating
// picture-in-picture on narrow screens (or when the user pops the preview out).
// One canvas throughout — only the wrapper's class/position change, so the WebGL
// context is never lost. Width drives the default mode; a manual toggle overrides.
const PIP_BELOW = 960;           // px: auto-PiP under this width
const LS_KEY = "projectMM.preview.v1";   // {corner, dismissed, forcePip}

// Hostile-storage guards (a 3-line idiom shared with the rest of the UI; localStorage
// throws in private mode / when disabled, and may hold a hand-edited non-JSON value).
function loadPrefs() {
    try { return JSON.parse(localStorage.getItem(LS_KEY) || "{}") || {}; }
    catch (_) { return {}; }
}
function savePrefs(p) {
    try { localStorage.setItem(LS_KEY, JSON.stringify(p)); } catch (_) { /* ignore */ }
}

function setupLayout() {
    const ws = document.querySelector(".workspace");
    const pane = document.querySelector(".preview-pane");
    const bar = document.querySelector(".preview-bar");
    const canvas = document.getElementById("preview");
    if (!ws || !pane || !bar || !canvas) return;

    const prefs = loadPrefs();
    let forcePip = !!prefs.forcePip;          // user popped the preview out on a wide screen
    let dismissed = !!prefs.dismissed;        // user hid the PiP entirely
    let corner = prefs.corner || "br";        // tl | tr | bl | br

    const refit = () => { if (lastVerts) redrawCached(); };

    // Place the PiP at its snapped corner (left/top so dragging can move it freely).
    function placeCorner() {
        if (!ws.classList.contains("mode-pip")) { pane.style.left = pane.style.top = ""; return; }
        const m = 12, w = pane.offsetWidth, h = pane.offsetHeight;
        const x = corner.includes("l") ? m : window.innerWidth - w - m;
        const y = corner.includes("t") ? 56 : window.innerHeight - h - m;
        pane.style.left = x + "px";
        pane.style.top = y + "px";
        pane.style.right = "auto";
        pane.style.bottom = "auto";
    }

    // Pick the mode from width + the manual override, apply classes, re-fit the canvas.
    function applyMode() {
        const pip = forcePip || window.innerWidth < PIP_BELOW;
        ws.classList.toggle("mode-pip", pip);
        ws.classList.toggle("mode-docked", !pip);
        ws.classList.toggle("preview-hidden", pip && dismissed);
        const showBtn = document.getElementById("preview-show");
        if (showBtn) showBtn.hidden = !(pip && dismissed);
        // The dock button means "pop out" when docked, "re-dock" when floating.
        const dockBtn = document.getElementById("preview-dock");
        if (dockBtn) dockBtn.textContent = pip ? "⤡" : "⤢";
        requestAnimationFrame(() => { placeCorner(); refit(); });
    }

    // matchMedia would only catch the breakpoint crossing; a resize listener also keeps
    // the PiP pinned to its corner as the window changes. rAF-throttled.
    let ticking = false;
    window.addEventListener("resize", () => {
        if (ticking) return;
        ticking = true;
        requestAnimationFrame(() => { ticking = false; applyMode(); });
    });

    // Dock / pop-out toggle.
    document.getElementById("preview-dock")?.addEventListener("click", () => {
        forcePip = !ws.classList.contains("mode-pip") ? true : false;
        savePrefs({ corner, dismissed, forcePip });
        applyMode();
    });
    // Hide the PiP; reveal the re-show pill.
    document.getElementById("preview-close")?.addEventListener("click", () => {
        dismissed = true;
        savePrefs({ corner, dismissed, forcePip });
        applyMode();
    });
    document.getElementById("preview-show")?.addEventListener("click", () => {
        dismissed = false;
        savePrefs({ corner, dismissed, forcePip });
        applyMode();
    });

    // Drag the PiP by its bar; snap to the nearest corner on release. Pointer events
    // on the BAR only (the canvas keeps its own orbit handler, untouched).
    let drag = null;
    bar.addEventListener("pointerdown", (e) => {
        if (!ws.classList.contains("mode-pip")) return;          // bar inert when docked
        if (e.target.closest(".preview-bar-btn")) return;        // let buttons click
        const r = pane.getBoundingClientRect();
        drag = { dx: e.clientX - r.left, dy: e.clientY - r.top };
        pane.classList.add("dragging");
        bar.setPointerCapture(e.pointerId);
        e.preventDefault();
    });
    bar.addEventListener("pointermove", (e) => {
        if (!drag) return;
        const w = pane.offsetWidth, h = pane.offsetHeight;
        let x = e.clientX - drag.dx, y = e.clientY - drag.dy;
        x = Math.max(0, Math.min(window.innerWidth - w, x));     // clamp to viewport
        y = Math.max(44, Math.min(window.innerHeight - h, y));
        pane.style.left = x + "px";
        pane.style.top = y + "px";
        pane.style.right = pane.style.bottom = "auto";
    });
    bar.addEventListener("pointerup", (e) => {
        if (!drag) return;
        drag = null;
        pane.classList.remove("dragging");
        try { bar.releasePointerCapture(e.pointerId); } catch (_) { /* ignore */ }
        // Snap to nearest corner by the pane's center.
        const r = pane.getBoundingClientRect();
        const cx = r.left + r.width / 2, cy = r.top + r.height / 2;
        corner = (cy < window.innerHeight / 2 ? "t" : "b") + (cx < window.innerWidth / 2 ? "l" : "r");
        savePrefs({ corner, dismissed, forcePip });
        placeCorner();
    });

    applyMode();
}

// True-shape preview: two binary message types on the preview WebSocket.
//   0x03 coordinate table (once per layout/LUT rebuild + ~1 Hz keepalive):
//        [0x03][count:u16][bx:u8][by:u8][bz:u8][stride:u16][(x,y,z):u8×3 × count]
//        Stores the real lights' normalised positions in previewCoords_ (the
//        geometry); per-frame 0x02 messages then just recolour those points.
//   0x02 per-frame channels: [0x02][count:u16][stride:u16][(r,g,b) × count]
//        Colour for light i sits at position previewCoords_[i].
// Light index i in the 0x02 stream matches coordinate-table entry i (both are
// every stride-th driver light, in the same order) — no dense grid, no decompress.
function renderPreviewBinary(buf) {
    if (buf.byteLength < 1) return;
    const view = new DataView(buf);
    const type = view.getUint8(0);
    if (type === 0x03) { parsePreviewCoords(view, buf); return; }
    if (type === 0x02) { renderPreviewFrame(view, buf); return; }
}

// Parse + cache the coordinate table: normalised (x,y,z) per point, centred on
// the bounding box so the cloud sits around the origin like the old grid did.
function parsePreviewCoords(view, buf) {
    if (buf.byteLength < 8) return;
    const count = view.getUint16(1, true);
    const bx = view.getUint8(3), by = view.getUint8(4), bz = view.getUint8(5);
    if (buf.byteLength < 8 + count * 3) return;
    const pos = new Uint8Array(buf, 8);
    const maxDim = Math.max(1, bx, by, bz);
    previewMaxDim_ = maxDim;
    previewCoords_ = new Float32Array(count * 3);
    for (let i = 0; i < count; i++) {
        previewCoords_[i * 3 + 0] = (pos[i * 3 + 0] / maxDim) - 0.5 * bx / maxDim;
        previewCoords_[i * 3 + 1] = (pos[i * 3 + 1] / maxDim) - 0.5 * by / maxDim;
        previewCoords_[i * 3 + 2] = (pos[i * 3 + 2] / maxDim) - 0.5 * bz / maxDim;
    }
    previewCoordCount_ = count;
    previewBox_ = { x: bx, y: by, z: bz };
}

function renderPreviewFrame(view, buf) {
    if (!gl) initWebGL();
    if (!gl) return;
    // Hold frames until positions have arrived (the table is sent on rebuild +
    // ~1 Hz, so a fresh client catches up within a second).
    if (!previewCoords_ || previewCoordCount_ === 0) return;
    if (buf.byteLength < 5) return;
    const count = view.getUint16(1, true);
    if (buf.byteLength < 5 + count * 3) return;
    const rgb = new Uint8Array(buf, 5);
    // RGB[i] colours the light at previewCoords_[i]. If the frame and table
    // counts disagree (a rebuild in flight), plot the overlap only.
    const n = Math.min(count, previewCoordCount_);

    if (!vertsBuf || vertsBuf.length < n * 6) vertsBuf = new Float32Array(n * 6);
    let vi = 0;
    for (let i = 0; i < n; i++) {
        const r = rgb[i * 3], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
        if (!(r | g | b)) continue;          // skip dark points
        vertsBuf[vi++] = previewCoords_[i * 3 + 0];
        vertsBuf[vi++] = previewCoords_[i * 3 + 1];
        vertsBuf[vi++] = previewCoords_[i * 3 + 2];
        vertsBuf[vi++] = r / 255;
        vertsBuf[vi++] = g / 255;
        vertsBuf[vi++] = b / 255;
    }
    const vertCount = vi / 6;

    if (vi === 0) return;  // all-dark frame — keep the last geometry, let rAF idle
    lastVerts = vertsBuf.subarray(0, vi);
    lastVertCount = vertCount;
    lastMaxDim = previewMaxDim_;

    if (camAutoFit && previewBox_) {
        camAutoFit = false;
        const canvas = document.getElementById("preview");
        const fov = 0.8;
        const aspect = canvas ? canvas.clientWidth / Math.max(1, canvas.clientHeight) : 1;
        const bx = previewBox_.x, by = previewBox_.y, bz = previewBox_.z;
        const halfExtent = 0.5 * Math.sqrt(bx * bx + by * by + bz * bz) / previewMaxDim_;
        const fitDist = halfExtent / Math.tan(fov / 2) * (aspect < 1 ? 1 / aspect : 1) * 1.1;
        camDist = Math.max(0.5, Math.min(10, fitDist));
    }

    if (!glLoopRunning) startRenderLoop();
}

function redrawCached() {
    if (!lastVerts) return;
    if (!glLoopRunning) startRenderLoop();
}

// Status-bar "reset preview" button: forget the saved camera and re-fit on the
// next frame. Owns the camera, so the reset lives here, not in app.js.
function resetCamera() {
    localStorage.removeItem("mm_cam");
    camTheta = Math.PI;
    camPhi = 0.4;
    camAutoFit = true;
    if (lastVerts) redrawCached();
}

function startRenderLoop() {
    if (glLoopRunning) return;
    glLoopRunning = true;
    function loop() {
        if (!lastVerts) { glLoopRunning = false; return; }
        drawVerts();
        requestAnimationFrame(loop);
    }
    requestAnimationFrame(loop);
}

function drawVerts() {
    if (!gl || !lastVerts || !glLocs) return;
    const canvas = document.getElementById("preview");
    const cw = Math.round(canvas.clientWidth), ch = Math.round(canvas.clientHeight);
    if (canvas.width !== cw || canvas.height !== ch) {
        canvas.width = cw;
        canvas.height = ch;
    }
    gl.viewport(0, 0, canvas.width, canvas.height);

    const cx = camDist * Math.cos(camPhi) * Math.sin(camTheta);
    const cy = camDist * Math.sin(camPhi);
    const cz = camDist * Math.cos(camPhi) * Math.cos(camTheta);
    const mvp = buildMVP(cx, cy, cz, canvas.width / Math.max(1, canvas.height));

    // alpha:false context — clear to page background colour so the canvas
    // blends seamlessly in both light and dark themes.
    const bg = getComputedStyle(document.documentElement).getPropertyValue("--bg-0").trim();
    const m = bg.match(/^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i);
    if (m) gl.clearColor(parseInt(m[1],16)/255, parseInt(m[2],16)/255, parseInt(m[3],16)/255, 1.0);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

    gl.bindBuffer(gl.ARRAY_BUFFER, glBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, lastVerts, gl.DYNAMIC_DRAW);

    gl.enableVertexAttribArray(glLocs.aPos);
    gl.enableVertexAttribArray(glLocs.aCol);
    gl.vertexAttribPointer(glLocs.aPos, 3, gl.FLOAT, false, 24, 0);
    gl.vertexAttribPointer(glLocs.aCol, 3, gl.FLOAT, false, 24, 12);

    gl.uniformMatrix4fv(glLocs.uMVP, false, mvp);
    const pointSize = Math.max(2, canvas.width * 0.8 / lastMaxDim);
    gl.uniform1f(glLocs.uPointSize, pointSize);

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

// Public surface — the only entry points app.js touches.
export const preview = {
    init: initWebGL,
    setupLayout: setupLayout,
    onBinaryMessage: renderPreviewBinary,
    resetCamera: resetCamera,
};
