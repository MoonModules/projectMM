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
let glMaxPointSize = 64;    // driver's gl_PointSize cap (ALIASED_POINT_SIZE_RANGE max)
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
// Camera-distance clamp. The scene is normalised to ~[-0.5, 0.5] (box-centred), so CAM_MIN
// well below the scene radius lets you zoom DEEP into a dense grid — close enough that a
// single 128²-grid cell fills the view and its sequence number fits the bulb (the projection
// near plane is lowered to match, so the scene doesn't clip as you approach). CAM_MAX frames
// the whole volume with headroom.
const CAM_MIN = 0.03, CAM_MAX = 10;
let camTheta    = _cam ? _cam.t : Math.PI;
let camPhi      = _cam ? _cam.p : 0.4;
let camDist     = _cam ? _cam.d : 2.5;
// The point the camera orbits + looks at. Origin by default (the scene is box-centred);
// cursor-anchored zoom pans it so the world point under the pointer stays put (Google-Maps
// style). Persisted with the angles/distance so a reload keeps the framing.
let camTgtX     = _cam ? (_cam.tx || 0) : 0;
let camTgtY     = _cam ? (_cam.ty || 0) : 0;
let camTgtZ     = _cam ? (_cam.tz || 0) : 0;
let camAutoFit  = !_cam;   // fit on first frame when no saved position
function saveCam() { localStorage.setItem("mm_cam", JSON.stringify({t: camTheta, p: camPhi, d: camDist, tx: camTgtX, ty: camTgtY, tz: camTgtZ})); }
let lastVerts = null;        // cached vertex array for orbit-without-server-frame
let lastVertCount = 0;
let lastMaxDim = 1;
let vertsBuf = null;         // reused worst-case Float32Array; grows but never shrinks
// True-shape preview geometry, set from the 0x03 coordinate table and reused
// across 0x02 colour frames (positions change only on a layout/LUT rebuild).
let previewCoords_ = null;   // Float32Array[count*3], normalised + box-centred positions
let previewCoordCount_ = 0;
let previewMaxDim_ = 1;
let previewStride_ = 1;      // device's adaptive downscale factor (1 = full res); for the status line
let showSeqNumbers_ = false; // sequence-number overlay toggle (preview-numbers button)
// Dot-size multiplier on the auto-computed "filled-panel" base (1 = ¾-fill). A user knob
// because the ideal fill is subjective and layout-dependent — a 2D panel reads best solid,
// a 3D cube reads best with smaller dots so the back layers show through. Persisted.
const DOT_MIN = 0.25, DOT_MAX = 1.5;   // matches the slider range; clamp so a bad
const clampDot = (v) => Math.min(DOT_MAX, Math.max(DOT_MIN, Number.isFinite(v) ? v : 1));
// localStorage value (or a manual edit) can't push the dot size to a performance-killing extreme.
let dotScale_ = clampDot(parseFloat(localStorage.getItem("mm_preview_dot")));
let resetLayout_ = null;     // set by setupLayout(): restores docked/PiP state to defaults
let previewBox_ = null;      // {x,y,z} bounding-box extent for camera auto-fit
let lineProgram = null;      // separate program for the wireframe bounding box
let lineLocs = null;
let lineBuffer = null;
let boxVerts = null;         // 12-edge wireframe (24 line endpoints) for the current box
let boxKey = "";             // cache key so the box buffer rebuilds only when extents change

function initWebGL() {
    const canvas = document.getElementById("preview");
    if (!canvas) return;
    gl = canvas.getContext("webgl", {alpha: false});
    if (!gl) return;

    const vsrc = `
        attribute vec3 aPos;
        attribute vec3 aCol;
        varying vec3 vCol;
        varying float vSize;
        uniform mat4 uMVP;
        uniform float uPointSize;
        void main() {
            vCol = aCol;
            gl_Position = uMVP * vec4(aPos, 1.0);
            // Depth-corrected point size — closer LEDs render larger
            gl_PointSize = uPointSize / gl_Position.w;
            vSize = gl_PointSize;   // px size, so the fragment can keep the AA edge ~1px wide
        }
    `;
    const fsrc = `
        precision mediump float;
        varying vec3 vCol;
        varying float vSize;
        uniform float uRingFade;   // 0..1: off-LED placeholder opacity. 1 at small/zoomed
                                   // grids (placeholders show the layout); →0 when points get
                                   // dense (they'd be noise, so the lit pattern reads cleanly).
        uniform float uLitPass;    // two-pass draw: 0 = placeholders only, 1 = lit LEDs only.
                                   // Lit are drawn second with depth-test off so they always
                                   // layer ABOVE the grey placeholders at the same spot,
                                   // regardless of pan/tilt draw order (no z-fighting).
        void main() {
            float d = length(gl_PointCoord - vec2(0.5));   // 0 at center .. 0.5 at rim
            // Anti-alias band ~1px wide regardless of sprite size: crisp disc at 8x8
            // (huge sprites) AND smooth at large grids (tiny sprites).
            float aa = clamp(1.0 / max(vSize, 1.0), 0.004, 0.12);
            float disc = 1.0 - smoothstep(0.5 - aa, 0.5, d);   // filled circle, thin soft rim
            // Gamma 0.7 lifts mid-greys so dim effects stay readable; not sRGB-correct.
            vec3 bright = pow(vCol, vec3(0.7));
            float lum = max(max(vCol.r, vCol.g), vCol.b);
            // How "lit" the LED is, ramped over the bottom of the range so a near-off LED is
            // a placeholder but a genuinely lit one is solid.
            float lit = smoothstep(0.02, 0.10, lum);

            if (uLitPass < 0.5) {
                // Pass 1 — placeholders for OFF LEDs only. A faint filled grey CIRCLE (a disc,
                // not a hollow ring or square) so irregular layouts (a wheel, a sphere) read
                // cleanly; fades out as the grid gets dense (uRingFade→0).
                if (lit > 0.5) discard;                       // lit LEDs belong to pass 2
                float a = disc * 0.22 * uRingFade;
                if (a < 0.01) discard;
                gl_FragColor = vec4(vec3(0.32), a);
            } else {
                // Pass 2 — lit LEDs only, solid disc in the real colour, on top.
                if (lit < 0.5) discard;                       // off LEDs were pass 1
                if (disc < 0.01) discard;
                gl_FragColor = vec4(bright, disc);
            }
        }
    `;

    const vs = gl.createShader(gl.VERTEX_SHADER);
    gl.shaderSource(vs, vsrc); gl.compileShader(vs);
    const fs = gl.createShader(gl.FRAGMENT_SHADER);
    gl.shaderSource(fs, fsrc); gl.compileShader(fs);
    glProgram = gl.createProgram();
    gl.attachShader(glProgram, vs); gl.attachShader(glProgram, fs);
    gl.linkProgram(glProgram); gl.useProgram(glProgram);

    // WebGL clamps gl_PointSize to the driver's range — bulbs stop growing past this even
    // as you zoom in, so the label fit-check must clamp to the same cap (else it thinks a
    // bulb is big enough for a number when the drawn sprite is actually capped smaller).
    glMaxPointSize = (gl.getParameter(gl.ALIASED_POINT_SIZE_RANGE) || [1, 64])[1] || 64;

    glBuffer = gl.createBuffer();
    glLocs = {
        aPos:      gl.getAttribLocation(glProgram,  "aPos"),
        aCol:      gl.getAttribLocation(glProgram,  "aCol"),
        uMVP:      gl.getUniformLocation(glProgram, "uMVP"),
        uPointSize:gl.getUniformLocation(glProgram, "uPointSize"),
        uRingFade: gl.getUniformLocation(glProgram, "uRingFade"),
        uLitPass:  gl.getUniformLocation(glProgram, "uLitPass"),
    };
    gl.enable(gl.DEPTH_TEST);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);

    // A second, minimal program for the wireframe bounding box (a faint cuboid around
    // the light volume — gives the scene bounds + 3D orientation while orbiting, and a
    // frame even when every LED is off). Flat colour, no per-vertex attributes beyond pos.
    const lvs = `attribute vec3 aPos; uniform mat4 uMVP; void main(){ gl_Position = uMVP * vec4(aPos,1.0); }`;
    const lfs = `precision mediump float; uniform vec4 uColor; void main(){ gl_FragColor = uColor; }`;
    const lv = gl.createShader(gl.VERTEX_SHADER); gl.shaderSource(lv, lvs); gl.compileShader(lv);
    const lf = gl.createShader(gl.FRAGMENT_SHADER); gl.shaderSource(lf, lfs); gl.compileShader(lf);
    lineProgram = gl.createProgram();
    gl.attachShader(lineProgram, lv); gl.attachShader(lineProgram, lf);
    gl.linkProgram(lineProgram);
    lineLocs = {
        aPos:   gl.getAttribLocation(lineProgram, "aPos"),
        uMVP:   gl.getUniformLocation(lineProgram, "uMVP"),
        uColor: gl.getUniformLocation(lineProgram, "uColor"),
    };
    lineBuffer = gl.createBuffer();

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
        e.preventDefault();
        // Cursor-anchored zoom (Google-Maps style): keep the world point under the pointer
        // fixed on screen while zooming. The orbit camera looks at camTgt from camDist; the
        // view half-extent at the target plane is camDist*tan(fov/2). The cursor's offset
        // from canvas centre, in that world scale along the camera's right/up axes, is where
        // the pointer is in the target plane. Scaling camDist by k scales that plane's extent
        // by k, so shifting camTgt by (1-k)*cursorOffset keeps the pointed-at point put.
        const r = canvas.getBoundingClientRect();
        const ndcX = ((e.clientX - r.left) / r.width) * 2 - 1;
        const ndcY = 1 - ((e.clientY - r.top) / r.height) * 2;   // y-up
        const aspect = r.width / Math.max(1, r.height);
        const fov = 0.8;
        const halfH = camDist * Math.tan(fov / 2);
        const offU = ndcY * halfH;             // world units along camera up at target plane
        const offR = ndcX * halfH * aspect;    // along camera right

        const oldDist = camDist;
        camDist = Math.max(CAM_MIN, Math.min(CAM_MAX, camDist * Math.exp(e.deltaY * 0.0015)));
        const k = camDist / oldDist;           // <1 zooming in, >1 zooming out

        // Camera right/up axes (same basis buildMVP derives: right = forward×worldUp, etc.).
        const fx = -Math.cos(camPhi) * Math.sin(camTheta);
        const fy = -Math.sin(camPhi);
        const fz = -Math.cos(camPhi) * Math.cos(camTheta);
        let rx = fz, rz = -fx; const rl = Math.hypot(rx, 0, rz) || 1; rx /= rl; rz /= rl;  // ry=0
        const ux = (-rz)*fy - 0, uy = rz*fx - rx*fz, uz = 0 - (-rx)*fy;  // up = right×forward
        // Shift the target so the cursor-pointed world point stays fixed as the extent scales.
        const s = (1 - k);
        camTgtX += s * (offR * rx + offU * ux);
        camTgtY += s * (offR * 0  + offU * uy);
        camTgtZ += s * (offR * rz + offU * uz);

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
                camDist = Math.max(CAM_MIN, Math.min(CAM_MAX, camDist * ratio));
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

    // Let the preview "reset" button restore the docked/PiP layout to defaults too (back to
    // auto docked-vs-PiP, not dismissed, default corner) — these vars are closure-local here.
    resetLayout_ = () => {
        forcePip = false; dismissed = false; corner = "br";
        savePrefs({ corner, dismissed, forcePip });
        applyMode();
    };

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
    // Hide the preview; reveal the re-show pill. Dismissal only takes visible effect in
    // PiP mode (the pill replaces the floating preview); closing from docked mode also
    // pops it out (forcePip) so the result is immediate — a dismissed docked preview that
    // only vanished later when narrow auto-PiP kicked in would be a confusing surprise.
    document.getElementById("preview-close")?.addEventListener("click", () => {
        dismissed = true;
        if (!ws.classList.contains("mode-pip")) forcePip = true;
        savePrefs({ corner, dismissed, forcePip });
        applyMode();
    });
    document.getElementById("preview-show")?.addEventListener("click", () => {
        dismissed = false;
        forcePip = false;   // bring it back in the width-appropriate mode (docked on wide)
        savePrefs({ corner, dismissed, forcePip });
        applyMode();
    });
    // Sequence-number overlay toggle. The active state reflects the flag; the labels
    // themselves only appear when also legible (few enough on-screen — see drawSeqLabels).
    const numBtn = document.getElementById("preview-numbers");
    numBtn?.addEventListener("click", () => {
        showSeqNumbers_ = !showSeqNumbers_;
        numBtn.classList.toggle("active", showSeqNumbers_);
        if (lastVerts) redrawCached();   // repaint so labels appear/clear immediately
    });

    // Dot-size knob: scales the auto "filled-panel" base. Persisted so the preference sticks.
    const dotSlider = document.getElementById("preview-dot");
    if (dotSlider) {
        dotSlider.value = String(dotScale_);
        dotSlider.addEventListener("input", () => {
            dotScale_ = clampDot(parseFloat(dotSlider.value));
            localStorage.setItem("mm_preview_dot", String(dotScale_));
            if (lastVerts) redrawCached();
        });
    }

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
//        [0x03][count:u32][bx:u8][by:u8][bz:u8][stride:u16][(x,y,z):u8×3 × count]
//        Stores the real lights' normalised positions in previewCoords_ (the
//        geometry); per-frame 0x02 messages then just recolour those points.
//   0x02 per-frame channels: [0x02][count:u32][stride:u16][(r,g,b) × count]
//        Colour for light i sits at position previewCoords_[i].
// count is u32 so a >65535-light panel (HUB75 walls) isn't capped by the wire format.
// Light index i in the 0x02 stream matches coordinate-table entry i (both are
// every stride-th driver light, in the same order) — no dense grid, no decompress.
// Show the device's adaptive-downscale factor in the preview bar — only while it's active
// (factor > 1), so the bar stays clean at full resolution. previewStride_ is the per-axis
// downscale: factor f means ~1/f² of the lights are shown (f per axis on a 2D grid).
function updatePreviewStatus() {
    const el = document.getElementById("preview-status");
    if (!el) return;
    const parts = [];
    if (previewStride_ > 1) parts.push(`1/${previewStride_} · link limited`);   // resolution shed
    if (effectiveFps_ > 0)  parts.push(`${Math.round(effectiveFps_)} fps`);      // adaptive rate
    if (parts.length) {
        el.textContent = "preview " + parts.join(" · ");
        el.hidden = false;
    } else {
        el.hidden = true;
    }
}

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
    // Header: [0x03][count:u32][bx][by][bz][stride:u16] = 10 bytes.
    if (buf.byteLength < 10) return;
    const count = view.getUint32(1, true);
    const bx = view.getUint8(5), by = view.getUint8(6), bz = view.getUint8(7);
    previewStride_ = view.getUint16(8, true) || 1;   // = device's adaptive downscale factor
    updatePreviewStatus();
    if (buf.byteLength < 10 + count * 3) return;
    const pos = new Uint8Array(buf, 10);
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
    // Draw the grid layout NOW, off (placeholder rings), so a fresh page / UI refresh shows the
    // geometry the instant the table arrives — not only once the first colour frame happens to land
    // (which never comes if the scene is paused/idle). Colour frames then light it.
    drawLights(null);
}

function renderPreviewFrame(view, buf) {
    if (!gl) initWebGL();
    if (!gl) return;
    // Hold frames until positions have arrived (the device sends the table on a geometry
    // rebuild and when a new client connects, so a fresh client gets it on connect).
    if (!previewCoords_ || previewCoordCount_ === 0) return;
    // Header: [0x02][count:u32][stride:u16] = 7 bytes.
    if (buf.byteLength < 7) return;
    const count = view.getUint32(1, true);
    const stride = view.getUint16(5, true) || 1;
    if (buf.byteLength < 7 + count * 3) return;
    const rgb = new Uint8Array(buf, 7);
    // RGB[i] colours the light at previewCoords_[i]. The colour frame and the coordinate table
    // MUST describe the same light set — if their count OR stride (downscale factor) disagree, a
    // geometry rebuild (a resize, or the device's adaptive downscale changing the lattice) is
    // mid-flight: the colours would land on the wrong positions (a visibly scrambled frame).
    // Skip such a frame; the matching coord table arrives within ~1 frame and they realign.
    if (count !== previewCoordCount_ || stride !== previewStride_) return;
    drawLights(rgb);
    measureFrameRate();
}

// Effective preview frame rate, measured browser-side as a SLIDING-WINDOW COUNT: how many frames
// arrived in the last second of wall time. The device drains each resumable frame across transport
// ticks, so frames arrive BURSTY (several back-to-back, then a gap) — an instantaneous 1000/Δt
// reading would spike to absurd values on a 0 ms gap (the "200 fps" artifact). Counting over a
// fixed window is immune to that: it's the true delivered rate. performance.now() is the standard
// high-resolution browser clock.
const frameStamps_ = [];        // arrival times (ms) within the trailing window
let effectiveFps_ = 0;
function measureFrameRate() {
    const now = performance.now();
    frameStamps_.push(now);
    const cutoff = now - 1000;                                  // 1-second trailing window
    while (frameStamps_.length && frameStamps_[0] < cutoff) frameStamps_.shift();
    effectiveFps_ = frameStamps_.length;                        // frames in the last second = fps
    updatePreviewStatus();
}

// Build the vertex buffer from previewCoords_ + per-light colour and (re)start the render loop.
// rgb may be null — then every light is drawn off (the shader's placeholder ring), so the grid
// LAYOUT shows the instant the coordinate table arrives (a fresh page / UI refresh), before any
// colour frame. A colour frame then calls this again with its rgb to light the scene.
function drawLights(rgb) {
    if (!gl) initWebGL();
    if (!gl) return;
    if (!previewCoords_ || previewCoordCount_ === 0) return;
    const n = previewCoordCount_;

    if (!vertsBuf || vertsBuf.length < n * 6) vertsBuf = new Float32Array(n * 6);
    let vi = 0;
    for (let i = 0; i < n; i++) {
        // Include EVERY light, dark ones too: the shader draws an off LED as a faint
        // placeholder ring (a lit one as a solid disc), so the grid shape stays visible
        // and an all-off scene shows the layout instead of a black screen. (The count is
        // already bounded by the table's stride downsampling for large grids.)
        vertsBuf[vi++] = previewCoords_[i * 3 + 0];
        vertsBuf[vi++] = previewCoords_[i * 3 + 1];
        vertsBuf[vi++] = previewCoords_[i * 3 + 2];
        vertsBuf[vi++] = rgb ? rgb[i * 3] / 255 : 0;
        vertsBuf[vi++] = rgb ? rgb[i * 3 + 1] / 255 : 0;
        vertsBuf[vi++] = rgb ? rgb[i * 3 + 2] / 255 : 0;
    }
    const vertCount = vi / 6;

    if (vi === 0) return;  // no coords at all — keep the last geometry, let rAF idle
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

// The preview "reset" (⌖) button: restore the WHOLE preview to defaults — camera, dot-size
// slider, sequence-number toggle, and docked/PiP layout. One button clears every preview
// preference (all browser-local; nothing touches the device). Lives here since it owns the
// camera + dot/number state; the layout part defers to setupLayout's resetLayout_ hook.
function resetCamera() {
    // Camera: forget the saved orbit + re-fit on the next frame.
    localStorage.removeItem("mm_cam");
    camTheta = Math.PI;
    camPhi = 0.4;
    camTgtX = camTgtY = camTgtZ = 0;   // recentre the pan target (cursor-zoom resets too)
    camAutoFit = true;

    // Dot size: back to the auto "filled-panel" base (1×); sync the slider control.
    dotScale_ = 1;
    localStorage.removeItem("mm_preview_dot");
    const dotSlider = document.getElementById("preview-dot");
    if (dotSlider) dotSlider.value = "1";

    // Sequence numbers: off; clear the № button's active state.
    showSeqNumbers_ = false;
    document.getElementById("preview-numbers")?.classList.remove("active");

    // Layout: back to auto docked/PiP, not dismissed, default corner.
    if (resetLayout_) resetLayout_();

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

    // Eye orbits the target at camDist; the view looks AT the target (not the origin), so
    // cursor-anchored zoom can pan the target without changing the orbit angles.
    const ex = camTgtX + camDist * Math.cos(camPhi) * Math.sin(camTheta);
    const ey = camTgtY + camDist * Math.sin(camPhi);
    const ez = camTgtZ + camDist * Math.cos(camPhi) * Math.cos(camTheta);
    const mvp = buildMVP(ex, ey, ez, camTgtX, camTgtY, camTgtZ, canvas.width / Math.max(1, canvas.height));

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
    // Size each dot to the spacing between the SAMPLED points so the layout reads as a filled
    // panel (¾ light, ¼ gap) at any size — a big grid is spatially downsampled (the device
    // sends ~1800 lattice points), so sizing by the full dimension left each dot a fraction of
    // its cell with big gaps. The sampled points fill the bounding box uniformly, so the pitch
    // between neighbours (in grid units) is (boxVolume / count)^(1/activeDims): the square root
    // for a flat grid, the CUBE root for a 3D volume (a cube's points spread over depth, so a
    // flat √ undercounts the pitch and the dots come out too small — the 3D-gap bug). Convert
    // that grid pitch to on-screen pixels (canvas px per grid unit) and take 75% of it. The
    // shader depth-corrects per point, so zooming still enlarges the dots beyond this base.
    const bX = previewBox_ ? Math.max(1, previewBox_.x) : 1;
    const bY = previewBox_ ? Math.max(1, previewBox_.y) : 1;
    const bZ = previewBox_ ? Math.max(1, previewBox_.z) : 1;
    const dims = (previewBox_ ? [bX, bY, bZ].filter(d => d > 1).length : 2) || 1;  // active axes
    const volume = (bX > 1 ? bX : 1) * (bY > 1 ? bY : 1) * (bZ > 1 ? bZ : 1);
    const gridPitch = Math.pow(volume / Math.max(1, lastVertCount), 1 / dims);     // grid units
    const pxPerGridUnit = canvas.width / lastMaxDim;
    const pointSize = Math.max(2, gridPitch * pxPerGridUnit * 0.75 * dotScale_);
    gl.uniform1f(glLocs.uPointSize, pointSize);
    // LOD: the off-LED placeholder rings are useful when sprites are big enough to show a
    // hollow rim, but become visual mud once sprites are tiny (a dense grid zoomed out).
    // Fade them by base sprite size — full rings ≥8px, gone ≤4px — so the layout shows on
    // small/zoomed grids and the lit pattern reads cleanly when dense. Lit dots are never
    // faded (their alpha ignores uRingFade in the shader).
    const ringFade = Math.max(0, Math.min(1, (pointSize - 4) / 4));
    gl.uniform1f(glLocs.uRingFade, ringFade);

    // Two passes so lit LEDs always sit ABOVE the grey placeholders (your "lights should layer
    // above the circles"). On a flat grid all LEDs share a z-plane, so a single pass let draw
    // order + z-fighting clip a lit dot behind a neighbour's placeholder. Pass 1 draws the
    // off-LED placeholders and writes depth; pass 2 draws the lit LEDs with depthFunc LEQUAL
    // and depth-WRITE off — so a lit dot beats a co-located placeholder (equal depth passes)
    // yet lit dots still depth-sort against each other in a true 3D cube under any pan/tilt.
    gl.uniform1f(glLocs.uLitPass, 0.0);                 // placeholders (write depth)
    gl.drawArrays(gl.POINTS, 0, lastVertCount);
    gl.depthFunc(gl.LEQUAL);
    gl.depthMask(false);
    gl.uniform1f(glLocs.uLitPass, 1.0);                 // lit LEDs, on top
    gl.drawArrays(gl.POINTS, 0, lastVertCount);
    gl.depthMask(true);
    gl.depthFunc(gl.LESS);

    drawBoundingBox(mvp);
    drawSeqLabels(mvp, canvas, pointSize);
}

// Faint wireframe cuboid around the light volume. Rebuilt only when the box extent
// changes (cached by boxKey). Half-extents are box/2/maxDim — matching the same
// normalisation the point coords use (pos/maxDim - 0.5*box/maxDim), so the cuboid's
// faces pass through the outermost LED centres.
function drawBoundingBox(mvp) {
    if (!lineProgram || !previewBox_ || !previewMaxDim_) return;
    const md = previewMaxDim_;
    const key = previewBox_.x + "x" + previewBox_.y + "x" + previewBox_.z + "@" + md;
    if (key !== boxKey) {
        const hx = (previewBox_.x) / 2 / md, hy = (previewBox_.y) / 2 / md, hz = (previewBox_.z) / 2 / md;
        // 8 corners → 12 edges → 24 endpoints.
        const c = [
            [-hx,-hy,-hz],[ hx,-hy,-hz],[ hx, hy,-hz],[-hx, hy,-hz],
            [-hx,-hy, hz],[ hx,-hy, hz],[ hx, hy, hz],[-hx, hy, hz],
        ];
        const E = [[0,1],[1,2],[2,3],[3,0],[4,5],[5,6],[6,7],[7,4],[0,4],[1,5],[2,6],[3,7]];
        boxVerts = new Float32Array(E.length * 6);
        let k = 0;
        for (const [a, b] of E) { boxVerts.set(c[a], k); k += 3; boxVerts.set(c[b], k); k += 3; }
        boxKey = key;
    }
    gl.useProgram(lineProgram);
    gl.bindBuffer(gl.ARRAY_BUFFER, lineBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, boxVerts, gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(lineLocs.aPos);
    gl.vertexAttribPointer(lineLocs.aPos, 3, gl.FLOAT, false, 0, 0);
    gl.uniformMatrix4fv(lineLocs.uMVP, false, mvp);
    // Faint, theme-neutral grey — visible on both dark and light backgrounds.
    gl.uniform4f(lineLocs.uColor, 0.5, 0.5, 0.55, 0.25);
    gl.drawArrays(gl.LINES, 0, boxVerts.length / 3);
    gl.useProgram(glProgram);   // restore the points program for the next frame
}

// Sequence-number overlay. WebGL point sprites can't draw text, so each light's index is
// rendered onto a 2D canvas laid over #preview: project the light's position through the
// SAME mvp the GL render uses (so labels track LEDs in 2D AND 3D layouts), to a screen
// pixel, and draw its number. Legibility LOD: a number is drawn only if it FITS INSIDE its
// light bulb (the on-screen sprite) — so it never overflows onto neighbours. The font is
// sized to the sprite, so as you zoom in (sprites grow, depth-corrected) more numbers fit
// and appear; zoomed out on a dense grid they don't fit and stay hidden. Behind-camera
// points (w ≤ 0) are skipped — essential for 3D.
function drawSeqLabels(mvp, glCanvas, pointSize) {
    const lc = document.getElementById("preview-labels");
    if (!lc) return;
    // Match the overlay to the GL canvas's on-screen box (the pane also holds the bar
    // above the canvas, so anchor to #preview's rect, not the pane's).
    const gr = glCanvas.getBoundingClientRect();
    const pr = lc.parentElement.getBoundingClientRect();
    lc.style.left = (gr.left - pr.left) + "px";
    lc.style.top = (gr.top - pr.top) + "px";
    lc.style.width = gr.width + "px";
    lc.style.height = gr.height + "px";
    const W = Math.max(1, Math.round(gr.width)), H = Math.max(1, Math.round(gr.height));
    if (lc.width !== W || lc.height !== H) { lc.width = W; lc.height = H; }
    const ctx = lc.getContext("2d");
    ctx.clearRect(0, 0, W, H);

    if (!showSeqNumbers_ || !previewCoords_ || previewCoordCount_ === 0) return;

    // Project every sent point; keep those in front of the camera + inside the viewport.
    // mvp is column-major: clip[r] = Σ_c mvp[c*4+r] * v[c].
    const proj = [];
    for (let i = 0; i < previewCoordCount_; i++) {
        const x = previewCoords_[i*3], y = previewCoords_[i*3+1], z = previewCoords_[i*3+2];
        const cw = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];
        if (cw <= 0) continue;                                   // behind the camera (3D)
        const cx = mvp[0] * x + mvp[4] * y + mvp[8]  * z + mvp[12];
        const cy = mvp[1] * x + mvp[5] * y + mvp[9]  * z + mvp[13];
        const sx = (cx / cw * 0.5 + 0.5) * W;
        const sy = (1 - (cy / cw * 0.5 + 0.5)) * H;              // GL y-up → canvas y-down
        if (sx < 0 || sx > W || sy < 0 || sy > H) continue;      // off-screen
        // The bulb's on-screen diameter, in CSS px. The shader's gl_PointSize = uPointSize/w
        // (same depth correction) is clamped to glMaxPointSize by the driver, so the DRAWN
        // bulb can't exceed that — clamp here too (in backing px) before converting to CSS
        // px (sprite px are backing px; the overlay is CSS px), or the fit-check would
        // believe a bulb is bigger than it renders and labels would never appear at the cap.
        const cssPerBacking = W / glCanvas.width;
        const diam = Math.min(pointSize / cw, glMaxPointSize) * cssPerBacking;
        // Label = the sent-point index. At full resolution (previewStride_==1) that IS the
        // driver light index. When downsampled (>1) the device sends a spatial LATTICE, not
        // every Nth flat light, so the true driver index isn't i*stride — we show the sent
        // index i (monotonic, still useful for orientation) rather than a wrong number.
        proj.push({ n: i, sx, sy, depth: cw, diam });
    }
    if (proj.length === 0) return;

    // Nearest first so a label drawn later (farther) doesn't overwrite a closer one's slot.
    proj.sort((a, b) => a.depth - b.depth);
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    for (const p of proj) {
        const t = String(p.n);
        // "Fits in the bulb or hide": size the font so the number's WIDTH fills ~85% of the
        // bulb (monospace digit ≈ 0.6em wide, so width ≈ digits*0.6*fontPx), capped so a
        // 1-digit number isn't comically tall. Draw only if the resulting font is readable
        // (≥7px). A dense grid zoomed out → tiny bulbs → fontPx<7 → hidden; zoom in → bulbs
        // grow → the same numbers cross 7px and appear. (A 3-digit number needs a ~3× bigger
        // bulb than a 1-digit one to show — exactly right: more digits need more room.)
        const widthLimited = (p.diam * 0.85) / (t.length * 0.6);
        const fontPx = Math.min(widthLimited, p.diam * 0.8);
        if (fontPx < 7) continue;                       // too small to read at this zoom
        ctx.font = `${fontPx}px ui-monospace, monospace`;
        // A dark halo so the number reads over both lit LEDs and the dark background.
        ctx.lineWidth = Math.max(2, fontPx * 0.18);
        ctx.strokeStyle = "rgba(0,0,0,0.85)";
        ctx.strokeText(t, p.sx, p.sy);
        ctx.fillStyle = "#fff";
        ctx.fillText(t, p.sx, p.sy);
    }
}

function buildMVP(ex, ey, ez, tx, ty, tz, aspect) {
    // forward = normalize(target - eye)
    const dx = tx - ex, dy = ty - ey, dz = tz - ez;
    const fLen = Math.sqrt(dx*dx + dy*dy + dz*dz) || 1;
    const fx = dx/fLen, fy = dy/fLen, fz = dz/fLen;
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

    // near is small so a deep zoom-in (camDist down to CAM_MIN) doesn't clip the LEDs in
    // front of the camera away before their sequence numbers get big enough to read.
    const near = 0.01, far = 50, fov = 0.8;
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
