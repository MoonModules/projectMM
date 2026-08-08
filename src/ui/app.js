// projectMM Web UI — all logic in one hand-maintained file per CLAUDE.md.
// Loaded as <script type="module"> so it can import the shared install-picker
// component used by both the device UI (here, OTA flash) and the GitHub Pages
// installer (first flash via Web Serial). Module loading is deferred by
// default; entry-point is the WS init at the bottom — no ordering surprises.
import { installPicker } from "/install-picker.js";
import { preview } from "/preview3d.js";
import { isNewer, parse } from "/semver.js";

// Sections (top to bottom):
//   1. State + storage
//   2. WebSocket (with keepalive, visibility pause, bfcache, exponential backoff)
//   3. REST helpers + module mutations
//   4. Render pipeline: render() → renderNav() → renderCards() → createCard() → createControl()
//   5. State patching (no-rebuild contract): updateValues() + updateModuleControls()
//   6. Type picker
//   7. Drag-to-reorder (HTML5 DnD on desktop; touchstart-gated on mobile)
//   (3D WebGL preview lives in preview3d.js — imported as `preview`)
//   8. Status bar wiring (device name, sys stats, theme, reboot)
//   9. Boot
//
// Load-bearing invariants:
//   - dragTs[mid:key] cooldown: ignore WS pushes for a control the user has touched
//     in the last 1s. Prevents slider snap-back during drag.
//   - ctrl.hidden: skip rendering hidden controls (plan-10 feature). Persistence still
//     loads them — toggling visibility doesn't lose state.
//   - No-rebuild contract: WS state updates patch values in place via querySelector.
//     We only rebuild the DOM on structural changes (add/delete/move) and explicit
//     select-driven defineControls rebuilds.

// ---------------------------------------------------------------------------
// 1. State + storage
// ---------------------------------------------------------------------------

let state = null;
let selectedModule = null;
let availableTypes = [];        // populated from GET /api/types after first connection
let ws = null;
const WS_RETRY_MIN_MS = 200;     // first reconnect is quick so a dropped initial connect (common on Safari,
                                 // whose first attempt can lose a contended socket) comes back near-instantly
let wsRetryMs = WS_RETRY_MIN_MS; // exponential backoff: 200 → 400 → 800 → … → 5000 ceiling
let wsHeartbeat = null;
let wsReconnectTimer = null;     // the pending reconnect setTimeout — tracked so pagehide can cancel it
let wsPaused = false;            // gated by document.visibilityState
let wsUnloading = false;         // true once the page starts unloading (refresh/navigate) — suppresses the
                                 // reconnect on the closing socket so the departing page doesn't error-log

const dragTimers = {};           // per-control debounce timers (clearTimeout handles)
const dragTs = {};               // per-control last-touched timestamp (ms) — a short post-interaction cooldown
// Control types whose value the user can edit — updateModuleControls suppresses a
// WS state push for one of these while the user is mid-edit. The read-only types
// (display/display-int/time/progress) and the composite `list` are absent on
// purpose: they always reflect the latest push.
const EDITABLE_CONTROL_TYPES = new Set(
    ["uint8", "uint16", "int16", "pin", "bool", "text", "textarea", "password", "select", "palette", "ipv4"]);
const TIMING_MODES = ["fps", "ms"];

// localStorage keys per ui.md
const LS_SELECTED  = "mm_selectedRoot";
const LS_THEME     = "mm_theme";
const LS_TIMING    = "mm_timing_mode";
const LS_TABS      = "mm_selectedTabs";   // { [containerName]: childName } — the open tab per container
const LS_EXPANDED  = "mm_expanded";       // [moduleName, …] — modules whose "controls" <details> is open
const LS_TA_SIZE   = "mm_textareaSizes";  // { "<module>:<control>": heightPx } — user-dragged textarea heights
const LS_CARDS_W   = "mm_cardsWidth";     // px — dragged width of the docked card column (right side)

// The open tab per container, persisted so a reload doesn't dump you back on the first child.
let selectedTabs = {};
try { selectedTabs = JSON.parse(lsRead(LS_TABS, "{}")) || {}; } catch { selectedTabs = {}; }

// Which modules have their "controls" disclosure open — VIEW-ONLY state the backend knows nothing about, so
// it lives here (like selectedTabs), NOT in the module state. Persisting it means a full-state rebuild (or a
// page reload) restores the open/closed expander instead of snapping it shut. Value/structure/picker state
// all come from the backend, so those need no client persistence.
let expandedSet = new Set();
try { expandedSet = new Set(JSON.parse(lsRead(LS_EXPANDED, "[]")) || []); } catch { expandedSet = new Set(); }
function saveExpanded() { localStorage.setItem(LS_EXPANDED, JSON.stringify([...expandedSet])); }

// The height a user dragged each textarea to — again VIEW-ONLY state the backend doesn't own (like the tab
// and expander state above), keyed by "<module>:<control>". Persisting it means a resized script/config box
// keeps its size across a full-state rebuild and a page reload instead of snapping back to the 2-row default.
let textareaSizes = {};
try { textareaSizes = JSON.parse(lsRead(LS_TA_SIZE, "{}")) || {}; } catch { textareaSizes = {}; }
function saveTextareaSize(key, heightPx) { textareaSizes[key] = heightPx; localStorage.setItem(LS_TA_SIZE, JSON.stringify(textareaSizes)); }

// The width the user dragged the docked card column to — VIEW-ONLY state (like the tab/expander/textarea
// state above). Applied as the --cards-width CSS custom property (the #main flex-basis reads it, clamped in
// CSS so it can't crowd out the preview or vanish); restored on boot. Only meaningful in docked mode, where
// the cards sit beside the preview; PiP/narrow mode makes them full-width and hides the handle.
function applyCardsWidth(px) { document.documentElement.style.setProperty("--cards-width", px + "px"); }
(function restoreCardsWidth() {
    const w = parseInt(lsRead(LS_CARDS_W, ""), 10);
    if (Number.isFinite(w) && w > 0) applyCardsWidth(w);
})();

// Wire the card-column resize handle (index.html #cards-resize). Dragging it left/right sets --cards-width
// live and persists the final value. The handle sits at the LEFT edge of #main, so dragging left widens the
// cards (they grow toward the preview); width = the pane's right edge minus the pointer x. Bounded by the
// same clamp the CSS enforces, and coalesced through requestAnimationFrame so a drag doesn't thrash layout.
function setupCardsResize() {
    const handle = document.getElementById("cards-resize");
    const main = document.getElementById("main");
    if (!handle || !main) return;
    const MIN = 280, MAX = 900;
    let dragging = false, raf = 0, pendingW = 0;
    const onMove = (clientX) => {
        // #main's right edge is fixed (the workspace's right edge); width grows as the pointer moves left.
        const right = main.getBoundingClientRect().right;
        pendingW = Math.max(MIN, Math.min(MAX, Math.round(right - clientX)));
        if (raf) return;
        raf = requestAnimationFrame(() => { raf = 0; applyCardsWidth(pendingW); });
    };
    const stop = () => {
        if (!dragging) return;
        dragging = false;
        handle.classList.remove("dragging");
        document.body.classList.remove("cards-resizing");
        if (pendingW > 0) localStorage.setItem(LS_CARDS_W, String(pendingW));
        window.removeEventListener("pointermove", onPointerMove);
        window.removeEventListener("pointerup", stop);
        window.removeEventListener("pointercancel", stop);
    };
    const onPointerMove = (e) => { if (dragging) onMove(e.clientX); };
    handle.addEventListener("pointerdown", (e) => {
        // Only resize in docked mode — the handle is CSS-hidden otherwise, but guard anyway.
        if (!document.querySelector(".workspace")?.classList.contains("mode-docked")) return;
        e.preventDefault();
        dragging = true;
        pendingW = main.getBoundingClientRect().width;
        handle.classList.add("dragging");
        document.body.classList.add("cards-resizing");
        window.addEventListener("pointermove", onPointerMove);
        window.addEventListener("pointerup", stop);
        window.addEventListener("pointercancel", stop);
    });
    // Double-click resets to the default width (a common resize-handle affordance).
    handle.addEventListener("dblclick", () => { applyCardsWidth(480); localStorage.setItem(LS_CARDS_W, "480"); });
}

function lsRead(key, defaultVal) {
    const v = localStorage.getItem(key);
    return v !== null ? v : defaultVal;
}

let timingMode = lsRead(LS_TIMING, "fps");
let theme      = lsRead(LS_THEME, "dark");

// ---------------------------------------------------------------------------
// 2. WebSocket
// ---------------------------------------------------------------------------

function connectWs() {
    if (wsReconnectTimer) { clearTimeout(wsReconnectTimer); wsReconnectTimer = null; }
    if (ws) {
        try { ws.close(); } catch {}
        ws = null;
    }
    const url = `ws://${location.host}/ws`;
    ws = new WebSocket(url);
    const sock = ws;   // captured so a stale socket's late callback (after a reconnect swapped `ws`) is a no-op
    ws.binaryType = "arraybuffer";

    ws.onopen = () => {
        if (sock !== ws) return;   // a newer socket already took over — ignore this stale open
        wsRetryMs = WS_RETRY_MIN_MS;                        // reset backoff
        setWsDot(true);
        // Keepalive ping every 25s — Safari kills idle WebSockets otherwise
        clearInterval(wsHeartbeat);
        wsHeartbeat = setInterval(() => {
            if (ws && ws.readyState === WebSocket.OPEN) ws.send("ping");
        }, 25000);
    };

    ws.onmessage = (e) => {
        if (sock !== ws || wsPaused) return;   // ignore a stale socket's late frame
        if (e.data instanceof ArrayBuffer) {
            preview.onBinaryMessage(e.data);
            return;
        }
        try {
            const data = JSON.parse(e.data);
            if (!data) return;
            // The device sends a FULL {modules:[...]} state on connect / after a structural change,
            // then a {patch:[...]} of only-changed leaves each second (diff-on-the-wire — the whole
            // module tree is ~34 KB of mostly-unchanging metadata that must NOT be re-serialised every
            // second on the render thread; see HttpServerModule buildStatePatch). Apply a patch onto
            // the existing `state` in place, then refresh the DOM. A patch before any full state is
            // ignored (we have nothing to patch); the device resyncs on connect so this self-corrects.
            if (Array.isArray(data.patch)) {
                if (state && Array.isArray(state.modules)) { applyStatePatch(data.patch); updateValues(); }
                return;
            }
            // The same /ws also carries WLED-compatibility {state,info} frames for the native WLED app
            // (see HttpServerModule's WLED shim). Those aren't our module-state shape — ignore anything
            // without a `modules` array, or it would clobber `state` and blank the module view.
            if (!Array.isArray(data.modules)) return;
            state = data;
            renderCards();     // a full state may add/remove/reshape cards (structural resync) — full render
            updateValues();
        } catch {
            // ignore malformed messages
        }
    };

    ws.onclose = () => {
        if (sock !== ws) return;   // a stale socket closing after we already moved on — leave the live one alone
        clearInterval(wsHeartbeat);
        wsHeartbeat = null;
        if (wsUnloading) return;   // the page is going away — don't reconnect (and don't touch the DOM)
        setWsDot(false);
        // Exponential backoff with 5s ceiling; track the timer so pagehide can cancel a pending reconnect.
        wsReconnectTimer = setTimeout(connectWs, wsRetryMs);
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
        wsUnloading = false;   // a bfcache-restored page is live again — allow reconnects
        if (!ws || ws.readyState !== WebSocket.OPEN) connectWs();
    }
});
// Close the socket cleanly as the page unloads (a refresh, a navigation, or a bfcache suspend). Without
// this the departing page's socket is torn down abnormally by the browser, which logs a "connection lost"
// error to the console every refresh. Sending a normal (1000) close first, and marking wsUnloading so
// onclose skips its reconnect, makes the handover silent. pagehide (not beforeunload) fires for bfcache too.
window.addEventListener("pagehide", () => {
    wsUnloading = true;
    clearInterval(wsHeartbeat);
    if (wsReconnectTimer) { clearTimeout(wsReconnectTimer); wsReconnectTimer = null; }   // no reconnect after unload
    if (ws) { try { ws.close(1000); } catch { /* already closing */ } }
});

// ---------------------------------------------------------------------------
// 3. REST helpers + module mutations
// ---------------------------------------------------------------------------

async function init() {
    applyTheme(theme);
    setupStatusBarButtons();
    setupUpdateBadge();
    setupCardsResize();
    // Open the WebSocket FIRST, before any HTTP fetch. The device pushes a full {modules} state on connect
    // (handleWebSocketUpgrade → requestFullResync), so the WS is the primary state source — the /api/state
    // fetch below is only a first-paint shortcut. Connecting first matters most on Safari: it opens more
    // parallel connections up front and is quicker to give up on a contended one, so if the WS is opened
    // LAST (after several awaited fetches + the page's file loads) it lands in the most-saturated moment of
    // the device's small socket pool and Safari abandons it — the "basic UI shows but the WS never goes
    // live" symptom. Opening it first lets it grab an uncontended slot; Chrome tolerated the old order, so
    // this fixes Safari without regressing Chrome.
    connectWs();
    preview.init();
    preview.setupLayout();
    // First-paint shortcut: render from a one-shot /api/state so the cards appear immediately instead of
    // waiting for the WS's first full-state push. The WS then keeps everything live. If this fetch fails
    // (a contended slot), it's non-fatal — the WS full state fills in the moment it lands. Since the WS is
    // opened FIRST, its full-state push can beat this await; when it has (state already set), DON'T let the
    // REST snapshot overwrite the newer, live WS state — just skip the commit.
    try {
        const resp = await fetch("/api/state");
        if (resp.ok && (!state || !Array.isArray(state.modules))) {
            const snap = await resp.json();
            if (snap && Array.isArray(snap.modules)) {
                state = snap;
                const savedSel = lsRead(LS_SELECTED, null);
                if (state.modules.length > 0) {
                    const exists = savedSel && state.modules.some(m => m.name === savedSel);
                    selectedModule = exists ? savedSel : state.modules[0].name;
                }
                renderNav();
                renderCards();
                updateStatusBar();
            }
        }
    } catch { /* non-fatal — the WS full state renders the UI when it arrives */ }
    // /api/types arrived in plan-11; fetch in parallel. When it arrives, the reset-to-default buttons (whose
    // defaults come from this payload) need to appear — but a full renderCards() rebuilds the DOM, which
    // would blow away a control the user is mid-edit (this fires ~1 s after first paint, so that's rare but
    // possible). Skip the re-render while an editable field is focused / a native select is open; the reset
    // buttons then appear on the next structural render instead of interrupting the edit.
    fetch("/api/types").then(r => r.json()).then(j => {
        availableTypes = j.types || [];
        const el = document.activeElement;
        const editing = el && (el.matches("input, textarea") || el.closest("select")
                               || document.querySelector('select[data-open="true"]'));
        if (state && !editing) renderCards();
    }).catch(() => {});
}

// The message for a failed fetch Response: the server's own `{"error": …}` body (JSON, e.g.
// "not enough space (N free)") when present, else a bare `HTTP <status>`. Every /api/* handler
// returns errors as that JSON shape, so this is the one place the extraction lives.
async function errorMessage(res) {
    try {
        const j = await res.json();
        if (j && j.error) return j.error;
    } catch (_) { /* non-JSON error body — fall through to the status code */ }
    return `HTTP ${res.status}`;
}

async function sendControl(moduleName, controlName, value) {
    // Optimistically update the local `state` to what we just sent — the standard controlled-input
    // pattern. Without this, `state` keeps the OLD value until the device echoes the change back in a
    // value patch (up to a tick1s later, or folded into a full resync for a control that triggers a
    // rebuild like a driver's ledsPerPin). In that gap, an unrelated WS frame runs updateModuleControls,
    // and once the dragTs edit-guard expires (>1s after the last keystroke — trivial if the user pauses)
    // it writes the stale `state` value straight back into the field the user just changed (the value
    // "reverses"; a manual refresh shows the correct value because it refetches). The client knows what
    // it sent, so update `state` now; any later echo just confirms it.
    if (state && Array.isArray(state.modules)) {
        const mod = allModules().find(m => m.name === moduleName);
        const ctrl = mod && Array.isArray(mod.controls) && mod.controls.find(c => c.name === controlName);
        if (ctrl) ctrl.value = value;
    }
    // Toggling expert mode changes which controls RENDER (the `advanced` ones), not just a value — so
    // re-render the cards. Structural change, same as an add/remove; the value write above already landed.
    if (moduleName === "System" && controlName === "expertMode") renderCards();
    // Best-effort by design — failures are not retried here. Non-ok responses +
    // network errors are logged to console so a user with devtools open can see
    // what went wrong (e.g. a control value the device-side validator rejected).
    try {
        const res = await fetch("/api/control", {
            method: "POST",
            headers: {"Content-Type": "application/json"},
            body: JSON.stringify({module: moduleName, control: controlName, value: value})
        });
        if (!res.ok) {
            console.warn(`[control] POST ${moduleName}.${controlName} failed (status=${res.status})`);
        }
    } catch (e) {
        console.warn(`[control] POST ${moduleName}.${controlName} failed (error=${e && e.message ? e.message : e})`);
    }
}

async function refetchState() {
    try {
        const r = await fetch("/api/state");
        state = await r.json();
        renderNav();
        renderCards();
    } catch {}
}

// --- Editable-list row ops (the client half of the editable List primitive) ---
// Same best-effort fetch style as sendControl: non-ok + network errors are logged,
// not retried. After a successful mutation the server persists + re-runs prepare and
// pushes fresh state over the WS; we also refetchState() to match the module add/
// delete/move ops (which don't wait for the WS push either). The row `id` is a number;
// it goes into the URL path as-is. `c` (control name) may contain any chars, so encode.
async function listAddRow(moduleName, ctrlName) {
    try {
        const res = await fetch(`/api/list/${encodeURIComponent(moduleName)}/${encodeURIComponent(ctrlName)}`, {
            method: "POST",
            headers: {"Content-Type": "application/json"},
            body: "{}"
        });
        if (!res.ok) { console.warn(`[list] add ${moduleName}.${ctrlName} failed (status=${res.status})`); return null; }
        const j = await res.json();
        refetchState();
        return j && typeof j.id === "number" ? j.id : null;
    } catch (e) {
        console.warn(`[list] add ${moduleName}.${ctrlName} failed (error=${e && e.message ? e.message : e})`);
        return null;
    }
}

async function listDeleteRow(moduleName, ctrlName, id) {
    try {
        const res = await fetch(`/api/list/${encodeURIComponent(moduleName)}/${encodeURIComponent(ctrlName)}/${id}`, {method: "DELETE"});
        if (!res.ok) { console.warn(`[list] delete ${moduleName}.${ctrlName}#${id} failed (status=${res.status})`); return; }
        refetchState();
    } catch (e) {
        console.warn(`[list] delete ${moduleName}.${ctrlName}#${id} failed (error=${e && e.message ? e.message : e})`);
    }
}

async function listMoveRow(moduleName, ctrlName, id, to) {
    try {
        const res = await fetch(`/api/list/${encodeURIComponent(moduleName)}/${encodeURIComponent(ctrlName)}/${id}`, {
            method: "PATCH",
            headers: {"Content-Type": "application/json"},
            body: JSON.stringify({to: to})
        });
        if (!res.ok) { console.warn(`[list] move ${moduleName}.${ctrlName}#${id}→${to} failed (status=${res.status})`); return; }
        refetchState();
    } catch (e) {
        console.warn(`[list] move ${moduleName}.${ctrlName}#${id} failed (error=${e && e.message ? e.message : e})`);
    }
}

async function listSetField(moduleName, ctrlName, id, field, value) {
    try {
        const res = await fetch(`/api/list/${encodeURIComponent(moduleName)}/${encodeURIComponent(ctrlName)}/${id}`, {
            method: "PATCH",
            headers: {"Content-Type": "application/json"},
            body: JSON.stringify({field: field, value: value})
        });
        if (!res.ok) { console.warn(`[list] set ${moduleName}.${ctrlName}#${id}.${field} failed (status=${res.status})`); return false; }
        // A FIELD edit does NOT refetch: the value is already applied server-side and the WS push
        // reconciles it, so rebuilding here would collapse the open row and snap an in-progress edit
        // back (a `channels` change also reshapes the row's fields — that structural update rides the
        // WS push, guarded by the dragTs cooldown, not a rebuild-on-every-keystroke). Structural ops
        // (add/delete/move) DO refetch — they change the row set. Return true so the caller can decide.
        return true;
    } catch (e) {
        console.warn(`[list] set ${moduleName}.${ctrlName}#${id}.${field} failed (error=${e && e.message ? e.message : e})`);
        return false;
    }
}

async function addModule(type, parentName) {
    if (!type) return;
    const body = {type: type};
    if (parentName) body.parent_id = parentName;
    let name = null;
    try {
        const r = await fetch("/api/modules", {
            method: "POST",
            headers: {"Content-Type": "application/json"},
            body: JSON.stringify(body)
        });
        name = (await r.json()).name;   // the created module's final name (post-disambiguation)
    } catch {}
    // Select the new module's tab BEFORE the re-render so renderCards shows it active (the tab strip
    // reads selectedTabs[parent]); then scroll it into view and focus its first control so a keyboard
    // user lands on it. Without this the view stays on the previously-active tab and the new module
    // is added out of sight.
    if (name && parentName) {
        selectedTabs[parentName] = name;
        localStorage.setItem(LS_TABS, JSON.stringify(selectedTabs));   // persist like the tab-click path
    }
    await refetchState();
    if (name) focusModule(name);
}

// Bring a module's card into view and focus its first control (added via the "+" flow).
function focusModule(name) {
    const card = document.querySelector(`.card[data-module="${cssEscape(name)}"]`);
    if (!card) return;
    // A child card can wrap its controls in a collapsed <details> (.card-controls-collapse) — open it
    // FIRST so the card is at its expanded height, THEN scroll: scrolling a still-collapsed card lands
    // on its pre-expansion geometry and the focused control ends up mispositioned.
    const collapse = card.querySelector("details.card-controls-collapse");
    if (collapse) collapse.open = true;
    card.scrollIntoView({ block: "nearest", behavior: "smooth" });
    // Focus the first real control input, NOT the tab strip / header buttons — and NOT the status row,
    // which is a `.control-row` with only spans (a freshly added driver leads with a status, so picking
    // the first `.control-row` would find no input and focus nothing). Query for the input directly
    // inside any control row so the status row is skipped.
    const first = card.querySelector(".control-row input, .control-row select, .control-row textarea, .control-row button");
    if (first) first.focus({ preventScroll: true });
}

async function deleteModule(name) {
    await fetch("/api/modules/" + encodeURIComponent(name), {method: "DELETE"});
    refetchState();
}

// move to absolute index (0..siblings.length-1). Called from drag-and-drop.
async function moveModuleTo(name, toIndex) {
    await fetch("/api/modules/" + encodeURIComponent(name) + "/move", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify({to: toIndex})
    });
    refetchState();
}

// swap a module for another type at the same position. The replacement starts
// with its own default control values — a clean swap, not a value carry-over.
async function replaceModule(name, newType) {
    if (!newType) return;
    await fetch("/api/modules/" + encodeURIComponent(name) + "/replace", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify({type: newType})
    });
    refetchState();
}

async function rebootDevice() {
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

    // One entry per root module. Clicking selects that root — only the selected
    // root's card subtree is rendered (one root visible at a time).
    const list = document.createElement("div");
    list.className = "nav-list";
    for (const mod of state.modules) {
        const item = document.createElement("button");
        item.type = "button";
        item.className = "nav-item";
        item.textContent = mod.name;
        item.dataset.module = mod.name;
        if (mod.name === selectedModule) item.classList.add("active");
        item.addEventListener("click", () => selectModule(mod.name));
        list.appendChild(item);
    }
    nav.appendChild(list);
    nav.appendChild(buildNavFooter());
}

// Footer pinned to the bottom of the side nav: copyright + social links.
function buildNavFooter() {
    const footer = document.createElement("footer");
    footer.className = "nav-footer";

    const links = document.createElement("div");
    links.className = "nav-social";
    const SOCIAL = [
        ["GitHub",  "https://github.com/MoonModules/projectMM",
         "M12 .5C5.65.5.5 5.65.5 12a11.5 11.5 0 0 0 7.86 10.92c.58.1.79-.25.79-.56v-2c-3.2.7-3.88-1.54-3.88-1.54-.53-1.34-1.3-1.7-1.3-1.7-1.06-.72.08-.71.08-.71 1.17.08 1.79 1.2 1.79 1.2 1.04 1.79 2.73 1.27 3.4.97.1-.76.41-1.27.74-1.56-2.55-.29-5.24-1.28-5.24-5.69 0-1.26.45-2.29 1.19-3.1-.12-.29-.52-1.46.11-3.05 0 0 .97-.31 3.18 1.18a11 11 0 0 1 5.8 0c2.2-1.49 3.17-1.18 3.17-1.18.63 1.59.23 2.76.11 3.05.74.81 1.19 1.84 1.19 3.1 0 4.42-2.69 5.39-5.25 5.68.42.36.8 1.08.8 2.18v3.23c0 .31.21.67.8.56A11.5 11.5 0 0 0 23.5 12C23.5 5.65 18.35.5 12 .5Z"],
        ["Discord", "https://discord.gg/TC8NSUSCdV",
         "M20.32 4.37A19.8 19.8 0 0 0 15.45 2.9a13.6 13.6 0 0 0-.62 1.27 18.3 18.3 0 0 0-5.67 0A13 13 0 0 0 8.54 2.9 19.7 19.7 0 0 0 3.67 4.37C.57 8.96-.27 13.44.15 17.85a19.9 19.9 0 0 0 6 3.03c.49-.66.92-1.36 1.29-2.1-.71-.27-1.39-.6-2.03-.99.17-.12.34-.25.5-.38a14.2 14.2 0 0 0 12.18 0c.16.13.33.26.5.38-.64.39-1.32.72-2.03.99.37.74.8 1.44 1.29 2.1a19.8 19.8 0 0 0 6-3.03c.5-5.1-.85-9.55-3.58-13.48ZM8.02 15.13c-1.18 0-2.15-1.08-2.15-2.41 0-1.33.95-2.42 2.15-2.42 1.2 0 2.17 1.1 2.15 2.42 0 1.33-.95 2.41-2.15 2.41Zm7.96 0c-1.18 0-2.15-1.08-2.15-2.41 0-1.33.95-2.42 2.15-2.42 1.2 0 2.17 1.1 2.15 2.42 0 1.33-.95 2.41-2.15 2.41Z"],
        ["Reddit",  "https://reddit.com/r/moonmodules",
         "M22 12c0-1.1-.9-2-2-2-.55 0-1.04.22-1.4.58a9.8 9.8 0 0 0-5.1-1.55l.87-4.1 2.85.6a1.5 1.5 0 1 0 .15-1l-3.18-.67a.5.5 0 0 0-.59.38l-.97 4.57a9.8 9.8 0 0 0-5.16 1.55A2 2 0 1 0 4 13.66a3.9 3.9 0 0 0-.05.6c0 3.3 3.86 5.98 8.62 5.98 4.76 0 8.62-2.68 8.62-5.98 0-.2-.02-.4-.05-.6.53-.36.86-.96.86-1.66ZM8 13.5a1.5 1.5 0 1 1 3 0 1.5 1.5 0 0 1-3 0Zm8.32 4.07c-1.04 1.04-3.02 1.12-3.6 1.12-.58 0-2.57-.08-3.6-1.12a.4.4 0 0 1 .56-.56c.65.65 2.05.88 3.04.88.99 0 2.39-.23 3.04-.88a.4.4 0 0 1 .56.56ZM16 15a1.5 1.5 0 1 1 0-3 1.5 1.5 0 0 1 0 3Z"],
        ["YouTube", "https://www.youtube.com/@MoonModulesLighting",
         "M23.5 6.5a3 3 0 0 0-2.12-2.12C19.5 3.87 12 3.87 12 3.87s-7.5 0-9.38.51A3 3 0 0 0 .5 6.5C0 8.38 0 12 0 12s0 3.62.5 5.5a3 3 0 0 0 2.12 2.12c1.88.51 9.38.51 9.38.51s7.5 0 9.38-.51a3 3 0 0 0 2.12-2.12C24 15.62 24 12 24 12s0-3.62-.5-5.5ZM9.6 15.6V8.4l6.2 3.6-6.2 3.6Z"],
    ];
    for (const [name, url, path] of SOCIAL) {
        const a = document.createElement("a");
        a.href = url;
        a.target = "_blank";
        a.rel = "noopener";
        a.title = name;
        a.setAttribute("aria-label", name);
        a.innerHTML = `<svg viewBox="0 0 24 24" width="18" height="18" fill="currentColor"><path d="${path}"/></svg>`;
        links.appendChild(a);
    }
    footer.appendChild(links);

    // Diagnostic bundle download. Fetches /api/state + /api/system from
    // the *same* origin we're on (the device itself) — sidesteps Chrome's
    // mixed-content blocker that prevents the install page (HTTPS Pages)
    // from doing the same fetch against the device (HTTP LAN). Output is
    // a single JSON blob the user can attach to a bug report.
    const diag = document.createElement("a");
    diag.href = "#";
    diag.className = "nav-diag-link";
    diag.textContent = "Download diagnostics";
    diag.addEventListener("click", async (ev) => {
        ev.preventDefault();
        try {
            const [stateResp, systemResp] = await Promise.all([
                fetch("/api/state"),
                fetch("/api/system"),
            ]);
            const [stateJson, systemJson] = await Promise.all([
                stateResp.json(),
                systemResp.json(),
            ]);
            const bundle = {
                capturedAt: new Date().toISOString(),
                origin: location.origin,
                state: stateJson,
                system: systemJson,
            };
            const blob = new Blob([JSON.stringify(bundle, null, 2)],
                                  { type: "application/json" });
            // Devicename comes from system.deviceName if present, else
            // falls back to the hostname (e.g. "MM-BD3C.local") so the
            // filename is still useful when SystemModule's wire shape
            // doesn't include the name field.
            const devName = (systemJson && systemJson.deviceName)
                || location.hostname || "device";
            const fname = `projectMM-diag-${devName}-${Date.now()}.json`;
            const a = document.createElement("a");
            const blobUrl = URL.createObjectURL(blob);
            a.href = blobUrl;
            a.download = fname;
            a.click();
            // Defer the revoke so the browser has time to start the download.
            // Revoking immediately after click() is technically race-safe on
            // recent Chrome / Firefox (the click navigation is synchronous)
            // but Safari has been observed dropping downloads under a fast
            // revoke. A few seconds is the canonical workaround.
            setTimeout(() => URL.revokeObjectURL(blobUrl), 4000);
        } catch (e) {
            alert(`Diagnostic capture failed: ${e && e.message ? e.message : e}`);
        }
    });
    footer.appendChild(diag);

    const copy = document.createElement("div");
    copy.className = "nav-copyright";
    copy.textContent = `© ${new Date().getFullYear()} MoonModules`;
    footer.appendChild(copy);

    return footer;
}

function selectModule(name) {
    // Surface demo sweep (REMOVABLE): leaving a surface re-arms it, so re-opening demos again while
    // a re-render of the SAME surface (a pad click) does not.
    if (name !== selectedModule) surfaceDemoShownFor = null;
    selectedModule = name;
    localStorage.setItem(LS_SELECTED, name);
    document.querySelectorAll(".nav-item").forEach((el) => {
        el.classList.toggle("active", el.dataset.module === name);
    });
    renderCards();
    closeNavDrawer();
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

// Global "expert mode": the System module's expertMode control. Controls tagged `advanced` (dev/tuning
// readouts + knobs) render only when this is on. Read live from state so a toggle takes effect on the
// next render with no reload; default off if System or the control isn't present yet.
function isExpertMode() {
    const sys = state ? findModule("System") : null;
    const c = sys && sys.controls && sys.controls.find(c => c.name === "expertMode");
    return !!(c && c.value);
}

function renderCards() {
    const main = document.getElementById("main");
    if (!main || !state) return;
    main.innerHTML = "";

    // One root visible at a time: render only the selected root's subtree.
    // Falls back to the first root if the selection is missing or stale.
    let root = selectedModule ? findModule(selectedModule) : null;
    if (!root && state.modules.length > 0) {
        root = state.modules[0];
        selectedModule = root.name;
    }
    if (root) renderModuleTree(root, main, 0);
    // Surface demo sweep (REMOVABLE): delete this line and the block above it to remove the feature.
    // `main` is the root of the rendered subtree, so it holds the encoder strips, the pad grid and
    // the fader strips wherever they sit in the card nesting.
    if (root && main.querySelector(".list-pads-fixed")) startSurfaceDemo(main, root.name);
}

// ============================================================================
// Surface demo sweep — REMOVABLE BLOCK
//
// A hardware control desk sweeps its motorised faders and rings when it powers up, both to show it
// is alive and to show what it has. This does the same for three seconds after the surface appears.
//
// It is deliberately isolated so it can be deleted whole: this block, plus the single
// `startSurfaceDemo()` call at the end of renderCards(). It is a VIEW-ONLY animation — it never
// calls sendControl, so it cannot write a swept value to the device, and the first WebSocket patch
// after it finishes restores whatever the device actually holds.
// ============================================================================
const SURFACE_DEMO_MS = 1000;
let surfaceDemoUntil = 0;
/// The module the sweep last ran for. renderCards() fires on every state push and every mutation (a
/// pad click refetches and re-renders), so "the surface was rendered" is NOT "the surface was
/// opened" — without this the sweep replayed on every button press.
let surfaceDemoShownFor = null;

function startSurfaceDemo(root, moduleName) {
    // The sweep is pure decoration, so it is the first thing to drop for someone who asked for less
    // motion. Checked before the once-per-surface flag, so the flag is not burned by a skipped run.
    if (window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches) return;
    if (surfaceDemoShownFor === moduleName) return;   // already demoed this surface
    // Every bank is queried from the SAME root. The encoder and fader strips sit on the top-level
    // card while the pad grid is inside a nested one, so scoping to the card that contains the pads
    // found the pads and nothing else.
    const q = (sel) => [...root.querySelectorAll(sel)];
    // Every bank is re-queried each frame, for the same reason the pads are: a state push mid-sweep
    // re-renders the card and replaces these nodes. A list captured once would keep writing values to
    // detached elements while the visible controls sat frozen — which is what stopped the knobs and
    // faders a few hundred ms into a refresh, while the (live-queried) pads ran the full period.
    const liveInputs = () => [
        ...q(".control-encoder .encoder-input"),
        ...q(".control-fader .fader-input"),
    ];
    const knobs  = q(".control-encoder .encoder-input");
    const faders = q(".control-fader .fader-input");
    // Pads are looked up LIVE each frame rather than captured: a state push mid-sweep re-renders the
    // card and replaces these nodes, and a captured list would spend the rest of the run toggling
    // classes on detached elements — which is why the chase stopped partway through.
    const livePads = () => q(".list-pad:not(.list-pad-empty)");
    if (!knobs.length && !faders.length && !livePads().length) return;
    surfaceDemoShownFor = moduleName;

    // No snapshot: the demo starts on the first render, BEFORE the caller assigns input.value, so a
    // snapshot here captures the browser's default (50) and restoring it would overwrite every real
    // value. The device state is the truth, so the sweep ends by replaying it.
    const start = performance.now();
    surfaceDemoUntil = start + SURFACE_DEMO_MS;

    const step = (now) => {
        const t = (now - start) / SURFACE_DEMO_MS;
        if (t >= 1) {
            // Put the real values back by re-applying device state, which also repaints the knobs
            // and readouts through the normal patch path.
            surfaceDemoUntil = 0;
            updateValues();
            livePads().forEach(p => p.classList.remove("list-pad-demo"));
            return;
        }
        // A travelling wave rather than every control moving together: the phase offset per column is
        // what makes it read as a sweep across the desk.
        liveInputs().forEach((el, i) => {
            const lo = Number(el.min) || 0, hi = Number(el.max) || 255;
            const phase = t * 1.5 * Math.PI * 2 - i * 0.5;
            el.value = Math.round(lo + (hi - lo) * (0.5 + 0.45 * Math.sin(phase)));
            redrawRangeDecorations(el);
        });
        // The pads chase in sequence, one lit at a time, so the grid shows its extent. Paced by TIME
        // rather than by pad count, so the chase runs at the same speed and for the full duration
        // whatever number of presets exist.
        const pads = livePads();
        if (pads.length) {
            const lit = Math.floor(t * SURFACE_DEMO_MS / 120) % pads.length;
            pads.forEach((p, i) => p.classList.toggle("list-pad-demo", i === lit));
        }
        requestAnimationFrame(step);
    };
    requestAnimationFrame(step);
}

/// True while the sweep is running: updateValues() skips patching the swept inputs, so a WebSocket
/// push mid-demo does not fight the animation for the same DOM node.
function surfaceDemoRunning() { return surfaceDemoUntil > performance.now(); }
// ======================= end REMOVABLE BLOCK ================================

function renderModuleTree(mod, parentEl, depth) {
    const { card, childrenEl } = createCard(mod, depth);
    parentEl.appendChild(card);
    // Children render inside this card's .card-children wrapper, not as flat
    // siblings. childrenEl is null for modules that don't accept children.
    if (!childrenEl || !mod.children || mod.children.length === 0) return;

    // One rule: a TOP-LEVEL module shows its children one at a time behind a tab strip, so its card
    // stays short however many children it has. Deeper levels stack as before. The tabs are derived
    // from mod.children on every render, so adding a layer adds its tab — there is no tab registry
    // to keep in sync, which is the whole of the "dynamic" requirement.
    if (depth === 0) {
        // The "+" tab takes over the add affordance, so hide the footer's duplicate button — but keep
        // the footer element itself, because openTypePicker renders the picker into it.
        const addBtn = card.querySelector(".card-footer > .add-btn");
        if (addBtn) addBtn.style.display = "none";
        renderChildTabs(mod, childrenEl, depth);
        return;
    }
    for (const child of mod.children) {
        renderModuleTree(child, childrenEl, depth + 1);
    }
}

// Drag a tab onto another to reorder — the tab strip IS the child list, so reordering tabs reorders
// the modules. Deliberately the same insert-semantics + moveModuleTo() call the card drag uses
// (attachDragHandlers): one reorder path, so a tab drag and a card drag can't drift apart.
function attachTabDragHandlers(tab, child, parent) {
    tab.draggable = true;

    tab.addEventListener("dragstart", (e) => {
        e.stopPropagation();                       // don't let the enclosing card's dragstart claim it
        e.dataTransfer.effectAllowed = "move";
        e.dataTransfer.setData("text/plain", child.name);
        tab.classList.add("dragging");
    });
    tab.addEventListener("dragend", () => {
        tab.classList.remove("dragging");
        document.querySelectorAll(".tab.drag-over").forEach(t => t.classList.remove("drag-over"));
    });
    tab.addEventListener("dragover", (e) => {
        const src = document.querySelector(".tab.dragging");
        if (!src || src === tab) return;
        if (src.parentElement !== tab.parentElement) return;   // same strip only — not another container's
        e.preventDefault();
        tab.classList.add("drag-over");
    });
    tab.addEventListener("dragleave", () => tab.classList.remove("drag-over"));
    tab.addEventListener("drop", (e) => {
        e.preventDefault();
        e.stopPropagation();
        tab.classList.remove("drag-over");
        const srcName = e.dataTransfer.getData("text/plain");
        if (!srcName || srcName === child.name) return;
        // Re-find the index by NAME, not from the captured render-time object: state is replaced on
        // every WS push, so `parent` here would be stale within ~1 s (same trap the card drag notes).
        const live = findModule(parent.name);
        const targetIdx = ((live && live.children) || []).findIndex(c => c.name === child.name);
        if (targetIdx < 0) return;
        moveModuleTo(srcName, targetIdx);
    });
}

// A tab carries its module's fault severity as a dot, because a tab that can HIDE an error is worse
// than no tab: a driver failing on a background tab must still be visible from the strip.
function applyTabDot(tab, mod) {
    const old = tab.querySelector(".tab-dot");
    if (old) old.remove();
    if (mod.severity !== "error" && mod.severity !== "warning") return;
    const dot = document.createElement("span");
    dot.className = "tab-dot tab-dot-" + mod.severity;
    tab.appendChild(dot);
}

// Patch-path twin of applyTabDot + the disabled greying: the tab strip is built in renderCards(), which the
// WS value patch deliberately never re-runs — so without this, a fault (or an enable/disable) on a background
// tab would stay invisible until the next full render. (The UI has two render paths; a rule must live in both.)
function updateTabDot(mod) {
    const tab = document.querySelector(`.tab[data-tab-mid="${cssEscape(mod.name)}"]`);
    if (!tab) return;
    applyTabDot(tab, mod);
    tab.classList.toggle("tab--disabled", mod.enabled === false);   // grey a disabled module's tab title
}

// Tab strip + the one selected child. `active` falls back to the first child whenever the remembered
// tab is gone (the driver was deleted) or was never set, so the selection can never dangle.
function renderChildTabs(mod, childrenEl, depth) {
    const names = mod.children.map(c => c.name);
    let active = selectedTabs[mod.name];
    if (!names.includes(active)) active = names[0];

    const strip = document.createElement("div");
    strip.className = "tab-strip";
    strip.setAttribute("role", "tablist");

    for (const child of mod.children) {
        const tab = document.createElement("button");
        // Grey a disabled child's tab title (mirrors the card's card--disabled), so it reads as inactive
        // from the strip without opening it. Derived purely from child.enabled — no backend round-trip.
        tab.className = "tab" + (child.name === active ? " tab-active" : "")
                              + (child.enabled === false ? " tab--disabled" : "");
        tab.type = "button";
        tab.setAttribute("role", "tab");
        tab.setAttribute("aria-selected", child.name === active ? "true" : "false");
        // Name + the module's own status dot, so a driver erroring on a background tab is still
        // visible without opening it — a tab that can hide a fault is worse than no tab.
        tab.dataset.tabMid = child.name;   // so updateTabDot can find it on the WS patch path
        tab.textContent = child.name;
        applyTabDot(tab, child);
        attachTabDragHandlers(tab, child, mod);
        tab.addEventListener("click", () => {
            selectedTabs[mod.name] = child.name;
            localStorage.setItem(LS_TABS, JSON.stringify(selectedTabs));
            renderCards();
        });
        strip.appendChild(tab);
    }

    // "+" lives at the END OF THE STRIP, where a new tab appears — not in the card footer below the
    // panel, which would read as "add something to the open driver" rather than "add a driver".
    // createCard still renders the footer button for non-tabbed containers; here we hide it and put
    // the affordance where the tabs are.
    if (acceptsNewChildren(mod)) {
        const addTab = document.createElement("button");
        addTab.className = "tab tab-add";
        addTab.type = "button";
        addTab.textContent = "+";
        addTab.title = "add " + rolesAcceptedBy(mod).join(" / ");
        addTab.addEventListener("click", () => {
            // THIS card's own footer — a plain querySelector would match the first .card-footer in the
            // subtree, which belongs to a nested child's card (Effects would then offer the Layer's
            // effects instead of another layer). Scope to direct children of this card.
            const card = childrenEl.parentElement;
            const footer = [...card.children].find(el => el.classList.contains("card-footer"));
            if (footer) openTypePicker(mod, footer);
        });
        strip.appendChild(addTab);
    }
    childrenEl.appendChild(strip);

    const panel = document.createElement("div");
    panel.className = "tab-panel";
    panel.setAttribute("role", "tabpanel");
    childrenEl.appendChild(panel);
    const child = mod.children.find(c => c.name === active);
    if (child) renderModuleTree(child, panel, depth + 1);
}

function createCard(mod, depth) {
    const card = document.createElement("div");
    card.className = "card";
    card.dataset.module = mod.name;
    card.dataset.depth = String(depth);

    // -- Title row: [enabled?] [name] [stats] [actions] --
    const title = document.createElement("div");
    title.className = "card-title";

    // The enabled toggle is built here but appended later — it joins the
    // right-hand action cluster (next to ✎ × ?) rather than sitting at the start
    // of the row, for visual grouping with the other per-card controls.
    // Rendered as a <button> styled as a 26×26 rounded box (matching .card-btn);
    // showing ✓ when on, blank when off. Stores its checked state in
    // data-checked so updateValues can sync from WS pushes. A native <input>
    // would not match the other buttons' frame and corner radius.
    const enabled = document.createElement("button");
    enabled.type = "button";
    enabled.className = "module-enabled";
    enabled.dataset.mid = mod.name;
    enabled.dataset.key = "enabled";
    enabled.setAttribute("aria-pressed", "true");
    enabled.title = "Enable / disable";
    const setEnabledUi = (on) => {
        enabled.dataset.checked = on ? "true" : "false";
        enabled.textContent = "⏻";
        enabled.classList.toggle("module-enabled--off", !on);
        enabled.setAttribute("aria-pressed", on ? "true" : "false");
        card.classList.toggle("card--disabled", !on);
        // Grey this module's TAB in the same click, alongside its card — so the tab title dims INSTANTLY
        // instead of waiting ~1s for the server's full-state round-trip. (updateTabDot still syncs it on the
        // patch path, idempotently, so this just makes the on/off button the immediate driver.) The tab
        // lives in the parent's strip, found by the same data-tab-mid updateTabDot uses.
        const tabEl = document.querySelector(`.tab[data-tab-mid="${cssEscape(mod.name)}"]`);
        if (tabEl) tabEl.classList.toggle("tab--disabled", !on);
    };
    setEnabledUi(mod.enabled === undefined ? true : !!mod.enabled);
    enabled.addEventListener("click", () => {
        const next = enabled.dataset.checked !== "true";
        setEnabledUi(next);
        // Stamp dragTs so a WS state push older than this click can't revert
        // the toggle before the server has acknowledged. updateValues reads
        // dragTs[mod.name + ":enabled"] on line ~952 and suppresses stale
        // patches within the 1s cooldown.
        dragTs[mod.name + ":enabled"] = Date.now();
        sendControl(mod.name, "enabled", next);
    });

    const name = document.createElement("span");
    name.className = "card-name";
    name.textContent = mod.name;
    title.appendChild(name);

    // Emoji tags (role + curated) shown after the name — same set used by the
    // type picker's chip filter, so visual identity is consistent across views.
    const emoji = emojiTagsForMod(mod);
    if (emoji) {
        const emojiEl = document.createElement("span");
        emojiEl.className = "card-name-emoji";
        emojiEl.textContent = emoji;
        title.appendChild(emojiEl);
    }

    // Flex spacer so the name stays left and everything else groups on the right.
    const spacer = document.createElement("span");
    spacer.className = "card-spacer";
    title.appendChild(spacer);

    // fps/ms toggle on the stats line — global mode, single click cycles all cards
    const stats = document.createElement("span");
    stats.className = "card-stats";
    stats.dataset.mid = mod.name;
    stats.dataset.key = "stats";
    stats.title = formatStatsTitle(mod);
    stats.textContent = formatStats(mod);
    stats.addEventListener("click", () => {
        const idx = TIMING_MODES.indexOf(timingMode);
        timingMode = TIMING_MODES[(idx + 1) % TIMING_MODES.length];
        localStorage.setItem(LS_TIMING, timingMode);
        // Refresh every card's stats line in place — no full re-render needed
        document.querySelectorAll(".card-stats[data-mid]").forEach(s => {
            const m = findModule(s.dataset.mid);
            if (m) { s.textContent = formatStats(m); s.title = formatStatsTitle(m); }
        });
    });
    title.appendChild(stats);

    // Enable checkbox joins the right-hand action cluster, before ✎/×.
    title.appendChild(enabled);

    // Delete / replace buttons for user-managed children (any role a container
    // accepts, minus modules that opted out via userEditable=false). Top-level
    // modules are fixed in main.cpp; code-wired children declare userEditable
    // false or carry a role no container accepts. See isUserEditableChild.
    if (isUserEditableChild(mod, depth)) {
        const actions = createActionButtons(mod);
        title.appendChild(actions);
    }

    // Help link → the module's spec page on the rendered docs site, far right of
    // the row. docPath comes from /api/types (relative to docs/moonmodules/, e.g.
    // "core/services.md#audio" or "light/effects.md#fire"); omitted if none.
    // The site is Material for MkDocs at moonmodules.org/projectMM/ (flat URLs, so
    // foo.md → foo.html; the MkDocs heading slugs match these #anchors), reached
    // via the same /projectMM/ subpath the installer uses. Convert only the `.md`
    // extension that sits right before the optional `#anchor` (suffix-anchored),
    // so a docPath that ever contained ".md" mid-string wouldn't be mangled.
    const docPath = docPathForType(mod.type);
    if (docPath) {
        const help = document.createElement("a");
        help.className = "card-help";
        help.textContent = "?";
        help.title = "Open module documentation";
        help.target = "_blank";
        help.rel = "noopener";
        const htmlPath = docPath.replace(/\.md(#.*)?$/, ".html$1");
        help.href = "https://moonmodules.org/projectMM/moonmodules/" + htmlPath;
        title.appendChild(help);
    }

    card.appendChild(title);

    // -- Controls --
    // Child-hosting modules deeper in the tree (Effects, Layer, Drivers, Layouts)
    // collapse their own controls so the children are the focus by default.
    // Modules that merely host a code-wired child (Network → Improv) keep their
    // controls expanded — the parent's settings are the main point, the code-wired
    // child is informational. Leaf modules render controls inline (no wrapper).
    // EXCEPTION: a top-level module (depth 0 — the selected root, e.g. System,
    // Network, or a container like Services/Drivers) never collapses its own
    // controls, even when it accepts children. It's the card the user is looking
    // at, so its settings should be visible, not hidden behind a "controls" disclosure.
    // Use the SAME predicate the row loop renders by (controlRendersGenerically), not a bare
    // !c.hidden — else the disclosure could open for a module whose only "visible" controls render
    // elsewhere (e.g. FileManager's filesystem/lastSaved render inside its own panel, not generically).
    const hasVisibleControls = mod.controls && mod.controls.some(c => controlRendersGenerically(mod, c));
    const wrapInDetails = depth > 0 && acceptsNewChildren(mod) && hasVisibleControls;
    const controlsHost = wrapInDetails ? (() => {
        const d = document.createElement("details");
        d.className = "card-controls-collapse";
        // Restore the open/closed state from localStorage, so a full-state rebuild (or a page reload) keeps
        // the expander as the user left it instead of snapping shut. Persist it on toggle — same pattern as
        // the selected tab (LS_TABS). VIEW-only state; nothing to do with the module's backend state.
        d.open = expandedSet.has(mod.name);
        d.addEventListener("toggle", () => {
            if (d.open) expandedSet.add(mod.name); else expandedSet.delete(mod.name);
            saveExpanded();
        });
        const s = document.createElement("summary");
        s.textContent = "controls";
        d.appendChild(s);
        card.appendChild(d);
        return d;
    })() : card;
    if (mod.status) {
        const row = document.createElement("div");
        row.className = "control-row";
        row.dataset.statusMid = mod.name;
        const label = document.createElement("span");
        label.className = "control-label";
        label.textContent = "status";
        const val = document.createElement("span");
        val.className = "status-value";
        val.dataset.sev = mod.severity || "status";
        val.textContent = mod.status;
        row.appendChild(label);
        row.appendChild(val);
        controlsHost.appendChild(row);
    }

    if (mod.controls) {
        for (const ctrl of mod.controls) {
            if (!controlRendersGenerically(mod, ctrl)) continue;
            const row = createControl(mod.name, mod.type, ctrl);
            if (row) controlsHost.appendChild(row);
        }
    }

    // File Manager: a modern expand/collapse folder tree + inline text editor. Browsing is UI-side
    // over /api/dir; a create/delete is a POST/DELETE /api/dir?path= call (the path rides the request,
    // no persisted control). Only the `show hidden` toggle renders as a raw control; the tree is the rest.
    if (mod.type === "FileManagerModule") {
        renderFileManager(mod, controlsHost);
    }

    // FirmwareUpdate card hosts the shared install picker. Mount once per
    // card-build. The picker reads SystemModule.firmware (already in
    // /api/state) to filter to OTA-compatible releases. On install, the
    // device fetches the binary via /api/firmware/url — no browser CORS in
    // the data path. See docs/architecture.md § Firmware vs board.
    if (mod.type === "FirmwareUpdateModule") {
        // Opening the Firmware card forces a fresh update check (the badge otherwise refreshes
        // only on the 1 h cache cadence) — so the badge agrees with the picker the user is about
        // to use. Fire-and-forget; best-effort.
        checkFirmwareUpdate(true);
        const ownFirmwareKey = (() => {
            // The `firmware` variant key is this module's own control now (moved here from
            // SystemModule), so read it straight off mod — no cross-module lookup.
            const fwCtrl = (mod.controls || []).find(c => c.name === "firmware");
            return fwCtrl && fwCtrl.value ? fwCtrl.value : null;
        })();
        const mount = document.createElement("div");
        mount.className = "install-picker-host";
        controlsHost.appendChild(mount);
        installPicker.init({
            container: mount,
            ownFirmwareKey,
            // Device already knows its deviceModel (SystemModule) — picker is for
            // releases + firmware compatibility only. Showing a board picker
            // here would invite the user to mis-narrow the firmware list.
            enableBoardPicker: false,
            onInstall: async (_firmware, _manifestUrl, binaryUrl) => {
                const res = await fetch("/api/firmware/url", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({ url: binaryUrl }),
                });
                if (!res.ok) throw new Error(await errorMessage(res));
            },
        });

        // Install-from-file: pick a locally-built .bin and stream its bytes to /api/firmware/upload,
        // which feeds platform::otaWriteStream (esp_ota_write). Complements the release picker above —
        // the release path has the device fetch a URL; this path has the browser push the body, so a
        // dev build that isn't a published release can be flashed straight over the network. The device
        // reboots on success (no response body), so a completed POST that closes mid-flight is success.
        const fileRow = document.createElement("div");
        fileRow.className = "fw-upload-row";
        const upBtn = document.createElement("button");
        upBtn.className = "fm-tool fm-tool--icon";
        upBtn.textContent = "↥";   // same upload glyph as the file-manager toolbar
        upBtn.title = "Install from file — flash a firmware .bin from your computer over the network (OTA)";
        const upStatus = document.createElement("span");
        upStatus.className = "fw-upload-status";
        const upInput = document.createElement("input");
        upInput.type = "file";
        upInput.accept = ".bin,application/octet-stream";
        upInput.style.display = "none";
        upInput.addEventListener("change", async () => {
            const file = (upInput.files || [])[0];
            upInput.value = "";                   // reset so re-picking the same file re-fires change
            if (!file) return;
            upBtn.disabled = true;
            upStatus.textContent = `uploading ${fmSize(file.size)}…`;
            try {
                const res = await fetch("/api/firmware/upload", {
                    method: "POST", headers: { "Content-Type": "application/octet-stream" }, body: file,
                });
                // The device sends a 200 once the image is committed, THEN reboots — so res.ok is the
                // real success signal. A 4xx/5xx carries the device's ota error in the JSON body.
                if (res.ok) upStatus.textContent = "flashed — device rebooting";
                else throw new Error(await errorMessage(res));
            } catch (err) {
                // A network error mid-upload is a genuine failure now (the device answers before it
                // reboots), so surface it as such rather than assuming a reboot.
                upStatus.textContent = "upload failed: " + err.message;
            } finally {
                upBtn.disabled = false;
            }
        });
        upBtn.addEventListener("click", () => upInput.click());
        fileRow.appendChild(upBtn);
        fileRow.appendChild(upStatus);
        fileRow.appendChild(upInput);
        controlsHost.appendChild(fileRow);
    }

    // -- Children block + footer --
    // The .card-children wrapper lives inside this card so the parent's border
    // encloses its children; renderModuleTree recurses into it. The "+ add module"
    // footer only appears on parents that accept user-created children — a parent
    // hosting only code-wired children (e.g. Network → Improv) renders the
    // children block but no add button.
    let childrenEl = null;
    if (hasNestedChildren(mod)) {
        childrenEl = document.createElement("div");
        childrenEl.className = "card-children";
        childrenEl.dataset.depth = String(depth + 1);
        card.appendChild(childrenEl);

        if (acceptsNewChildren(mod)) {
            // -- Footer: + add module --
            const footer = document.createElement("div");
            footer.className = "card-footer";
            const addBtn = document.createElement("button");
            addBtn.className = "add-btn";
            addBtn.textContent = "+ add module";
            addBtn.addEventListener("click", () => {
                // Hide the button while the picker is open (the picker takes its
                // place); restore it once the picker is removed (cancel/create/Esc).
                addBtn.style.display = "none";
                openTypePicker(mod, footer);
                const obs = new MutationObserver(() => {
                    if (!footer.querySelector(".type-picker")) {
                        addBtn.style.display = "";
                        obs.disconnect();
                    }
                });
                obs.observe(footer, {childList: true});
            });
            footer.appendChild(addBtn);
            card.appendChild(footer);
        }
    }

    // -- Drag-to-reorder (HTML5 DnD on desktop; touchstart-gated on mobile) --
    // Same gate as the delete/replace buttons: a user-managed child is also
    // reorderable within its parent.
    if (isUserEditableChild(mod, depth)) {
        attachDragHandlers(card, mod);
    }

    return { card, childrenEl };
}

// Wire a button as a press-twice confirm: the first click arms it (adds the
// `armed` class, optional armed label/title), a second click runs `onConfirm`.
// Disarms after 3s or when the pointer leaves. Used by the delete and reboot
// buttons — no browser confirm() popup. `armedText` is optional (delete swaps
// × → ✓; reboot keeps its glyph). The pre-arm title is captured live so a
// title updated elsewhere (e.g. reboot's crashed-state text) restores correctly.
function armPressTwice(btn, onConfirm, opts = {}) {
    let armed = false;
    let disarmTimer = null;
    let savedText = "";
    let savedTitle = "";
    const disarm = () => {
        // Only restore label/title if we actually armed — otherwise a mouseleave before the first
        // click would write the initial empty savedText/savedTitle over the button, collapsing a
        // text-sized button (e.g. "🗑 delete") to an empty sliver.
        if (!armed) return;
        armed = false;
        btn.classList.remove("armed");
        if (opts.armedText !== undefined) btn.textContent = savedText;
        btn.title = savedTitle;
        if (disarmTimer) { clearTimeout(disarmTimer); disarmTimer = null; }
    };
    btn.addEventListener("click", () => {
        // Disarm before running the action so a stray second click can't fire
        // it twice (e.g. two /api/reboot requests).
        if (armed) { disarm(); onConfirm(); return; }
        armed = true;
        savedText = btn.textContent;
        savedTitle = btn.title;
        btn.classList.add("armed");
        if (opts.armedText !== undefined) btn.textContent = opts.armedText;
        if (opts.armedTitle !== undefined) btn.title = opts.armedTitle;
        disarmTimer = setTimeout(disarm, 3000);
    });
    btn.addEventListener("mouseleave", disarm);
}

// Compact byte formatter — "B" under 1 KB, "KB" otherwise (one decimal under 10 KB).
function fmtBytes(n) {
    if (n < 1024) return n + "B";
    const k = n / 1024;
    return (k < 10 ? k.toFixed(1) : Math.round(k)) + "KB";
}

// Stats line: timing (🕒, fps or µs/ms per the global toggle) + memory
// (🧠 static, plus "+ dynamic" only when the module allocated heap).
// Timing is omitted entirely when the module has no measured loop time.
function formatStats(mod) {
    const us = (mod.tickTimeUs !== undefined) ? mod.tickTimeUs : 0;
    let timing = "";
    if (us > 0) {
        if (timingMode === "fps") {
            const fps = Math.round(1_000_000 / us);
            timing = "🕒 " + (fps >= 1000 ? Math.round(fps / 1000) + "K fps" : fps + " fps");
        } else {
            timing = "🕒 " + (us < 1000 ? us + " µs" : (us / 1000).toFixed(2) + " ms");
        }
    }
    const stat = mod.classSize || 0;
    const dyn = mod.dynamicBytes || 0;
    const mem = "🧠 " + fmtBytes(stat) + (dyn > 0 ? " + " + fmtBytes(dyn) : "");
    // Status chip: emitted by the engine when a module has something to say.
    // Severity picks the emoji — ℹ️ neutral (Eth: 192.168.1.210), ⚠️ degraded
    // (buffer reduced), ❌ error (No network). Tooltip carries the full text.
    const sev = mod.severity || "status";
    const sevEmoji = sev === "error" ? "❌" : sev === "warning" ? "⚠️" : "ℹ️";
    const statusChip = mod.status ? "  " + sevEmoji : "";
    const head = timing ? timing + "   " + mem : mem;
    return head + statusChip;
}
function formatStatsTitle(mod) {
    return "Click to toggle fps/ms";
}

function createActionButtons(mod) {
    const wrap = document.createElement("span");
    wrap.className = "card-actions";

    // Reorder is drag-and-drop only (works on desktop and mobile). The whole
    // card body is the drag source; the controls region excludes itself via
    // the mousedown gate in attachDragHandlers. No up/down buttons, no
    // dedicated drag handle.

    const replaceBtn = document.createElement("button");
    replaceBtn.className = "card-btn";
    replaceBtn.textContent = "✎";
    replaceBtn.title = "Replace with another type";
    replaceBtn.addEventListener("click", () => {
        // Anchor the picker to the card so it drops below the card content,
        // not inside the cramped 26px action-button row.
        openReplacePicker(mod, replaceBtn.closest(".card"));
    });
    wrap.appendChild(replaceBtn);

    // Delete: press × once to arm, again to confirm — see armPressTwice.
    const delBtn = document.createElement("button");
    delBtn.className = "card-btn card-btn-del";
    delBtn.textContent = "×";
    delBtn.title = "Delete";
    armPressTwice(delBtn, () => deleteModule(mod.name),
                  {armedText: "✓", armedTitle: "Click again to delete"});
    wrap.appendChild(delBtn);

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

// Whether this module renders any nested children at all (a "+ add module"
// button included if it also accepts new ones via the UI). True whenever the
// module has at least one child today OR is one of the light-pipeline
// containers that users can add to. This lets code-wired children (e.g.
// ImprovProvisioning under Network) render without making the parent UI-addable.
function hasNestedChildren(mod) {
    return (mod.children && mod.children.length > 0) || acceptsNewChildren(mod);
}

// Roles this parent accepts as user-added children, from the device's
// `acceptsChildRoles` (per-type in /api/types — e.g. Layer → "effect,modifier").
// Domain-neutral: the UI no longer hardcodes which module types are containers;
// the device declares it via MoonModule::acceptsChildRoles(). "" → [] (accepts
// none), which is also the default for modules whose type isn't loaded yet.
function rolesAcceptedBy(parentMod) {
    const t = availableTypes.find(t => t.name === parentMod.type);
    const csv = (t && t.acceptsChildRoles) ? t.acceptsChildRoles : "";
    return csv ? csv.split(",") : [];
}

// Whether the "+ add module" affordance applies — derived from acceptsChildRoles
// being non-empty, so there's a single source of truth (no separate list).
function acceptsNewChildren(mod) {
    return rolesAcceptedBy(mod).length > 0;
}

// The set of child roles ANY loaded type accepts — the union of every type's
// acceptsChildRoles. A module is "user-managed as a child" iff its role is in
// this set, which is how the UI decides to show delete/replace/drag without
// hardcoding role names. Code-wired children (ImprovProvisioning,
// — roles no container declares) correctly fall outside it.
function allAcceptedChildRoles() {
    const roles = new Set();
    for (const t of availableTypes) {
        const csv = t.acceptsChildRoles || "";
        if (csv) csv.split(",").forEach(r => roles.add(r));
    }
    return roles;
}

// Whether the UI shows delete / replace / drag for this module. True when it's
// a nested module (depth > 0) whose role is one some container accepts AND it
// hasn't opted out via the device's userEditable=false (e.g. PreviewDriver).
// Replaces the old hardcoded `role === "effect" || "modifier"` gate — now any
// add-accepted role (driver, layout, …) is editable, and the child itself can
// veto via userEditable.
//
// We test mod.role against the UNION of all containers' acceptsChildRoles, not
// against this module's specific parent. That's exact while the role→container
// mapping is 1:1 (effect→Layer, driver→Drivers, layout→Layouts, layer→Effects) —
// a child of an add-accepted role is always under the one container that
// accepts it. If a role ever becomes accepted by more than one container, this
// would need the parent threaded in to scope the check to the actual parent.
function isUserEditableChild(mod, depth) {
    return depth > 0
        && mod.userEditable !== false
        && allAcceptedChildRoles().has(mod.role);
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

// The module type's spec-page path (relative to docs/moonmodules/), from /api/types.
// Returns "" when the type isn't loaded yet or declares no doc path.
function docPathForType(moduleType) {
    if (!moduleType) return "";
    const t = availableTypes.find(t => t.name === moduleType);
    return (t && t.docPath) ? t.docPath : "";
}

// Curated emoji string for a live module — its role emoji plus the type's
// `tags` from /api/types, deduplicated, in role-first order. "" if the type
// isn't loaded yet. Used on the card title and in the type picker.
function emojiTagsForMod(mod) {
    if (!mod) return "";
    const t = availableTypes.find(t => t.name === mod.type) || {role: mod.role, tags: ""};
    return emojiTagsFor(t).join("");
}

// Whether a control appears in the generic control list — false for controls the module marked
// `hidden`. A module that renders a control in its own panel instead (e.g. the File Manager's
// `filesystem` usage bar + `lastSaved` readout, drawn by renderFileManager) sets the hidden flag on
// the C++ side, so the generic list skips it and the value never shows twice. Used by BOTH render
// paths (renderCards's initial build + updateModuleControls's WS live-patch) so they agree.
function controlRendersGenerically(mod, ctrl) {
    if (ctrl.hidden) return false;
    if (ctrl.advanced && !isExpertMode()) return false;   // expert-only control, expert mode is off
    return true;
}

function createControl(moduleName, moduleType, ctrl) {
    const row = document.createElement("div");
    row.className = "control-row";
    // Expert-only controls (only reachable here when expert mode is on — see controlRendersGenerically)
    // get a distinct treatment so they read as a different tier: a left accent stripe + muted label.
    if (ctrl.advanced) row.classList.add("control-advanced");
    row.dataset.key = ctrl.name;

    const label = document.createElement("label");
    label.className = "control-label";
    // The expertMode toggle itself carries the wrench glyph (via CSS ::before), so it reads as the switch
    // that governs the 🔧 controls — the toggle is never `advanced` (it must always be reachable), so key
    // it by name rather than the flag.
    if (moduleName === "System" && ctrl.name === "expertMode") label.classList.add("control-label--expert");
    label.textContent = ctrl.name;
    row.appendChild(label);

    const key = moduleName + ":" + ctrl.name;
    const def = defaultFor(moduleType, ctrl.name);

    // numberField: a numeric control that opted out of the slider (server sets it for a value where each
    // integer is a discrete identity, not a magnitude — a PHY/I2C address, a channel). Render a plain
    // number input, same shape as the `pin` case, whatever the underlying numeric type. The WS-patch path
    // (updateModuleControls) reads the input by [data-mid][data-key] the same way, so no extra patch case.
    const isNumericType = ctrl.type === "uint8" || ctrl.type === "uint16" || ctrl.type === "int16";
    if (ctrl.numberField && isNumericType) {
        const nMin = Number(ctrl.min ?? 0);
        const nMax = Number(ctrl.max ?? 65535);
        const input = document.createElement("input");
        input.type = "number";
        input.min = nMin;
        input.max = nMax;
        input.value = ctrl.value ?? 0;
        input.dataset.mid = moduleName;
        input.dataset.key = ctrl.name;
        input.addEventListener("input", () => {
            dragTs[key] = Date.now();
            let v = parseInt(input.value, 10);
            if (Number.isNaN(v)) return;   // mid-edit empty field: send nothing until digits arrive
            v = Math.max(nMin, Math.min(nMax, v));
            if (String(v) !== input.value) input.value = v;   // display always matches what's sent
            debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, v));
        });
        input.addEventListener("change", () => {   // blur/Enter with a still-empty field: snap to min + send
            if (Number.isNaN(parseInt(input.value, 10))) {
                dragTs[key] = Date.now();
                input.value = nMin;
                debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, nMin));
            }
        });
        row.appendChild(input);
        appendResetButton(row, moduleName, ctrl, def, () => { input.value = def; });
        return row;
    }

    switch (ctrl.type) {
        case "uint8": {
            const input = document.createElement("input");
            input.type = "range";
            // A fader is the same range input stood on end (CSS `writing-mode: vertical-lr`), so the
            // value, the range and the WS live-update path are untouched — only the orientation and
            // the row's layout change. Consecutive faders sit side by side as one bank.
            // An encoder is the same range input drawn as a KNOB: the input itself is hidden and an
            // SVG dial sits on top, so the value, the range, the WS update path and keyboard access
            // are the range input's — only the appearance changes. Dragging vertically turns it,
            // which is the gesture a real encoder takes.
            if (ctrl.encoder) {
                input.classList.add("encoder-input");
                row.classList.add("control-encoder");
                row.appendChild(buildKnob(input, ctrl));
                row.appendChild(buildSevenSeg(input));
                attachTargetPopup(row, input, ctrl);
            }
            if (ctrl.fader) {
                input.classList.add("fader-input");
                row.classList.add("control-fader");
                row.appendChild(buildSevenSeg(input));
                // Right-click a fader to see (and later choose) what it drives — the same
                // configure-on-the-control rule the pads follow.
                attachTargetPopup(row, input, ctrl);
            }
            input.min = ctrl.min ?? 0;
            input.max = ctrl.max ?? 255;
            input.value = ctrl.value ?? 0;
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            // The knob and the readout were built above, before these three lines ran, so they drew
            // against an empty input (the source of the 880 readouts). Refresh now that the value and
            // the bounds exist.
            redrawRangeDecorations(input);
            const numInput = document.createElement("input");
            numInput.type = "number";
            numInput.className = "control-value-input";
            numInput.min = input.min;
            numInput.max = input.max;
            numInput.value = input.value;
            input.addEventListener("input", () => {
                dragTs[key] = Date.now();
                numInput.value = input.value;
                debounceSend(key, 150, () => sendControl(moduleName, ctrl.name, parseInt(input.value)));
            });
            numInput.addEventListener("input", () => {
                dragTs[key] = Date.now();   // stamp so a WS push can't revert what's being typed
                const v = Math.max(Number(input.min), Math.min(Number(input.max), parseInt(numInput.value) || 0));
                input.value = v;
                debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, v));
            });
            row.appendChild(input);
            row.appendChild(numInput);
            appendResetButton(row, moduleName, ctrl, def, () => {
                input.value = def;
                numInput.value = def;
            });
            break;
        }
        case "uint16": {
            // Bounded (server sent an explicit max below the type ceiling) →
            // slider, like uint8/int16. Unbounded (max == 65535, the default for
            // port/universe-style values with no natural range) → plain number.
            const uMin = Number(ctrl.min ?? 0);
            const uMax = Number(ctrl.max ?? 65535);
            if (uMax < 65535) {
                const input = document.createElement("input");
                input.type = "range";
                input.min = uMin;
                input.max = uMax;
                input.value = Math.max(uMin, Math.min(uMax, Number(ctrl.value ?? 0)));
                input.dataset.mid = moduleName;
                input.dataset.key = ctrl.name;
                const numInput = document.createElement("input");
                numInput.type = "number";
                numInput.className = "control-value-input";
                numInput.min = uMin;
                numInput.max = uMax;
                numInput.value = input.value;
                input.addEventListener("input", () => {
                    dragTs[key] = Date.now();
                    numInput.value = input.value;
                    debounceSend(key, 150, () => sendControl(moduleName, ctrl.name, parseInt(input.value)));
                });
                numInput.addEventListener("input", () => {
                    dragTs[key] = Date.now();   // stamp so a WS push can't revert what's being typed
                    const v = Math.max(uMin, Math.min(uMax, parseInt(numInput.value) || 0));
                    input.value = v;
                    debounceSend(key, 150, () => sendControl(moduleName, ctrl.name, v));
                });
                row.appendChild(input);
                row.appendChild(numInput);
                appendResetButton(row, moduleName, ctrl, def, () => {
                    input.value = def; numInput.value = def;
                });
            } else {
                const input = document.createElement("input");
                input.type = "number";
                input.value = ctrl.value ?? 0;
                input.dataset.mid = moduleName;
                input.dataset.key = ctrl.name;
                input.addEventListener("input", () => {
                    dragTs[key] = Date.now();
                    // Sanitise: empty/garbage → 0, clamp into the uint16 range so a
                    // NaN or out-of-range value never reaches the device.
                    let v = parseInt(input.value, 10);
                    if (Number.isNaN(v)) v = 0;
                    v = Math.max(0, Math.min(65535, v));
                    debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, v));
                });
                row.appendChild(input);
                appendResetButton(row, moduleName, ctrl, def, () => { input.value = def; });
            }
            break;
        }
        case "pin": {
            // A GPIO pin: plain number input, never a slider (a pin has no range to
            // drag). −1 = unused. ctrl.min/max are the valid-GPIO span used only to
            // clamp the typed value before sending.
            const pMin = Number(ctrl.min ?? -1);
            const pMax = Number(ctrl.max ?? 52);
            const input = document.createElement("input");
            input.type = "number";
            input.min = pMin;
            input.max = pMax;
            input.value = ctrl.value ?? -1;
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            input.addEventListener("input", () => {
                dragTs[key] = Date.now();
                let v = parseInt(input.value, 10);
                if (Number.isNaN(v)) v = -1;
                v = Math.max(pMin, Math.min(pMax, v));
                debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, v));
            });
            row.appendChild(input);
            appendResetButton(row, moduleName, ctrl, def, () => { input.value = def; });
            break;
        }
        case "int16": {
            // ctrl.min/ctrl.max are always present (server sends them). Sentinel
            // values INT16_MIN (-32768) / INT16_MAX (32767) mean "unbounded" —
            // fall back to a ±percentage range.
            const rawMin = Number(ctrl.min ?? -32768);
            const rawMax = Number(ctrl.max ?? 32767);
            const min = rawMin <= -32768 ? -100 : rawMin;
            const max = rawMax >= 32767  ?  200 : rawMax;
            const raw = Number(ctrl.value ?? 0);
            const clamped = Math.max(min, Math.min(max, raw));
            const input = document.createElement("input");
            input.type = "range";
            input.min = min;
            input.max = max;
            input.value = clamped;
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            const numInput = document.createElement("input");
            numInput.type = "number";
            numInput.className = "control-value-input";
            numInput.min = min;
            numInput.max = max;
            numInput.value = input.value;
            input.addEventListener("input", () => {
                dragTs[key] = Date.now();
                numInput.value = input.value;
                debounceSend(key, 150, () => sendControl(moduleName, ctrl.name, parseInt(input.value)));
            });
            numInput.addEventListener("input", () => {
                dragTs[key] = Date.now();   // stamp so a WS push can't revert what's being typed
                const v = Math.max(min, Math.min(max, parseInt(numInput.value) || 0));
                input.value = v;
                debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, v));
            });
            row.appendChild(input);
            row.appendChild(numInput);
            appendResetButton(row, moduleName, ctrl, def, () => {
                input.value = def;
                numInput.value = def;
            });
            break;
        }
        case "bool": {
            // Modern on/off switch: an <input type=checkbox> (kept for the
            // existing change/sync paths) wrapped in a <label> that draws a
            // pill-shaped track + sliding thumb via CSS. The checkbox itself
            // is visually hidden but stays the source of truth.
            const sw = document.createElement("label");
            sw.className = "switch";
            const input = document.createElement("input");
            input.type = "checkbox";
            input.checked = !!ctrl.value;
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            input.addEventListener("change", () => {
                dragTs[key] = Date.now();
                sendControl(moduleName, ctrl.name, input.checked);
            });
            const track = document.createElement("span");
            track.className = "switch-track";
            sw.appendChild(input);
            sw.appendChild(track);
            row.appendChild(sw);
            appendResetButton(row, moduleName, ctrl, def, () => { input.checked = !!def; });
            break;
        }
        case "text": {
            const input = document.createElement("input");
            input.type = "text";
            input.value = ctrl.value ?? "";
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            // ctrl.readonly is a UI hint (the server still accepts writes — the
            // flag exists for values pushed by tooling like MoonDeck or the
            // web installer, where editing in the device UI is by-convention
            // disallowed). `readOnly` lets the user select/copy the text but
            // not modify it; `disabled` would also block copy.
            if (ctrl.readonly) {
                input.readOnly = true;
            } else {
                input.addEventListener("input", () => {
                    dragTs[key] = Date.now();
                    debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, input.value));
                });
            }
            row.appendChild(input);
            break;
        }
        case "textarea": {
            // Multi-line text (e.g. a script source). A resizable <textarea>; the
            // value syncs and debounces exactly like a "text" control.
            const input = document.createElement("textarea");
            input.className = "control-textarea";
            input.value = ctrl.value ?? "";
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            input.rows = 2;            // default 2 lines; CSS height + resize grip control size
            input.spellcheck = false;
            // Restore a previously dragged height (view-state, see textareaSizes). A stored px height
            // overrides the 2-row default; an untouched textarea has no entry and keeps the default.
            const savedH = textareaSizes[key];
            if (typeof savedH === "number" && savedH > 0) input.style.height = savedH + "px";
            // Persist the height whenever the user drags the resize grip. A <textarea> has no native resize
            // event, so a ResizeObserver is the standard way to observe it; store the pixel height keyed by
            // "<module>:<control>". Use the observed height (not the inline style, which the initial observe
            // fire and a layout-driven change wouldn't set), track the last saved value, and write only on a
            // real change — so the initial fire and idle re-observations don't thrash localStorage. rAF-
            // coalesced so a drag saves once per frame at most.
            let taRaf = 0, taPrevH = Math.round(savedH > 0 ? savedH : 0);
            const taObserver = new ResizeObserver((entries) => {
                const h = Math.round(entries[0].contentRect.height);
                if (taRaf || h <= 0 || h === taPrevH) return;
                taRaf = requestAnimationFrame(() => { taRaf = 0; taPrevH = h; saveTextareaSize(key, h); });
            });
            taObserver.observe(input);
            if (ctrl.readonly) {
                input.readOnly = true;
            } else {
                input.addEventListener("input", () => {
                    dragTs[key] = Date.now();
                    debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, input.value));
                });
            }
            row.appendChild(input);
            break;
        }
        case "password": {
            // ctrl.value arrives XOR-obfuscated + base64-encoded (see
            // HttpServerModule PASSWORD_XOR_KEY). Decode it so the input holds
            // the real stored password — masked by the password input, revealed
            // by hold-to-peek. The obfuscation is trivially reversible by design.
            const input = document.createElement("input");
            input.type = "password";
            input.value = decodePassword(ctrl.value);
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            input.addEventListener("input", () => {
                dragTs[key] = Date.now();
                debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, input.value));
            });
            row.appendChild(input);
            // Hold-to-peek button — reveals the stored password.
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
            // Protect the dropdown while the user has it open. A native <select>
            // popup stays open for several frames (seconds, if deliberating) while
            // a continuously-refreshed module keeps pushing state over the WS; an
            // unguarded `sel.value = ctrl.value` patch during that window snaps the
            // menu back to the old option and visibly closes it — the user never
            // gets to pick. We mark the select "open" on pointerdown (fires BEFORE
            // the popup opens, unlike focus, which some browsers delay or skip) and
            // clear it on change/blur; updateModuleControls skips any select marked
            // open. pointerdown also stamps the dragTs cooldown as a belt-and-braces
            // fallback for the post-close frames.
            sel.dataset.open = "false";
            const markOpen = () => { sel.dataset.open = "true"; dragTs[key] = Date.now(); };
            sel.addEventListener("pointerdown", markOpen);
            sel.addEventListener("focus", markOpen);
            sel.addEventListener("blur", () => { sel.dataset.open = "false"; });
            sel.addEventListener("change", () => {
                sel.dataset.open = "false";
                dragTs[key] = Date.now();
                sendControl(moduleName, ctrl.name, parseInt(sel.value));
                // No refetch/re-render here: blendMode/opacity-style selects don't
                // change the control SET, and a control that does (a hidden-flag
                // flip) is reconciled in place by syncVisibleControls on the next
                // WS push — so the card (and its expanded state) is preserved.
                // A full refetchState() rebuilt the DOM and collapsed the card.
            });
            row.appendChild(sel);
            appendResetButton(row, moduleName, ctrl, def, () => { sel.value = def; });
            break;
        }
        case "palette": {
            // A color-palette dropdown where EVERY option shows its own gradient — so the colors
            // are visible before selecting, not just after. A native <select> can't do this
            // (browsers ignore a gradient background on <option>, and the macOS popup is OS-drawn),
            // so this is a custom dropdown: a trigger button (selected swatch + name + caret) that
            // toggles a list of styled rows, each a gradient swatch + name. The value still rides as
            // the option index. Prior art: MoonLight's palette control (same native-select limit).
            const opts = ctrl.options || [];
            const gradientFor = (i) => {
                const cols = (opts[i] || {}).colors || "";
                const stops = cols.split(/\s+/).filter(Boolean).map(h => "#" + h);
                return stops.length ? `linear-gradient(to right, ${stops.join(",")})` : "none";
            };
            const wrap = document.createElement("div");
            wrap.className = "palette-control";
            wrap.dataset.mid = moduleName;
            wrap.dataset.key = ctrl.name;
            wrap.dataset.value = ctrl.value;

            // Trigger: shows the currently-selected palette (swatch + name) and opens the list.
            const trigger = document.createElement("button");
            trigger.type = "button";
            trigger.className = "palette-trigger";
            const triSwatch = document.createElement("span");
            triSwatch.className = "palette-swatch";
            const triName = document.createElement("span");
            triName.className = "palette-name";
            const caret = document.createElement("span");
            caret.className = "palette-caret";
            caret.textContent = "▾";
            const paintTrigger = (i) => {
                triSwatch.style.background = gradientFor(i);
                triName.textContent = (opts[i] || {}).name || String(i);
            };
            paintTrigger(ctrl.value);
            trigger.append(triSwatch, triName, caret);

            // The list of gradient rows, hidden until the trigger is clicked.
            const list = document.createElement("div");
            list.className = "palette-list";
            list.hidden = true;
            opts.forEach((opt, i) => {
                const item = document.createElement("button");
                item.type = "button";
                item.className = "palette-item" + (i === ctrl.value ? " selected" : "");
                item.dataset.idx = i;
                const sw = document.createElement("span");
                sw.className = "palette-swatch";
                sw.style.background = gradientFor(i);
                const nm = document.createElement("span");
                nm.className = "palette-name";
                nm.textContent = opt.name || String(i);
                item.append(sw, nm);
                item.addEventListener("click", () => {
                    wrap.dataset.value = i;
                    paintTrigger(i);
                    list.querySelectorAll(".selected").forEach(x => x.classList.remove("selected"));
                    item.classList.add("selected");
                    closeList();
                    dragTs[key] = Date.now();
                    sendControl(moduleName, ctrl.name, i);
                });
                list.appendChild(item);
            });

            // Open/close, dismissing on outside-click or Escape (the type-picker pattern).
            let onDocClick = null;
            const onKey = (e) => { if (e.key === "Escape") closeList(); };
            const closeList = () => {
                list.hidden = true;
                wrap.dataset.open = "false";
                if (onDocClick) {
                    document.removeEventListener("pointerdown", onDocClick);
                    document.removeEventListener("keydown", onKey);
                    onDocClick = null;
                }
            };
            const openList = () => {
                list.hidden = false;
                wrap.dataset.open = "true";
                dragTs[key] = Date.now();
                // Bring the selected row into view (a long palette list may overflow the popup).
                const sel = list.querySelector(".palette-item.selected");
                if (sel) sel.scrollIntoView({ block: "nearest" });
                onDocClick = (e) => { if (!wrap.contains(e.target)) closeList(); };
                document.addEventListener("pointerdown", onDocClick);
                document.addEventListener("keydown", onKey);
            };
            trigger.addEventListener("click", () => { list.hidden ? openList() : closeList(); });

            wrap.append(trigger, list);
            row.appendChild(wrap);
            // Reset to the default palette like every other persisted control: re-paint the trigger
            // and the selected row to `def` (sendControl is handled by appendResetButton).
            appendResetButton(row, moduleName, ctrl, def, () => {
                wrap.dataset.value = def;
                paintTrigger(def);
                list.querySelectorAll(".selected").forEach(x => x.classList.remove("selected"));
                const r = list.querySelector(`.palette-item[data-idx="${def}"]`);
                if (r) r.classList.add("selected");
            });
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
        case "display-int": {
            // Read-only signed int with a unit suffix (e.g. "-58 dBm").
            // ctrl.unit is the suffix the device chose at addReadOnlyInt time.
            const span = document.createElement("span");
            span.className = "display";
            span.dataset.mid = moduleName;
            span.dataset.key = ctrl.name;
            span.dataset.kind = "display-int";
            span.dataset.unit = ctrl.unit ?? "";
            span.textContent = fmtDisplayInt(ctrl);
            row.appendChild(span);
            break;
        }
        case "ipv4": {
            // Editable dotted-quad. Wire format is the same string the user
            // types — the device parses + validates server-side and rejects
            // malformed values with 400. Inline validation on the client is
            // a future enhancement; today an invalid value goes to the
            // server and the response surfaces the rejection.
            //
            // Same dragTs + debounceSend pattern as text / password so the
            // ipv4 input participates in stale-WS-push protection: while
            // the user is typing, dragTs[key] gets bumped, and an arriving
            // WS push within the cooldown window won't revert mid-edit
            // (see updateValues + the dragTs check ~line 1260).
            const input = document.createElement("input");
            input.type = "text";
            input.className = "ipv4-input";
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            input.value = ctrl.value ?? "";
            input.placeholder = "0.0.0.0";
            input.maxLength = 15;  // "255.255.255.255" = 15
            input.addEventListener("input", () => {
                dragTs[key] = Date.now();
                debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, input.value));
            });
            row.appendChild(input);
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
        case "list": {
            // A generic read-only list (ControlType::List). value = array of row
            // summary objects; ctrl.detail = parallel array of detail objects (same
            // order). Render one clickable row per summary; clicking toggles a detail
            // panel below it. Self rows (summary.self === true) get a marker. Fully
            // generic — the engine decides the fields; the UI just renders objects.
            row.classList.add("control-list-row");
            const rows = Array.isArray(ctrl.value) ? ctrl.value : [];
            const details = Array.isArray(ctrl.detail) ? ctrl.detail : [];
            const list = document.createElement("div");
            list.className = "list-control";
            list.dataset.mid = moduleName;
            list.dataset.key = ctrl.name;
            // When ctrl.editable is set, the list gains add/delete/reorder + inline field
            // editing (the editable-List primitive); otherwise it stays read-only (identical
            // to before). The flag flows through to both render paths via the opts object.
            if (ctrl.editable) list.dataset.editable = "true";
            buildListEntries(list, rows, details, new Set(),   // initial: nothing expanded
                {editable: ctrl.editable, pads: ctrl.pads, gridCols: ctrl.gridCols, gridRows: ctrl.gridRows,
                 moduleName: moduleName, ctrlName: ctrl.name, optionSets: ctrl.optionSets || {}});
            row.appendChild(list);
            break;
        }
        default:
            // Unknown control type — skip silently. New types may be added engine-side
            // without breaking the UI; they just don't render until handled here.
            return null;
    }

    return row;
}

// Rebuild a List control's entries inside `container` from `rows` (summary objects)
// and `details` (parallel detail objects). `openSet` is a Set<string> of summary
// texts whose detail panels start expanded — pass `new Set()` for a fresh build, or
// the currently-open set on a live re-render so an expanded row stays open. Shared by
// createControl (initial) and updateModuleControls (WS live patch) so the two can't drift.
//
// `opts` optionally carries {editable, moduleName, ctrlName}. When editable is true the
// list gains row add/delete/reorder + inline field editing (the editable-List primitive);
// each row must then carry a numeric `id`, and each detail an array `fields[]` of field
// descriptors. When editable is falsy the render is exactly the read-only path (unchanged).
// Stable key for the open-detail set. An editable row carries a durable `id` (survives
// field edits: renaming a preset or changing its channel count doesn't change the id), so
// key on it — otherwise changing a field that shows in the summary label would fold the
// open row shut (the label-text key no longer matches after the rebuild). A read-only row
// (e.g. a discovered device) has no `id`; fall back to the summary label, which is stable
// enough there (identity by display text, the previous behaviour, unchanged for devices).
function listRowKey(item, labelText) {
    return (item && item.id != null) ? "#" + item.id : labelText;
}

// A grid of uniform pads, one per row: the presentation a `pads` list asks for. Rows are unchanged
// — this is the same data the stacked list renders, laid out for triggering rather than reading.
// Uniform size on purpose: sizing each pad to its label (as MoonLight does) makes a ragged grid that
// is harder to scan and to hit; a fixed pad with a truncated label is what a MIDI deck looks like.
// Long-press as the touch equivalent of a right-click: a touch device has no second button, so the
// configure gesture has to come from somewhere. 500ms, cancelled by movement so it never fires
// during a drag.
function attachLongPress(el, fn) {
    let timer = 0;
    const cancel = () => { if (timer) { clearTimeout(timer); timer = 0; } };
    el.addEventListener("touchstart", () => {
        cancel();
        timer = setTimeout(() => { timer = 0; fn(); }, 500);
    }, {passive: true});
    el.addEventListener("touchmove", cancel, {passive: true});
    el.addEventListener("touchend", cancel);
    el.addEventListener("touchcancel", cancel);
}

// A small popup anchored to a surface control. Floating rather than appended to the anchor: a panel
// inside the grid would reflow the pads underneath it, and a surface must not move while you are
// working on it. Dismisses on click-away, Escape, or opening another.
function openSurfacePopup(anchorEl, title, build) {
    document.querySelectorAll(".surface-popup").forEach(p => p.remove());
    const pop = document.createElement("div");
    pop.className = "surface-popup";

    const head = document.createElement("div");
    head.className = "surface-popup-title";
    head.textContent = title;
    pop.appendChild(head);

    const body = document.createElement("div");
    body.className = "surface-popup-body";
    pop.appendChild(body);
    // One teardown for every exit path (click-away, Escape, or a button calling close()): removing
    // the popup without detaching the document listeners leaked a pair per open.
    let away = null, esc = null;
    const close = () => {
        pop.remove();
        if (away) document.removeEventListener("mousedown", away);
        if (esc) document.removeEventListener("keydown", esc);
    };
    build(body, close);

    document.body.appendChild(pop);
    // Anchor below-right of the control, then pull back inside the viewport if that would overflow.
    const r = anchorEl.getBoundingClientRect();
    pop.style.left = `${Math.min(r.left, window.innerWidth - pop.offsetWidth - 8)}px`;
    pop.style.top = `${Math.min(r.bottom + 4, window.innerHeight - pop.offsetHeight - 8)}px`;

    // Defer so the click that opened it does not immediately close it.
    setTimeout(() => {
        away = (e) => { if (!pop.contains(e.target)) close(); };
        esc = (e) => { if (e.key === "Escape") close(); };
        document.addEventListener("mousedown", away);
        document.addEventListener("keydown", esc);
    }, 0);
    return pop;
}

// The edit form behind a right-click on a pad: rename, choose what it captures, save over it, or
// delete. These are the SAME operations the card's bottom controls performed, moved onto the thing
// they act on — a form at the bottom of the card cannot say which pad it means.
// What a preset captures: exactly ONE of the four top-level subtrees. A radio group rather than four
// checkboxes, so "a look" and "a geometry" are the only things expressible — the combinations that
// used to be possible were the hard part to explain and the hard part to display.
function buildCaptureToggles(body, moduleName) {
    const mod = findModule(moduleName);
    const ctrl = mod && (mod.controls || []).find(c => c.name === "captures");
    if (!ctrl) return;
    const names = Array.isArray(ctrl.options) && ctrl.options.length
        ? ctrl.options : ["Layouts", "Effects", "Drivers", "Services"];
    const wrap = document.createElement("div");
    wrap.className = "surface-popup-captures";
    names.forEach((n, i) => {
        const lab = document.createElement("label");
        lab.className = "surface-popup-capture";
        const rb = document.createElement("input");
        rb.type = "radio";
        rb.name = `capture-${moduleName}`;
        rb.checked = Number(ctrl.value) === i;
        rb.addEventListener("change", () => { if (rb.checked) sendControl(moduleName, "captures", i); });
        lab.append(rb, document.createTextNode(n));
        wrap.appendChild(lab);
    });
    const cap = document.createElement("div");
    cap.className = "surface-popup-caption";
    cap.textContent = "captures";
    body.append(cap, wrap);
}

function openPadEditor(anchorEl, moduleName, ctrlName, item, slot) {
    const filled = item != null;
    openSurfacePopup(anchorEl, filled ? `pad: ${item.name}` : `pad ${slot + 1} (empty)`, (body, close) => {
        if (filled) {
            const nameRow = document.createElement("div");
            nameRow.className = "surface-popup-row";
            const nameLbl = document.createElement("span");
            nameLbl.textContent = "name";
            const nameIn = document.createElement("input");
            nameIn.type = "text";
            nameIn.className = "list-field-input";
            nameIn.value = item.name || "";
            nameIn.addEventListener("change", async () => {
                await listSetField(moduleName, ctrlName, item.id, "name", nameIn.value);
                refetchState();
            });
            nameRow.append(nameLbl, nameIn);
            body.appendChild(nameRow);

            // What this preset carries, read from the file rather than the form.
            if (item.captures) {
                const cap = document.createElement("div");
                cap.className = "surface-popup-row";
                cap.textContent = `captures ${item.captures}`;
                body.appendChild(cap);
            }
            // Re-saving over the pad: the toggles decide what the NEW contents carry.
            buildCaptureToggles(body, moduleName);
            const over = document.createElement("button");
            over.className = "surface-popup-primary";
            over.textContent = "save current state over it";
            over.addEventListener("click", async () => {
                await sendControl(moduleName, "name", item.name);
                await sendControl(moduleName, "slot", slot);
                await sendControl(moduleName, "save", 1);
                close();
                refetchState();
            });
            body.appendChild(over);

            const del = document.createElement("button");
            del.className = "surface-popup-danger";
            del.textContent = "delete preset";
            del.addEventListener("click", async () => {
                await listDeleteRow(moduleName, ctrlName, item.id);
                close();
                refetchState();
            });
            body.appendChild(del);
        } else {
            // An empty pad's action is to fill it: name it and save the current state here.
            const nameRow = document.createElement("div");
            nameRow.className = "surface-popup-row";
            const nameLbl = document.createElement("span");
            nameLbl.textContent = "name";
            const nameIn = document.createElement("input");
            nameIn.type = "text";
            nameIn.className = "list-field-input";
            nameIn.placeholder = "new preset";
            nameRow.append(nameLbl, nameIn);
            body.appendChild(nameRow);
            buildCaptureToggles(body, moduleName);

            const saveBtn = document.createElement("button");
            saveBtn.className = "surface-popup-primary";
            saveBtn.textContent = "save current state here";
            saveBtn.addEventListener("click", async () => {
                if (!nameIn.value.trim()) { nameIn.focus(); return; }
                // The module's own `name` + `slot` + `save` controls do the work; the popup is only
                // the form that fills them, so the save path stays the one the tests cover.
                await sendControl(moduleName, "name", nameIn.value.trim());
                await sendControl(moduleName, "slot", slot);
                await sendControl(moduleName, "save", 1);
                close();
                refetchState();
            });
            body.appendChild(saveBtn);
            setTimeout(() => nameIn.focus(), 0);
        }
    });
}

// A rotary knob driving a range input. The input stays in the DOM (hidden) so every other path —
// the WS live patch, defaults, keyboard — keeps working against it unchanged; this only draws the
// value and turns drags into value changes.
/// "What does this strip drive?" — the same popup for an encoder and a fader, since the question and
/// the answer are identical for both. Right-click on a pointer, long-press on touch.
function attachTargetPopup(row, input, ctrl) {
    const show = () => openSurfacePopup(input, ctrl.name, (body) => {
        const line = document.createElement("div");
        line.className = "surface-popup-row";
        line.textContent = ctrl.target ? `drives ${ctrl.target}` : "unassigned — no target yet";
        body.appendChild(line);
    });
    row.addEventListener("contextmenu", (e) => { e.preventDefault(); show(); });
    attachLongPress(row, show);
}

function buildKnob(input, ctrl) {
    // Read the bounds LIVE rather than snapshotting them: the caller assigns input.min/max/value
    // after this returns, so a snapshot here is NaN and the dial draws against nothing.
    const bounds = () => {
        const lo = Number(input.min), hi = Number(input.max);
        return [Number.isFinite(lo) ? lo : 0, Number.isFinite(hi) ? hi : 255];
    };
    const wrap = document.createElement("div");
    wrap.className = "knob";
    const NS = "http://www.w3.org/2000/svg";
    const svg = document.createElementNS(NS, "svg");
    svg.setAttribute("viewBox", "0 0 40 40");
    // The arc runs from 7 o'clock to 5 o'clock — the 270° sweep a real encoder cap shows, with the
    // gap at the bottom so the pointer never sits in an ambiguous place.
    const track = document.createElementNS(NS, "circle");
    track.setAttribute("cx", "20"); track.setAttribute("cy", "20"); track.setAttribute("r", "15");
    track.setAttribute("class", "knob-track");
    const fill = document.createElementNS(NS, "circle");
    fill.setAttribute("cx", "20"); fill.setAttribute("cy", "20"); fill.setAttribute("r", "15");
    fill.setAttribute("class", "knob-fill");
    const pointer = document.createElementNS(NS, "line");
    pointer.setAttribute("class", "knob-pointer");
    svg.append(track, fill, pointer);
    wrap.appendChild(svg);

    const CIRC = 2 * Math.PI * 15;
    const SWEEP = 0.75;                       // 270° of the circle
    // rotate() as an ATTRIBUTE, not CSS: a CSS transform on an SVG geometry element resolves against
    // a different origin and drew a quarter arc instead of the intended 270°.
    track.setAttribute("transform", "rotate(135 20 20)");
    fill.setAttribute("transform", "rotate(135 20 20)");
    track.setAttribute("stroke-dasharray", `${CIRC * SWEEP} ${CIRC}`);
    const draw = () => {
        const [min, max] = bounds();
        const v = Number(input.value);
        const frac = max > min ? Math.min(1, Math.max(0, (v - min) / (max - min))) : 0;
        fill.style.strokeDasharray = `${CIRC * SWEEP * frac} ${CIRC}`;
        // 135° is the 7 o'clock start; sweep clockwise from there.
        const ang = (135 + frac * 270) * Math.PI / 180;
        pointer.setAttribute("x1", String(20 + 6 * Math.cos(ang)));
        pointer.setAttribute("y1", String(20 + 6 * Math.sin(ang)));
        pointer.setAttribute("x2", String(20 + 13 * Math.cos(ang)));
        pointer.setAttribute("y2", String(20 + 13 * Math.sin(ang)));
    };
    draw();
    // Redraw when the value changes from anywhere — a drag here, or a WS push patching the input.
    input.addEventListener("input", draw);
    input.addEventListener("change", draw);
    wrap._redraw = draw;

    // Vertical drag turns the knob: up increases. 150px of travel covers the full range, which is
    // fine for a coarse control and keeps the gesture inside a card.
    // The gesture needs to be discoverable: without a hint the knob looks like a picture, and the
    // range input underneath makes the browser offer a resize cursor that suggests the wrong action.
    wrap.title = "drag up/down or scroll to turn";
    wrap.addEventListener("pointerdown", (e) => {
        e.preventDefault();
        wrap.setPointerCapture(e.pointerId);
        wrap.classList.add("knob-turning");
        const startY = e.clientY, startV = Number(input.value);
        const [min, max] = bounds();
        const move = (ev) => {
            const delta = (startY - ev.clientY) / 150 * (max - min);
            const next = Math.round(Math.min(max, Math.max(min, startV + delta)));
            if (next === Number(input.value)) return;
            input.value = String(next);
            input.dispatchEvent(new Event("input", {bubbles: true}));
        };
        // Every termination path runs this once: pointerup, but also pointercancel and
        // lostpointercapture, which fire when the browser takes the gesture over (scroll, a system
        // gesture). Without them the move listener stayed attached and the knob kept turning.
        let ended = false;
        const up = (ev) => {
            if (ended) return;
            ended = true;
            wrap.classList.remove("knob-turning");
            if (ev && ev.pointerId != null && wrap.hasPointerCapture(ev.pointerId))
                wrap.releasePointerCapture(ev.pointerId);
            wrap.removeEventListener("pointermove", move);
            wrap.removeEventListener("pointerup", up);
            wrap.removeEventListener("pointercancel", up);
            wrap.removeEventListener("lostpointercapture", up);
            input.dispatchEvent(new Event("change", {bubbles: true}));
        };
        wrap.addEventListener("pointermove", move);
        wrap.addEventListener("pointerup", up);
        wrap.addEventListener("pointercancel", up);
        wrap.addEventListener("lostpointercapture", up);
    });
    // Scroll to turn: the gesture people try first on anything round, and the one that works without
    // knowing the drag exists. `passive:false` so the page does not scroll underneath it.
    wrap.addEventListener("wheel", (e) => {
        e.preventDefault();
        const [min, max] = bounds();
        const step = Math.max(1, Math.round((max - min) / 100));
        const next = Math.round(Math.min(max, Math.max(min,
            Number(input.value) + (e.deltaY < 0 ? step : -step))));
        if (next === Number(input.value)) return;
        input.value = String(next);
        input.dispatchEvent(new Event("input", {bubbles: true}));
        input.dispatchEvent(new Event("change", {bubbles: true}));
    }, {passive: false});
    return wrap;
}

// A seven-segment readout, the way the hardware shows a value: three digits, lit segments over an
// unlit ghost so the display reads like a real LED module rather than plain text.
const SEG7 = {
    "0": "abcdef", "1": "bc", "2": "abdeg", "3": "abcdg", "4": "bcfg",
    "5": "acdfg", "6": "acdefg", "7": "abc", "8": "abcdefg", "9": "abcdfg", " ": "",
};
function buildSevenSeg(input) {
    const NS = "http://www.w3.org/2000/svg";
    const wrap = document.createElement("div");
    wrap.className = "seg7";
    const svg = document.createElementNS(NS, "svg");
    svg.setAttribute("viewBox", "0 0 42 20");
    // Segment geometry per digit, in the standard a..g naming.
    const seg = (x, y, horiz) => {
        const p = document.createElementNS(NS, "rect");
        p.setAttribute("x", String(x)); p.setAttribute("y", String(y));
        p.setAttribute("width", horiz ? "7" : "2");
        p.setAttribute("height", horiz ? "2" : "7");
        p.setAttribute("rx", "1");
        return p;
    };
    const digits = [];
    for (let d = 0; d < 3; d++) {
        const ox = d * 14 + 1;
        const map = {
            a: seg(ox + 2, 1, true),  b: seg(ox + 9, 2, false), c: seg(ox + 9, 11, false),
            d: seg(ox + 2, 17, true), e: seg(ox, 11, false),    f: seg(ox, 2, false),
            g: seg(ox + 2, 9, true),
        };
        for (const el of Object.values(map)) { el.setAttribute("class", "seg7-off"); svg.appendChild(el); }
        digits.push(map);
    }
    wrap.appendChild(svg);
    const draw = () => {
        // Right-aligned, blank-padded: "  7", " 42", "255" — how a 3-digit module displays.
        const txt = String(Math.round(Number(input.value))).padStart(3, " ").slice(-3);
        digits.forEach((map, i) => {
            const on = SEG7[txt[i]] ?? "";
            for (const [name, el] of Object.entries(map))
                el.setAttribute("class", on.includes(name) ? "seg7-on" : "seg7-off");
        });
    };
    draw();
    input.addEventListener("input", draw);
    input.addEventListener("change", draw);
    // Same hook the knob exposes, so redrawRangeDecorations refreshes both without knowing what
    // either one is.
    wrap._redraw = draw;
    return wrap;
}

function buildListPads(container, rows, opts) {
    const moduleName = opts && opts.moduleName;
    const ctrlName = opts && opts.ctrlName;
    const editable = !!(opts && opts.editable);
    const cols = (opts && opts.gridCols) | 0;
    const gridRows = (opts && opts.gridRows) | 0;
    const fixed = cols > 0 && gridRows > 0;
    container.replaceChildren();
    const grid = document.createElement("div");
    grid.className = "list-pads" + (fixed ? " list-pads-fixed" : "");
    // Columns track --surface-col, the ONE width the knob strips and fader strips also use, so the
    // three banks line up as columns of a single surface. It is a clamp() against the card, so the
    // alignment survives the right pane being dragged narrower or wider.
    if (fixed) grid.style.gridTemplateColumns = `repeat(${cols}, var(--surface-col))`;
    container.appendChild(grid);
    if (!fixed && rows.length === 0) {
        const empty = document.createElement("div");
        empty.className = "list-empty";
        empty.textContent = "(none)";
        container.appendChild(empty);
        return;
    }
    // On a FIXED surface every cell is rendered, occupied or not: an empty cell is a position you
    // can drop a pad onto, which is what makes the grid a surface rather than a wrapped list.
    const cells = fixed ? new Array(cols * gridRows).fill(null) : rows.slice();
    if (fixed) for (const r of rows) {
        const s = (r && r.slot) | 0;
        if (s >= 0 && s < cells.length) cells[s] = r;
    }
    cells.forEach((item, i) => {
        if (fixed && item == null) {
            // An empty cell: a drop target and nothing else. It carries the slot index so a drop
            // knows where it landed.
            // A button, not a div: an empty cell is an ACTION (create a preset here), so it must be
            // reachable and activatable from the keyboard like every other pad.
            const hole = document.createElement("button");
            hole.type = "button";
            hole.className = "list-pad list-pad-empty";
            hole.title = `pad ${i + 1} (empty) — click to save the current state here`;
            hole.addEventListener("click", () => openPadEditor(hole, moduleName, ctrlName, null, i));
            hole.addEventListener("dragover", (e) => {
                e.preventDefault();
                e.dataTransfer.dropEffect = "move";
                hole.classList.add("list-drop-target");
            });
            hole.addEventListener("dragleave", () => hole.classList.remove("list-drop-target"));
            hole.addEventListener("contextmenu", (e) => {
                e.preventDefault();
                openPadEditor(hole, moduleName, ctrlName, null, i);
            });
            attachLongPress(hole, () => openPadEditor(hole, moduleName, ctrlName, null, i));
            hole.addEventListener("drop", (e) => {
                e.preventDefault();
                hole.classList.remove("list-drop-target");
                const draggedId = parseInt(e.dataTransfer.getData("text/plain"));
                if (!Number.isNaN(draggedId)) listMoveRow(moduleName, ctrlName, draggedId, i);
            });
            grid.appendChild(hole);
            return;
        }
        const pad = document.createElement("button");
        pad.type = "button";
        pad.className = "list-pad" + (item && item.active ? " list-pad-active" : "");
        // A pad is tinted by the roles it carries, so what a preset covers reads as color as well as
        // emoji. When it is lit, the tint comes from the roles it still HOLDS (activeRoles): a mixed
        // preset whose layer was superseded but whose layout is still on stays lit in the layout hue.
        const padRoles = Array.isArray(item && item.roles) ? item.roles : [];
        const litRoles = Array.isArray(item && item.activeRoles) ? item.activeRoles : [];
        const hue = roleHue(litRoles.length ? litRoles : padRoles);
        if (hue) pad.style.setProperty("--pad-hue", hue);
        const label = String((item && item.name) ?? "");
        // Role emoji through the SAME table the module cards use, so 🥞 means the same thing on a pad
        // as on a card. A row that carries no roles simply gets no emoji.
        const roles = Array.isArray(item && item.roles) ? item.roles : [];
        const emoji = roles.map(r => ROLE_EMOJI[r] || "").join("");
        if (emoji) {
            const em = document.createElement("span");
            em.className = "list-pad-emoji";
            em.textContent = emoji;
            pad.appendChild(em);
        }
        const nameEl = document.createElement("span");
        nameEl.className = "list-pad-name";
        nameEl.textContent = label;
        pad.appendChild(nameEl);
        pad.title = emoji ? `${label} (${roles.join(", ")})` : label;   // the full name when truncated
        pad.addEventListener("click", async () => {
            if (item == null || item.id == null) return;
            pad.disabled = true;
            // An ACTION field: the arrival is the whole message, so the value is unused. (The tests
            // call setListRowField directly and pass the request BODY, which is why they read "{}".)
            await listSetField(moduleName, ctrlName, item.id, "activate", "");
            refetchState();
            pad.disabled = false;
        });
        // Right-click edits the pad rather than triggering it: the controls belong on the thing they
        // configure, not in a form at the bottom of the card that cannot say which pad it means.
        pad.addEventListener("contextmenu", (e) => {
            e.preventDefault();
            openPadEditor(pad, moduleName, ctrlName, item, (item && item.slot) | 0);
        });
        // Touch has no right button, so a long press opens the same editor. Cancelled by movement so
        // a drag to reorder is not mistaken for a press-and-hold.
        attachLongPress(pad, () => openPadEditor(pad, moduleName, ctrlName, item, (item && item.slot) | 0));
        // Drag to reorder, so a grid can be arranged to match a physical control surface. Same
        // mechanism the list rows use (listMoveRow by stable id), and the same reason the ids are
        // stable: a reorder must not change what a pad refers to.
        if (editable && item && item.id != null) {
            pad.draggable = true;
            pad.addEventListener("dragstart", (e) => {
                e.dataTransfer.effectAllowed = "move";
                e.dataTransfer.setData("text/plain", String(item.id));
                pad.classList.add("list-dragging");
            });
            pad.addEventListener("dragend", () => {
                pad.classList.remove("list-dragging");
                grid.querySelectorAll(".list-drop-target").forEach(el => el.classList.remove("list-drop-target"));
            });
            pad.addEventListener("dragover", (e) => {
                e.preventDefault();
                e.dataTransfer.dropEffect = "move";
                pad.classList.add("list-drop-target");
            });
            pad.addEventListener("dragleave", () => pad.classList.remove("list-drop-target"));
            pad.addEventListener("drop", (e) => {
                e.preventDefault();
                pad.classList.remove("list-drop-target");
                const draggedId = parseInt(e.dataTransfer.getData("text/plain"));
                if (!Number.isNaN(draggedId) && draggedId !== item.id) {
                    // `i` is the SLOT on a fixed grid (the cell), or the row index on a flowing one.
                    listMoveRow(moduleName, ctrlName, draggedId, i);
                }
            });
        }
        grid.appendChild(pad);
    });
}

function buildListEntries(container, rows, details, openSet, opts) {
    if (opts && opts.pads) { buildListPads(container, rows, opts); return; }
    const editable = !!(opts && opts.editable);
    const moduleName = opts && opts.moduleName;
    const ctrlName = opts && opts.ctrlName;
    const optionSets = (opts && opts.optionSets) || {};   // shared option arrays, keyed by name (optionsRef)
    container.replaceChildren();
    if (rows.length === 0 && !editable) {
        const empty = document.createElement("div");
        empty.className = "list-empty";
        empty.textContent = "(none)";
        container.appendChild(empty);
        return;
    }
    // Rows live in their own scroll box so a long list (e.g. the seeded fixture presets) caps its
    // height (~5 rows, see .list-scroll in the CSS) and scrolls, instead of eating the whole card.
    // The "+ Add" button stays OUTSIDE this box, pinned below the scroll area — the standard
    // scrollable-list-with-fixed-footer pattern.
    const scroll = document.createElement("div");
    scroll.className = "list-scroll";
    container.appendChild(scroll);
    rows.forEach((item, i) => {
        const entry = document.createElement("div");
        // Row classes: the `self` marker + a generic `severity` marker (rowSeverityClass) — both are
        // field-name conventions the engine opts into, no per-module UI code.
        const sevClass = rowSeverityClass(item);
        entry.className = "list-entry" + (item && item.self ? " list-self" : "") +
                          (sevClass ? " " + sevClass : "");
        const summary = document.createElement("div");
        summary.className = "list-summary";
        summary.tabIndex = 0;
        summary.setAttribute("role", "button");
        // Freshness dot (always-visible age at a glance) when the row carries a `*Sec`
        // duration; colored by ageBucketClass. Generic — no device knowledge here.
        // The age fields (`ageSec`/`cached`) live in the DETAIL object, not the summary,
        // so read the detail for the dot (it also carries `self`); fall back to the
        // summary item when a list has no separate detail.
        const ageClass = rowAgeClass(details[i] ?? item);
        if (ageClass) {
            const dot = document.createElement("span");
            dot.className = "age-dot " + ageClass;
            dot.setAttribute("aria-hidden", "true");
            summary.appendChild(dot);
        }
        const label = document.createElement("span");
        label.className = "list-summary-label";
        label.textContent = listSummaryText(item);
        summary.appendChild(label);
        const detailPanel = document.createElement("div");
        detailPanel.className = "list-detail";
        // Keyed via listRowKey (stable `id` for editable rows, else the label text — see the
        // helper) so an expanded row stays expanded across a live re-render, even when the field
        // that changed is shown in the summary label (e.g. a preset's channel count).
        detailPanel.hidden = !openSet.has(listRowKey(item, label.textContent));
        summary.setAttribute("aria-expanded", String(!detailPanel.hidden));
        const locked = !!(item && item.locked);
        if (editable) {
            // Right-side row actions: delete (✕) then a `⠿` drag-to-reorder handle, in that order —
            // handles on the RIGHT, after delete, keep every row's controls aligned in one column
            // (the common data-grid layout). A `locked` (seeded) row isn't draggable and has no
            // delete — the server refuses both, so the UI shows neither, and locked rows have an
            // empty actions area that still aligns. The buttons stop click propagation so they don't
            // toggle the detail panel. entry carries the row id so a drop knows what moved where.
            entry.dataset.rowId = item.id;
            entry.dataset.rowIndex = i;
            const actions = document.createElement("span");
            actions.className = "list-actions";
            if (!locked) {
                const del = document.createElement("button");
                del.type = "button";
                del.className = "list-action-btn";
                del.textContent = "✕";
                del.title = "Delete";
                del.addEventListener("click", (e) => { e.stopPropagation(); listDeleteRow(moduleName, ctrlName, item.id); });
                actions.appendChild(del);

                const handle = document.createElement("span");
                handle.className = "list-drag-handle";
                handle.textContent = "⠿";                 // a braille-dots grab affordance (the ::: idiom)
                handle.title = "Drag to reorder";
                handle.setAttribute("aria-hidden", "true");
                handle.addEventListener("click", (e) => e.stopPropagation());
                // The entry is draggable, gated by the SAME mechanism the module-card drag uses
                // (attachDragHandlers): a capture-phase MOUSEDOWN handler sets `draggable` based on where
                // the grab landed. HTML5 drag initiates from mousedown, so the flag must be set there —
                // a pointerdown handler runs too late (the browser has already read draggable at
                // mousedown) and dragstart never fires. Here the row drags ONLY when the grab is on the
                // handle (a click elsewhere expands the row); the touchstart mirror arms mobile drag.
                const dragGate = (e) => { entry.draggable = !!e.target.closest(".list-drag-handle"); };
                entry.addEventListener("mousedown", dragGate, true);
                entry.addEventListener("touchstart", dragGate, {capture: true, passive: true});
                entry.addEventListener("dragstart", (e) => {
                    e.dataTransfer.effectAllowed = "move";
                    e.dataTransfer.setData("text/plain", String(item.id));
                    entry.classList.add("list-dragging");
                });
                entry.addEventListener("dragend", () => {
                    entry.draggable = false;
                    entry.classList.remove("list-dragging");
                    container.querySelectorAll(".list-drop-target").forEach(el => el.classList.remove("list-drop-target"));
                });
                actions.appendChild(handle);
            }
            summary.appendChild(actions);
            // Every editable row is a drop TARGET (drop a dragged row onto it to move there). A drop
            // onto a locked row is clamped server-side to the first custom slot, so it's harmless.
            entry.addEventListener("dragover", (e) => { e.preventDefault(); e.dataTransfer.dropEffect = "move"; entry.classList.add("list-drop-target"); });
            entry.addEventListener("dragleave", () => entry.classList.remove("list-drop-target"));
            entry.addEventListener("drop", (e) => {
                e.preventDefault();
                entry.classList.remove("list-drop-target");
                const draggedId = parseInt(e.dataTransfer.getData("text/plain"));
                if (!Number.isNaN(draggedId) && draggedId !== item.id) {
                    listMoveRow(moduleName, ctrlName, draggedId, i);   // move the dragged row to THIS row's index
                }
            });
            // Editable detail: render each field descriptor as an inline control, unless
            // the row is locked (then fall back to the read-only key/value render).
            if (locked) fillListDetail(detailPanel, details[i] ?? item);
            else fillEditableListDetail(detailPanel, details[i] ?? item, moduleName, ctrlName, item.id, optionSets);
        } else {
            fillListDetail(detailPanel, details[i] ?? item);
        }
        const toggle = () => {
            detailPanel.hidden = !detailPanel.hidden;
            summary.setAttribute("aria-expanded", String(!detailPanel.hidden));
        };
        summary.addEventListener("click", toggle);
        summary.addEventListener("keydown", (e) => {
            if (e.key === "Enter" || e.key === " ") { e.preventDefault(); toggle(); }
        });
        entry.append(summary, detailPanel);
        scroll.appendChild(entry);
    });
    if (editable) {
        // "+ Add" appends a new row (server assigns the id, then state refreshes).
        const addBtn = document.createElement("button");
        addBtn.type = "button";
        addBtn.className = "list-add-btn";
        addBtn.textContent = "+ Add";
        addBtn.addEventListener("click", () => listAddRow(moduleName, ctrlName));
        container.appendChild(addBtn);
    }
}

// Render an editable row's detail: each descriptor in `detail.fields[]` becomes an inline
// control (text→<input>, uint8→<input type=number>, select→<select>). On change we call
// listSetField(...) and let the normal refresh reflect the persisted state — no optimistic
// local mutation. A field with `readonly:true` renders as a plain read-only value (same look
// as fillListDetail). Fields carry a dragTs cooldown so a WS state push mid-edit can't revert
// what's being typed/picked, mirroring the select/text guards in createControl.
function fillEditableListDetail(panel, detail, moduleName, ctrlName, id, optionSets) {
    panel.replaceChildren();
    optionSets = optionSets || {};
    const fields = detail && Array.isArray(detail.fields) ? detail.fields : [];
    for (const f of fields) {
        const r = document.createElement("div");
        r.className = "list-detail-row";
        const kEl = document.createElement("span");
        kEl.className = "list-detail-key";
        kEl.textContent = f.name;
        const vEl = document.createElement("span");
        vEl.className = "list-detail-val";
        // Per-field cooldown key: unique per module/control/row/field so edits don't collide.
        const dragKey = `list:${moduleName}:${ctrlName}:${id}:${f.name}`;
        if (f.readonly) {
            vEl.textContent = String(f.value ?? "");
            vEl.classList.add("list-detail-muted");
        } else if (f.type === "button") {
            // A row ACTION rather than a value: the click PATCHes the field like any edit, and the
            // source reads the arrival as "do this to this row" (ControlModule's preset `apply`).
            // Generic on purpose — a row button is a primitive the list has lacked, not a
            // preset-specific affordance. Refetches, because an action typically changes the row set
            // or the wider tree, unlike a field edit which the WS push reconciles.
            const btn = document.createElement("button");
            btn.className = "list-field-btn";
            btn.textContent = f.label || f.name;
            btn.addEventListener("click", async () => {
                btn.disabled = true;
                await listSetField(moduleName, ctrlName, id, f.name, "");
                refetchState();
                btn.disabled = false;
            });
            vEl.appendChild(btn);
        } else if (f.type === "select") {
            const sel = document.createElement("select");
            sel.className = "list-field-input";
            sel.dataset.dragkey = dragKey;
            // Options come from the field's inline `options`, or (the common case for a repeated select
            // like the channel-role pickers) from the list's shared `optionSets` via `optionsRef` — so
            // the option array is sent once per list, not re-inlined in every row (see writeListOptionSets).
            const fieldOptions = f.options || (f.optionsRef ? optionSets[f.optionsRef] : null) || [];
            fieldOptions.forEach((opt, idx) => {
                const o = document.createElement("option");
                o.value = idx;
                o.textContent = opt;
                if (idx === f.value) o.selected = true;
                sel.appendChild(o);
            });
            const mark = () => { dragTs[dragKey] = Date.now(); };
            sel.addEventListener("pointerdown", mark);
            sel.addEventListener("focus", mark);
            sel.addEventListener("change", () => {
                dragTs[dragKey] = Date.now();
                listSetField(moduleName, ctrlName, id, f.name, parseInt(sel.value));
            });
            vEl.appendChild(sel);
        } else if (f.type === "uint8") {
            const inp = document.createElement("input");
            inp.type = "number";
            inp.className = "list-field-input";
            inp.dataset.dragkey = dragKey;
            if (f.min !== undefined) inp.min = f.min;
            if (f.max !== undefined) inp.max = f.max;
            inp.value = f.value ?? 0;
            inp.addEventListener("input", () => { dragTs[dragKey] = Date.now(); });
            inp.addEventListener("change", () => {
                dragTs[dragKey] = Date.now();
                // Guard against empty/invalid entry (parseInt → NaN) and clamp to the control's
                // range — the HTML min/max attributes don't enforce a hand-typed value, so a stray
                // "" or out-of-range number would otherwise reach the device as NaN / an overflow.
                let v = parseInt(inp.value, 10);
                if (!Number.isFinite(v)) v = f.min ?? 0;
                if (f.min !== undefined) v = Math.max(v, f.min);
                if (f.max !== undefined) v = Math.min(v, f.max);
                inp.value = v;   // reflect the clamped value back into the field
                listSetField(moduleName, ctrlName, id, f.name, v);
            });
            vEl.appendChild(inp);
        } else {   // "text" and any unknown type render as a text input
            const inp = document.createElement("input");
            inp.type = "text";
            inp.className = "list-field-input";
            inp.dataset.dragkey = dragKey;
            inp.value = f.value ?? "";
            inp.addEventListener("input", () => { dragTs[dragKey] = Date.now(); });
            inp.addEventListener("change", () => {
                dragTs[dragKey] = Date.now();
                listSetField(moduleName, ctrlName, id, f.name, inp.value);
            });
            vEl.appendChild(inp);
        }
        r.append(kEl, vEl);
        panel.appendChild(r);
    }
}

// Join a list row's scalar fields into a one-line summary (skips marker fields — `self`, `severity`
// — that render as row styling rather than text, and any nested objects). Generic: the engine names
// the fields.
function listSummaryText(item) {
    if (!item || typeof item !== "object") return String(item ?? "");
    return Object.entries(item)
        .filter(([k, v]) => k !== "self" && k !== "severity" && typeof v !== "object")
        .map(([, v]) => v)
        .join("  ·  ");
}

// Render a list row's detail object as read-only key/value rows. Scalars print as-is;
// an array of scalars (e.g. a device's `speaks:["http"]` or `via:["mdns","scan"]`)
// renders as small chips so multi-valued fields like the discovery source are visible
// at a glance. Nested objects are still skipped (no use case yet). Generic — the engine
// names the fields, so a new array field shows up with no UI change here.
function fillListDetail(panel, detail) {
    panel.replaceChildren();
    if (!detail || typeof detail !== "object") return;
    for (const [k, v] of Object.entries(detail)) {
        const isScalarArray = Array.isArray(v) && v.every(e => typeof e !== "object");
        if (typeof v === "object" && !isScalarArray) continue;
        // `cached` and `ageSec` both render as "last seen"; a projectMM device emits
        // exactly one (mutually exclusive in DevicesModule), but skip ageSec when a
        // `cached` key is also present so any other source can't produce two conflicting
        // "last seen" rows. Match the cached branch's render condition, which fires on
        // key EXISTENCE (`k === "cached"`), not truthiness — so gate on the key being
        // present, not on its value. (Robust-to-any-input, generic.)
        if (k === "ageSec" && "cached" in detail) continue;
        const r = document.createElement("div");
        r.className = "list-detail-row";
        const kEl = document.createElement("span");
        // A `*Sec` field is a duration in seconds (e.g. a device's `ageSec`) — show it
        // under a plainer label ("last seen") and as a relative time, not a bare count.
        // `cached` is the sibling: a restored device not yet re-seen live → "last seen:
        // cached" rather than a fake recent time.
        const isDuration = k.endsWith("Sec");
        kEl.className = "list-detail-key";
        kEl.textContent = (k === "ageSec" || k === "cached") ? "last seen" : k;
        const vEl = document.createElement("span");
        vEl.className = "list-detail-val";
        if (k === "cached") {
            vEl.textContent = "cached";
            vEl.classList.add("list-detail-muted");
        } else if (isScalarArray) {
            for (const e of v) {
                const chip = document.createElement("span");
                chip.className = "list-detail-chip";
                chip.textContent = String(e);
                vEl.appendChild(chip);
            }
        } else if (isDuration) {
            vEl.textContent = relativeAge(Number(v));
            const ageClass = ageBucketClass(Number(v));   // tint to match the summary dot
            if (ageClass) vEl.classList.add(ageClass);
        } else if (typeof v === "string" && /^https?:\/\//.test(v)) {
            // A value that is an http(s) URL (e.g. a device's `url`) renders as a link that
            // opens in a new tab — generic, any ListSource detail can surface one. rel
            // guards the opened page from reaching back via window.opener.
            const a = document.createElement("a");
            a.href = v;
            a.textContent = v;
            a.target = "_blank";
            a.rel = "noopener noreferrer";
            a.className = "list-detail-link";
            vEl.appendChild(a);
        } else {
            vEl.textContent = String(v);
        }
        r.append(kEl, vEl);
        panel.appendChild(r);
    }
}

// Freshness bucket for a duration-in-seconds (a `*Sec` field): green < 1 min, orange
// < 1 hour, red beyond. Generic — any duration field a ListSource emits gets the same
// scale; nothing device-specific here. (DevicesModule's ageSec is the first user: a
// device unseen > 24h is aged out of the list entirely, so "red" spans 1h–24h.)
function ageBucketClass(sec) {
    if (!Number.isFinite(sec) || sec < 0) return "";
    if (sec < 60) return "age-fresh";
    if (sec < 3600) return "age-recent";
    return "age-stale";
}

// Find a row's freshness CSS class from its first `*Sec` scalar field, or "" if it has
// none. `self` is always "now" → fresh; a `cached` row (restored, not re-seen) is
// unknown-age → no dot (the detail says "cached"). Generic over field names.
function rowAgeClass(item) {
    if (!item || typeof item !== "object") return "";
    if (item.self) return "age-fresh";
    if (item.cached) return "";
    for (const [k, v] of Object.entries(item)) {
        if (k.endsWith("Sec") && typeof v === "number") return ageBucketClass(v);
    }
    return "";
}

// Severity CSS class from a row's `severity` field ("error"/"warn"), or "" if none/absent. Generic
// over the field name, exactly like rowAgeClass over `*Sec`: any ListSource that emits a `severity`
// string gets the same visual (a colored row marker). PinsModule is the first user — it flags a GPIO
// claim on a reserved/strap/input-only pin — but nothing here is pins-specific; a task in a bad state
// or a device with an error could emit `severity` and light up the same way.
function rowSeverityClass(item) {
    if (!item || typeof item !== "object") return "";
    if (item.severity === "error") return "list-severity-error";
    if (item.severity === "warn") return "list-severity-warn";
    return "";
}

// Render a seconds-ago count as a short relative time ("just now", "2m ago", "3h ago",
// "5d ago"). Snapshot at state-push time — it refreshes when the list re-renders, not
// per second. Mirrors the device-side ageSec (now - lastSeenMs); kept simple on purpose.
function relativeAge(sec) {
    if (!Number.isFinite(sec) || sec < 0) return "—";
    if (sec < 10) return "just now";
    if (sec < 60) return `${sec}s ago`;
    if (sec < 3600) return `${Math.floor(sec / 60)}m ago`;
    if (sec < 86400) return `${Math.floor(sec / 3600)}h ago`;
    return `${Math.floor(sec / 86400)}d ago`;
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
    const eq = controlValuesEqual(ctrl, def);
    btn.classList.toggle("active", !eq);
    btn.addEventListener("click", () => {
        const key = moduleName + ":" + ctrl.name;
        clearTimeout(dragTimers[key]);   // a pending debounced edit must not overwrite the reset
        dragTs[key] = Date.now();        // and a stale WS patch must not revert it
        applyVisually();
        sendControl(moduleName, ctrl.name, def);
    });
    row.appendChild(btn);
}

function debounceSend(key, ms, fn) {
    clearTimeout(dragTimers[key]);
    dragTimers[key] = setTimeout(fn, ms);
}

// Password controls arrive XOR-obfuscated + base64-encoded (see
// HttpServerModule PASSWORD_XOR_KEY). This reverses it. The XOR key is a fixed
// shared constant, not a secret — this is obfuscation so the password is not
// plainly readable in a raw /api/state response, not real encryption.
const PW_XOR_KEY = 0x5A;
function decodePassword(encoded) {
    if (!encoded) return "";
    try {
        const bytes = atob(encoded);
        let out = "";
        for (let i = 0; i < bytes.length; i++) {
            out += String.fromCharCode(bytes.charCodeAt(i) ^ PW_XOR_KEY);
        }
        return out;
    } catch {
        return "";
    }
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
    // bytes === false → a plain count (e.g. a scan position "37 / 254"); otherwise
    // KB (the heap / flash / filesystem gauges, the original use).
    if (ctrl.bytes === false) return v + " / " + t;
    return Math.round(v / 1024) + "KB / " + Math.round(t / 1024) + "KB";
}

// "<value> <unit>" — treats null / undefined / 0 as "unavailable" so the
// UI doesn't render bogus "0 dBm" when the device is in a state where the
// metric isn't meaningful. The device's updateMetrics() writes 0 to rssi /
// txPower in non-WiFi states; the control is hidden in those states, but
// if anyone toggles hidden off (DevTools, future code path) the unit-with-
// zero rendering would still mislead. Real metric values are never zero
// in practice — RSSI is negative, TX power is 0..127 dBm (zero only on
// driver-uninitialised reads).
function fmtDisplayInt(ctrl) {
    const v = ctrl.value;
    const u = ctrl.unit || "";
    if (v === null || v === undefined || v === 0) return "";
    return u ? `${v} ${u}` : String(v);
}

// ---------------------------------------------------------------------------
// 5. State patching (no-rebuild contract)
// ---------------------------------------------------------------------------

// Apply a diff-on-the-wire patch onto the in-memory `state`. Each entry is {path,value} where path is
// "<module>/<control>" for a control value, or "<module>/@<field>" for a live module-header field
// (tickTimeUs / dynamicBytes — shown per card). Module names are unique tree-wide (the device dedups
// them), so the first path segment identifies the module. Mutates `state` in place; the caller then
// calls updateValues() to refresh the DOM without a rebuild. An unknown path is skipped (a resync will
// reconcile). List controls: the value is the full summary array (the device sends a changed list's
// whole value — see the plan), so we replace it wholesale, which updateModuleControls/renderCards read.
function applyStatePatch(patch) {
    const byName = new Map(allModules().map(m => [m.name, m]));
    for (const entry of patch) {
        if (!entry || typeof entry.path !== "string") continue;
        const slash = entry.path.indexOf("/");
        if (slash < 0) continue;
        const modName = entry.path.slice(0, slash);
        const leaf = entry.path.slice(slash + 1);
        const mod = byName.get(modName);
        if (!mod) continue;
        if (leaf[0] === "@") {
            mod[leaf.slice(1)] = entry.value;   // header field, e.g. @tickTimeUs -> mod.tickTimeUs
        } else if (Array.isArray(mod.controls)) {
            const c = mod.controls.find(c => c.name === leaf);
            if (c) c.value = entry.value;
        }
    }
}

/// Refresh the decorations drawn ON TOP of a range input (the knob dial, the seven-segment readout).
///
/// A WebSocket patch assigns `input.value` directly, which fires NEITHER `input` NOR `change` — those
/// only fire for user gestures. Anything listening for them therefore keeps showing the value it was
/// built with, which is why the readouts read 880/50 while the knobs pointed elsewhere. Call this
/// wherever a range input's value is set programmatically.
function redrawRangeDecorations(input) {
    if (!input) return;
    for (const el of input.parentElement ? input.parentElement.children : []) {
        if (typeof el._redraw === "function") el._redraw();
    }
}

function updateValues() {
    if (!state || !state.modules) return;
    // Patch each visible card's controls and stats line; never rebuild the DOM here.
    for (const mod of allModules()) {
        updateTabDot(mod);   // a fault on a BACKGROUND tab must surface without opening it
        updateModuleControls(mod);
        // refresh the stats line for this module if visible
        const statsEl = document.querySelector(`.card-stats[data-mid="${cssEscape(mod.name)}"]`);
        if (statsEl) { statsEl.textContent = formatStats(mod); statsEl.title = formatStatsTitle(mod); }
        // refresh status row — insert it if status appeared after card build
        let statusRow = document.querySelector(`[data-status-mid="${cssEscape(mod.name)}"]`);
        if (mod.status) {
            if (!statusRow) {
                // Card exists but had no status at build time — insert now before first control.
                const card = document.querySelector(`.card[data-module="${cssEscape(mod.name)}"]`);
                const host = card && (card.querySelector(".card-controls-collapse") || card);
                if (host) {
                    statusRow = document.createElement("div");
                    statusRow.className = "control-row";
                    statusRow.dataset.statusMid = mod.name;
                    const label = document.createElement("span");
                    label.className = "control-label";
                    label.textContent = "status";
                    const val = document.createElement("span");
                    val.className = "status-value";
                    val.dataset.sev = mod.severity || "status";
                    val.textContent = mod.status;
                    statusRow.appendChild(label);
                    statusRow.appendChild(val);
                    // Insert before first .control-row, or append.
                    const firstRow = host.querySelector(".control-row");
                    firstRow ? host.insertBefore(statusRow, firstRow) : host.appendChild(statusRow);
                }
            } else {
                statusRow.style.display = "";
                const val = statusRow.querySelector(".status-value");
                if (val) { val.textContent = mod.status; val.dataset.sev = mod.severity || "status"; }
            }
        } else if (statusRow) {
            statusRow.style.display = "none";
        }
        // refresh enabled toggle (now a styled <button>, not an <input>)
        const enabledEl = document.querySelector(`button.module-enabled[data-mid="${cssEscape(mod.name)}"]`);
        if (enabledEl) {
            const ts = dragTs[mod.name + ":enabled"] || 0;
            if (Date.now() - ts > 1000) {
                const on = (mod.enabled === undefined) ? true : !!mod.enabled;
                enabledEl.dataset.checked = on ? "true" : "false";
                enabledEl.textContent = "⏻";
                enabledEl.classList.toggle("module-enabled--off", !on);
                enabledEl.setAttribute("aria-pressed", on ? "true" : "false");
                const cardEl = document.querySelector(`.card[data-module="${cssEscape(mod.name)}"]`);
                if (cardEl) cardEl.classList.toggle("card--disabled", !on);
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

// Reconcile a card's control rows when its set of VISIBLE controls changed (a
// `hidden` flag flipped at runtime, e.g. NetworkModule's static-IP fields or
// RmtLedDriver's loopbackRxPin). The value-patch path in updateModuleControls
// can't add or remove rows, so this handles that half. Returns true if it
// changed the DOM. No-op (returns false) on the common frame where nothing moved.
//
// Position-stable by design: it inserts each newly-visible row at its correct
// index among the existing control rows and removes rows that became hidden —
// it does NOT tear down and re-append every row (that would land them after the
// card's child-module block / install-picker mount, never converge, and re-fire
// every WS tick — a render loop that wedges the UI).
function syncVisibleControls(mod) {
    const card = document.querySelector(`.card[data-module="${cssEscape(mod.name)}"]`);
    if (!card) return false;
    // The controls host is THIS card's own collapse wrapper — must be a DIRECT
    // child (`:scope >`), not any descendant: a container card (e.g. Effects) nests
    // its child cards (Layer) inside .card-children, and a plain
    // `card.querySelector(".card-controls-collapse")` would reach down and match
    // the CHILD's wrapper. That made Effects adopt Layer's control rows as its own,
    // so both cards saw a control-set mismatch every WS frame and rebuilt each
    // other's rows in a loop — tearing down (and closing) any open <select>.
    const host = card.querySelector(":scope > .card-controls-collapse") || card;

    const wantNames = mod.controls.filter(c => controlRendersGenerically(mod, c)).map(c => c.name);
    const haveRows = [...host.querySelectorAll(":scope > .control-row[data-key]")];
    const haveNames = haveRows.map(r => r.dataset.key);
    if (wantNames.length === haveNames.length && wantNames.every((n, i) => n === haveNames[i])) {
        return false;  // unchanged — the common case, so an idle WS push rebuilds nothing
    }
    // The visible-control set genuinely drifted (a hidden flag flipped — e.g. whiteMode appearing when
    // the preset changes). Rebuild to reflect it; latest state wins. We don't defer for an in-progress
    // edit: the drift is a consequence of a real change, and suppressing it would hide that change.

    // Remove rows whose control is no longer visible.
    const wantSet = new Set(wantNames);
    for (const row of haveRows) {
        if (!wantSet.has(row.dataset.key)) row.remove();
    }
    // Insert each visible control's row at its correct position. The anchor is the
    // first existing control row that should come AFTER this one; null → append
    // before the children block (insertBefore(node, null) appends to host's end,
    // but control rows precede .card-children which lives on the card, not here).
    const visibleControls = mod.controls.filter(c => controlRendersGenerically(mod, c));
    for (let i = 0; i < visibleControls.length; i++) {
        const name = visibleControls[i].name;
        if (host.querySelector(`:scope > .control-row[data-key="${cssEscape(name)}"]`)) continue;
        const row = createControl(mod.name, mod.type, visibleControls[i]);
        if (!row) continue;
        // Anchor: the rendered row of the next visible control that already exists.
        let anchor = null;
        for (let j = i + 1; j < visibleControls.length && !anchor; j++) {
            anchor = host.querySelector(`:scope > .control-row[data-key="${cssEscape(visibleControls[j].name)}"]`);
        }
        // No later control row exists yet → keep this row above the card's
        // children block / install-picker mount / footer (which live in the host
        // when host===card), so controls never render below the children.
        if (!anchor) {
            anchor = host.querySelector(":scope > .card-children")
                  || host.querySelector(":scope > .install-picker-host")
                  || host.querySelector(":scope > .card-footer");
        }
        host.insertBefore(row, anchor);
    }
    return true;
}

function updateModuleControls(mod) {
    if (!mod.controls) return;

    // Conditional controls: a module can flip a control's `hidden` flag at runtime
    // (e.g. RmtLedDriver reveals loopbackRxPin while the test is on, NetworkModule
    // reveals static-IP fields). The value-patch loop below only updates controls
    // already in the DOM — it can't add or remove one. So first detect whether the
    // set of VISIBLE controls drifted from what's rendered, and if so re-render
    // this card's control rows. Cheap: only fires on the rare frame where a hidden
    // flag actually changed.
    if (syncVisibleControls(mod)) return;  // re-rendered — values are fresh, skip patch

    for (const ctrl of mod.controls) {
        const mid = cssEscape(mod.name);
        const k = cssEscape(ctrl.name);
        const dragKey = mod.name + ":" + ctrl.name;
        const ts = dragTs[dragKey] || 0;
        const userActive = Date.now() - ts < 1000;

        // One guard for every editable control: while the user is mid-edit (a
        // keystroke / drag within the last second, tracked by dragTs), don't let a
        // WS state push overwrite the field with the value it had before the edit
        // landed. The read-only types (display/display-int/time/progress) and the
        // composite `list` aren't in this set — they always reflect the latest push.
        if (userActive && EDITABLE_CONTROL_TYPES.has(ctrl.type)) continue;

        switch (ctrl.type) {
            case "uint8":
            case "uint16":
            case "int16":
            case "pin": {   // pin is a plain number input (no slider sibling); patches the same way
                const input = document.querySelector(`input[data-mid="${mid}"][data-key="${k}"]`);
                // While the demo sweep animates a control, leave it alone: the sweep restores the
                // real value when it ends, and the next patch after that lands normally.
                if (input && surfaceDemoRunning() &&
                    (input.classList.contains("encoder-input") || input.classList.contains("fader-input"))) break;
                if (input && Number(input.value) !== Number(ctrl.value)) {
                    input.value = ctrl.value ?? 0;
                    const val = input.nextElementSibling;
                    if (val && val.classList.contains("control-value-input")) val.value = ctrl.value ?? 0;
                    redrawRangeDecorations(input);
                }
                break;
            }
            case "bool": {
                const input = document.querySelector(`input[data-mid="${mid}"][data-key="${k}"]`);
                if (input && input.checked !== !!ctrl.value) input.checked = !!ctrl.value;
                break;
            }
            case "text": {
                const input = document.querySelector(`input[type="text"][data-mid="${mid}"][data-key="${k}"]`);
                if (input && input.value !== (ctrl.value ?? "")) input.value = ctrl.value ?? "";
                break;
            }
            case "textarea": {
                const input = document.querySelector(`textarea[data-mid="${mid}"][data-key="${k}"]`);
                // Don't clobber the box while the user is typing in it.
                if (input && document.activeElement !== input && input.value !== (ctrl.value ?? "")) input.value = ctrl.value ?? "";
                break;
            }
            case "password": {
                // The peek button flips the input to type="text", so match either.
                const input = document.querySelector(`input[data-mid="${mid}"][data-key="${k}"]`);
                const decoded = decodePassword(ctrl.value);
                if (input && input.value !== decoded) input.value = decoded;
                break;
            }
            case "select": {
                const sel = document.querySelector(`select[data-mid="${mid}"][data-key="${k}"]`);
                // Never overwrite a select the user currently has OPEN (popup
                // showing) or focused. data-open is set on pointerdown/focus and
                // cleared on change/blur — more reliable than document.activeElement,
                // which is ambiguous while a native popup is up (the popup is a
                // separate OS layer on macOS). The 1s dragTs cooldown is the
                // additional fallback for the frames right after the popup closes.
                if (sel && sel.dataset.open !== "true" && sel !== document.activeElement) {
                    // Re-sync the OPTION list when it changed since render: some selects are
                    // populated asynchronously (e.g. HueDriver learns its rooms/lights ~1-2s after
                    // boot, growing this select from ["All"] to the full list). The value-only patch
                    // below can't reveal new options, so rebuild them in place when they differ.
                    const opts = ctrl.options || [];
                    const cur = Array.from(sel.options).map(o => o.textContent);
                    if (cur.length !== opts.length || opts.some((o, i) => o !== cur[i])) {
                        sel.innerHTML = "";
                        opts.forEach((opt, i) => {
                            const o = document.createElement("option");
                            o.value = i;
                            o.textContent = opt;
                            sel.appendChild(o);
                        });
                    }
                    if (Number(sel.value) !== Number(ctrl.value)) sel.value = ctrl.value;
                }
                break;
            }
            case "palette": {
                // Custom dropdown: patch the trigger (swatch + name) and the selected row, but not
                // while the user has the list open (data-open === "true").
                const wrap = document.querySelector(`.palette-control[data-mid="${mid}"][data-key="${k}"]`);
                if (wrap && wrap.dataset.open !== "true" && Number(wrap.dataset.value) !== Number(ctrl.value)) {
                    wrap.dataset.value = ctrl.value;
                    const cols = ((ctrl.options || [])[ctrl.value] || {}).colors || "";
                    const stops = cols.split(/\s+/).filter(Boolean).map(h => "#" + h);
                    const grad = stops.length ? `linear-gradient(to right, ${stops.join(",")})` : "none";
                    const triSwatch = wrap.querySelector(".palette-trigger .palette-swatch");
                    if (triSwatch) triSwatch.style.background = grad;
                    const triName = wrap.querySelector(".palette-trigger .palette-name");
                    if (triName) triName.textContent = ((ctrl.options || [])[ctrl.value] || {}).name || String(ctrl.value);
                    wrap.querySelectorAll(".palette-item.selected").forEach(x => x.classList.remove("selected"));
                    const row = wrap.querySelector(`.palette-item[data-idx="${ctrl.value}"]`);
                    if (row) row.classList.add("selected");
                }
                break;
            }
            case "display": {
                const span = document.querySelector(`span.display[data-mid="${mid}"][data-key="${k}"]`);
                if (span) span.textContent = ctrl.value ?? "";
                break;
            }
            case "display-int": {
                const span = document.querySelector(`span.display[data-mid="${mid}"][data-key="${k}"]`);
                if (span) {
                    // Re-cache the unit in case the device changed it (it
                    // shouldn't, but the WS path is the authority).
                    span.dataset.unit = ctrl.unit ?? span.dataset.unit ?? "";
                    span.textContent = fmtDisplayInt(ctrl);
                }
                break;
            }
            case "ipv4": {
                // Guarded by the shared userActive check above (same as text).
                const input = document.querySelector(`input.ipv4-input[data-mid="${mid}"][data-key="${k}"]`);
                if (input && input.value !== (ctrl.value ?? "")) input.value = ctrl.value ?? "";
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
            case "list": {
                const list = document.querySelector(`div.list-control[data-mid="${mid}"][data-key="${k}"]`);
                if (!list) break;
                const rows = Array.isArray(ctrl.value) ? ctrl.value : [];
                const details = Array.isArray(ctrl.detail) ? ctrl.detail : [];
                // ONLY REBUILD IF THE LIST ACTUALLY CHANGED. The device pushes full state ~1/sec, so an
                // unconditional rebuild destroyed + recreated every row (and its inline dropdowns / drag
                // handlers) every second — collapsing an open dropdown and breaking a drag. Compare the
                // incoming rows+details against the last rendered snapshot cached on the element; if
                // identical, do nothing (the common case, and the whole fix). Only a GENUINE change
                // (someone edited the list) rebuilds — latest state wins, no attempt to protect a stale
                // in-progress view (that would suppress the user's own edit from re-rendering). A
                // rebuild on a real change is momentary and a direct consequence of that change.
                const sig = JSON.stringify([rows, details]);
                if (list.dataset.sig === sig) break;   // unchanged — leave the DOM (and any open edit) alone
                list.dataset.sig = sig;
                // Preserve which detail panels are open across the rebuild. Capture the SAME key
                // listRowKey emits: the row's stable `id` (on entry.dataset.rowId for editable
                // rows) when present, else the summary label text. Keying on id means changing a
                // field shown in the label (a preset's channel count) keeps the row expanded.
                const open = new Set(
                    [...list.querySelectorAll(".list-entry")]
                        .filter(e => { const d = e.querySelector(".list-detail"); return d && !d.hidden; })
                        .map(e => e.dataset.rowId != null
                            ? "#" + e.dataset.rowId
                            : e.querySelector(".list-summary-label")?.textContent));
                // Preserve scroll position too: a rebuild recreates .list-scroll (scrollTop 0), which
                // would jump a long list back to the top mid-edit of a row further down.
                const prevScroll = list.querySelector(".list-scroll")?.scrollTop ?? 0;
                buildListEntries(list, rows, details, open,
                    {editable: ctrl.editable, pads: ctrl.pads, gridCols: ctrl.gridCols, gridRows: ctrl.gridRows,
                     moduleName: mod.name, ctrlName: ctrl.name, optionSets: ctrl.optionSets || {}});
                const newScroll = list.querySelector(".list-scroll");
                if (newScroll) newScroll.scrollTop = prevScroll;
                break;
            }
        }
        // Reset-button state may change as the value drifts in/out of default.
        // Defaults live in availableTypes (populated from /api/types) keyed by module type.
        const def = defaultFor(mod.type, ctrl.name);
        if (def !== undefined && def !== null) {
            const btn = document.querySelector(`button.reset-btn[data-mid="${mid}"][data-key="${k}.reset"]`);
            if (btn) {
                const eq = controlValuesEqual(ctrl, def);
                btn.classList.toggle("active", !eq);
            }
        }
    }
}

// Per-type equality for reset-button highlighting. bool→boolish, ipv4/text→
// string compare, everything else → numeric. Centralised so the rules can't
// drift between createControl and updateModuleControls.
function controlValuesEqual(ctrl, def) {
    if (ctrl.type === "bool") return !!ctrl.value === !!def;
    if (ctrl.type === "ipv4" || ctrl.type === "text" || ctrl.type === "textarea" || ctrl.type === "password") {
        return String(ctrl.value ?? "") === String(def ?? "");
    }
    return Number(ctrl.value) === Number(def);
}

function cssEscape(s) {
    // Minimal CSS attribute selector escape. Module/control names are alphanumeric
    // in practice, so this is defensive.
    return String(s).replace(/(["\\])/g, "\\$1");
}

// ---------------------------------------------------------------------------
// 6. Type picker
// ---------------------------------------------------------------------------

// Role → emoji. The role part of the MoonLight emoji-key system
// (https://moonmodules.org/MoonLight/moonlight/overview/#emoji-key):
// 🔥 effect · 💎 modifier · 🚥 layout · ☸️ driver · 🥞 layer (projectMM
// addition — every Layer instance, child of the Effects container). The role
// tag is derived here, not duplicated in every module's tags() string — one
// source of truth in the UI saves repeating the same character in ~30 module
// headers and a few bytes per type in /api/types. Each module's tags() then
// only carries its categorical origin (🐙 WLED · 💫 MoonLight · ⚡️ FastLED)
// and any feature extras (audio: ♫ FFT · ♪ volume · moving-head: 🚨 color ·
// 🗼 movement). The dimensional emoji (📏 1D · 🟦 2D · 🧊 3D) is derived from
// the type's `dim` field. All three are merged in emojiTagsFor().
const ROLE_EMOJI = {
    effect:     "🔥",
    driver:     "☸️",
    modifier:   "💎",
    layout:     "🚥",
    layer:      "🥞",
    effects:    "🥞",   // the container a preset captures; same pancake as the Layers it holds
    service:    "🛰️",
    generic:    "⚙️",
};

// Role → hue, the color half of the same vocabulary ROLE_EMOJI carries. Four capturable roles get a
// distinct hue so a preset pad says what it covers before its name is read. Hues (not full colors) so
// one value drives the face, the border and the glow at different mixes, and so several roles can be
// averaged into one tint.
const ROLE_HUE = {
    layout:   210,   // blue
    effects:  280,   // violet
    driver:   150,   // green
    service:   35,   // amber
};

/// The tint for a preset's role. A preset carries exactly one role, so this is a lookup: a file
/// naming several is one an older build wrote, and gets no tint because it cannot be applied.
function roleHue(roles) {
    const hues = (roles || []).map(r => ROLE_HUE[r]).filter(h => h != null);
    return hues.length === 1 ? String(hues[0]) : null;
}

// Dim int → emoji. Only effects carry `dim` (1/2/3); other modules have dim == 0
// and contribute nothing here. Same MoonLight key. Keeps emojiTagsFor() the
// single place that assembles the chip set per type.
const DIM_EMOJI = {
    1: "📏",
    2: "🟦",
    3: "🧊",
};

// Split a string into grapheme clusters so multi-codepoint emoji (e.g. 🌫️,
// which is base char + variation selector) stay whole. Falls back to a plain
// code-point split if Intl.Segmenter is unavailable.
const _graphemeSeg = (typeof Intl !== "undefined" && Intl.Segmenter)
    ? new Intl.Segmenter(undefined, {granularity: "grapheme"})
    : null;
function graphemes(s) {
    if (!s) return [];
    if (_graphemeSeg) return [..._graphemeSeg.segment(s)].map(seg => seg.segment);
    return [...s];
}

// All emoji for a type: role first, then dimensional (effects only), then each
// curated tag emoji from tags(). Deduplicated, order preserved.
function emojiTagsFor(t) {
    const out = [];
    const seen = new Set();
    const push = (ch) => { if (ch && !seen.has(ch)) { seen.add(ch); out.push(ch); } };
    push(ROLE_EMOJI[t.role]);
    push(DIM_EMOJI[t.dim]);
    for (const ch of graphemes(t.tags || "")) push(ch);
    return out;
}

// The type picker serves two modes:
//  - add (default): pick a type to create as a child of parentMod.
//  - replace: pick a type to swap parentMod for, at the same position.
// They differ only in the role filter and the commit action; the search box,
// list, and keyboard nav are shared.
function openTypePicker(parentMod, anchorEl) {
    const roles = rolesAcceptedBy(parentMod);
    // One candidate = no choice to make, so don't stage a picker to ask a question with one answer:
    // "+" on Effects just adds a Layer. (Same filter openPicker uses, so the two can't disagree about
    // what the candidates are.)
    const candidates = availableTypes.filter(t => roles.includes(t.role));
    if (candidates.length === 1) {
        addModule(candidates[0].name, parentMod.name);
        return;
    }
    openPicker(anchorEl, {
        roles,
        actionLabel: "create",
        commit: (type) => addModule(type, parentMod.name)
    });
}

// Replace mode: filter to the target module's own role (effect ↔ effect), and
// pre-select the module's CURRENT type so the cursor lands on it (not the first row).
function openReplacePicker(targetMod, anchorEl) {
    openPicker(anchorEl, {
        roles: [targetMod.role],
        actionLabel: "replace",
        currentType: targetMod.type,
        commit: (type) => replaceModule(targetMod.name, type)
    });
}

function openPicker(anchorEl, opts) {
    // Close any existing picker
    document.querySelectorAll(".type-picker").forEach(p => p.remove());

    // Alphabetical by display name so the picker list is scannable regardless of registration
    // order (localeCompare — case-insensitive, locale-aware).
    const filtered = availableTypes
        .filter(t => opts.roles.includes(t.role))
        .sort((a, b) => (a.displayName || a.name).localeCompare(b.displayName || b.name));

    const picker = document.createElement("div");
    picker.className = "type-picker";

    const search = document.createElement("input");
    search.type = "text";
    search.placeholder = "search…";
    search.className = "type-picker-search";
    picker.appendChild(search);

    // Emoji chip filter row — every distinct emoji across the role-filtered types.
    // Toggling chips narrows the list (AND: a type must carry all active chips).
    const activeChips = new Set();
    const chipRow = document.createElement("div");
    chipRow.className = "type-picker-chips";
    const chipEmoji = [];
    const chipSeen = new Set();
    for (const t of filtered) {
        for (const ch of emojiTagsFor(t)) {
            if (!chipSeen.has(ch)) { chipSeen.add(ch); chipEmoji.push(ch); }
        }
    }
    for (const emoji of chipEmoji) {
        const chip = document.createElement("button");
        chip.className = "type-picker-chip";
        chip.textContent = emoji;
        chip.addEventListener("click", () => {
            if (activeChips.has(emoji)) { activeChips.delete(emoji); chip.classList.remove("active"); }
            else { activeChips.add(emoji); chip.classList.add("active"); }
            refresh();
        });
        chipRow.appendChild(chip);
    }
    if (chipEmoji.length > 0) picker.appendChild(chipRow);

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
    createBtn.textContent = opts.actionLabel;
    createBtn.disabled = true;
    actions.appendChild(cancelBtn);
    actions.appendChild(createBtn);
    picker.appendChild(actions);

    let selectedType = null;

    // Types matching the search box AND all active emoji chips. The query matches
    // against both the raw typeName ("RainbowEffect") and the displayName
    // ("Rainbow") supplied by /api/types so typing either form finds the row.
    function currentMatches() {
        const q = search.value.toLowerCase();
        return filtered.filter(t => {
            if (q) {
                const raw = t.name.toLowerCase();
                const disp = (t.displayName || t.name).toLowerCase();
                if (!raw.includes(q) && !disp.includes(q)) return false;
            }
            if (activeChips.size > 0) {
                const has = new Set(emojiTagsFor(t));
                for (const chip of activeChips) if (!has.has(chip)) return false;
            }
            return true;
        });
    }

    function refresh() {
        const matches = currentMatches();
        list.innerHTML = "";
        // Highlight the module's current type if it's in the list (replace mode lands the cursor
        // on what's already there); otherwise the first row.
        let selIdx = opts.currentType ? matches.findIndex(t => t.name === opts.currentType) : -1;
        if (selIdx < 0) selIdx = 0;
        matches.forEach((t, i) => {
            const item = document.createElement("div");
            item.className = "type-picker-item" + (i === selIdx ? " selected" : "");
            const emoji = document.createElement("span");
            emoji.className = "type-picker-item-emoji";
            emoji.textContent = emojiTagsFor(t).join("");
            item.appendChild(emoji);
            // Show the factory-stripped name ("Rainbow") not the typeName
            // ("RainbowEffect"); the role text on the right already conveys
            // "effect", so repeating it in the name would just be noise.
            item.appendChild(document.createTextNode(t.displayName || t.name));
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
                opts.commit(t.name);
                picker.remove();
            });
            list.appendChild(item);
        });
        selectedType = matches.length > 0 ? matches[selIdx].name : null;
        createBtn.disabled = !selectedType;
        // Scroll the pre-selected row into view (it may be below the fold for a long list).
        const selEl = list.querySelector(".type-picker-item.selected");
        if (selEl) selEl.scrollIntoView({ block: "nearest" });
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
                opts.commit(selectedType);
                picker.remove();
            }
        } else if (e.key === "Escape") {
            picker.remove();
        }
    });

    function filteredAt(i) {
        return currentMatches()[i];
    }

    createBtn.addEventListener("click", () => {
        if (selectedType) {
            opts.commit(selectedType);
            picker.remove();
        }
    });

    anchorEl.appendChild(picker);
    refresh();
    search.focus();
}

// ---------------------------------------------------------------------------
// 7. Drag-to-reorder (HTML5 DnD on desktop; touchstart-gated on mobile)
// ---------------------------------------------------------------------------

function attachDragHandlers(card, mod) {
    card.draggable = true;

    // Why we toggle `draggable` on mousedown instead of vetoing in dragstart:
    // HTML5 dragstart's `e.target` is always the draggable element (the card),
    // not the deepest element under the mouse — so closest(".control-row")
    // never matches. The reliable signal is the *mousedown* target. Disable
    // drag at mousedown when the grab landed on a control, re-enable on
    // mouseup so the next click on the card body can still drag.
    const gate = (e) => {
        card.draggable = !e.target.closest(".control-row, .card-controls-collapse > summary");
    };
    card.addEventListener("mousedown", gate, true);   // capture: runs before the input
    card.addEventListener("touchstart", gate, {capture: true, passive: true});

    card.addEventListener("dragstart", (e) => {
        // Innermost card wins — without stopPropagation a nested child's
        // dragstart would bubble to the parent and the parent's listener
        // would overwrite dataTransfer with its own name.
        e.stopPropagation();
        e.dataTransfer.effectAllowed = "move";
        e.dataTransfer.setData("text/plain", mod.name);
        card.classList.add("dragging");
    });
    card.addEventListener("dragend", () => {
        card.classList.remove("dragging");
        document.querySelectorAll(".drag-over").forEach(c => c.classList.remove("drag-over"));
    });
    card.addEventListener("dragover", (e) => {
        // Only allow drop on a true sibling — same .card-children container.
        // Cards now nest, so equal data-depth is no longer enough: two effects
        // under different Effects share a depth but aren't siblings.
        const src = document.querySelector(".card.dragging");
        if (!src || src === card) return;
        if (src.parentElement === card.parentElement &&
            card.parentElement &&
            card.parentElement.classList.contains("card-children")) {
            e.preventDefault();
            card.classList.add("drag-over");
        }
    });
    card.addEventListener("dragleave", () => {
        card.classList.remove("drag-over");
    });
    card.addEventListener("drop", (e) => {
        e.preventDefault();
        // Innermost card wins — without stopPropagation the drop bubbles to every
        // ancestor card that also has a drop handler, firing a SECOND move onto
        // the grandparent's child list (e.g. dropping onto Mirror also dropped
        // onto the Layer card → move into Effects, index 0 → undoing the first
        // move). Same reason dragstart stops propagation above.
        e.stopPropagation();
        card.classList.remove("drag-over");
        const srcName = e.dataTransfer.getData("text/plain");
        if (!srcName || srcName === mod.name) return;
        // Insert semantics (not swap): the dropped item takes the target row's
        // slot and the others shift to fill — the standard reorderable-list
        // behaviour (Finder, Trello, VS Code, SortableJS). Because we drop ONTO a
        // row (not into a between-rows gap), the landing is the target's absolute
        // index: dragging down lands after the target, dragging up lands before
        // it. That's consistent ("take the target's slot"), just not always-before.
        //
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
// 8. Status bar wiring
// ---------------------------------------------------------------------------

function setupStatusBarButtons() {
    document.getElementById("preview-reset")?.addEventListener("click", () => {
        preview.resetCamera();
    });

    // Reboot: press once to arm, again to confirm — see armPressTwice. The glyph
    // stays (no armedText); only the title changes.
    const rebootBtn = document.getElementById("reboot-btn");
    if (rebootBtn) {
        armPressTwice(rebootBtn, rebootDevice, {armedTitle: "Click again to reboot"});
    }
    document.getElementById("theme-toggle")?.addEventListener("click", () => {
        theme = (theme === "dark") ? "light" : "dark";
        localStorage.setItem(LS_THEME, theme);
        applyTheme(theme);
        // Repaint the preview to the new theme's background — a live preview would
        // pick it up on its next frame, but an idle one (no incoming frames) needs
        // a nudge so the canvas doesn't keep the previous theme's clear color.
        preview.redraw();
    });

    // Hamburger: toggles the side nav. On wide screens it collapses/expands the
    // static column; on narrow screens (<820px) the same class drives a slide-in
    // drawer over an overlay (CSS handles the responsive difference).
    document.getElementById("nav-toggle")?.addEventListener("click", () => {
        document.body.classList.toggle("nav-open");
    });
    document.getElementById("nav-overlay")?.addEventListener("click", closeNavDrawer);
    document.addEventListener("keydown", (e) => {
        if (e.key === "Escape") closeNavDrawer();
    });
}

// Close the side nav. On wide screens this collapses the column; on narrow
// screens it dismisses the slide-in drawer + overlay.
function closeNavDrawer() {
    document.body.classList.remove("nav-open");
}

function applyTheme(t) {
    document.body.dataset.theme = t;
    const btn = document.getElementById("theme-toggle");
    if (btn) btn.textContent = (t === "dark") ? "☀" : "🌙";
}

function updateStatusBar() {
    if (!state || !state.modules) return;
    const sys = state.modules.find(m => m.type === "SystemModule")
             || state.modules.find(m => m.name === "System");
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

    // System stats: "uptime · 🧠 NNK · 🧱 NNKB"
    // 🧠 = internal-RAM free (heap progress total−used); 🧱 = largest contiguous
    // internal-RAM block (the maxBlock control, already maxInternalAllocBlock).
    // Both matter: free can be ample while fragmentation leaves no single block
    // big enough for the next allocation.
    const uptimeCtrl = ctrls.find(c => c.name === "uptime");
    const heapCtrl = ctrls.find(c => c.name === "heap");
    const blockCtrl = ctrls.find(c => c.name === "maxBlock");
    const statsEl = document.getElementById("sys-stats");
    if (statsEl) {
        const parts = [];
        if (uptimeCtrl) parts.push(uptimeCtrl.value);
        if (heapCtrl && heapCtrl.value !== undefined && heapCtrl.total) {
            const freeKb = Math.round((heapCtrl.total - heapCtrl.value) / 1024);
            parts.push("🧠 " + freeKb + "K");
        }
        // Skip on desktop, where the platform stub reports "0KB" (no real
        // block measurement / unlimited heap) — same reason heap free above
        // only shows when the heap progress control is present.
        if (blockCtrl && blockCtrl.value && blockCtrl.value !== "0KB") {
            parts.push("🧱 " + blockCtrl.value);
        }
        statsEl.textContent = parts.join(" · ");
    }

    // Hide reboot button on desktop builds — platform::reboot() just exits the process,
    // which is not useful from the UI and can be mistaken for a crash.
    const chipCtrl = ctrls.find(c => c.name === "chip");
    const rebootBtn = document.getElementById("reboot-btn");
    if (rebootBtn && chipCtrl) {
        rebootBtn.hidden = chipCtrl.value === "desktop";
    }

    // bootReason → crashed-state styling on reboot button
    const reasonCtrl = ctrls.find(c => c.name === "bootReason");
    if (rebootBtn && reasonCtrl) {
        const crashed = ["PANIC", "INT_WDT", "TASK_WDT", "BROWNOUT"].includes(reasonCtrl.value);
        rebootBtn.dataset.crashed = crashed ? "true" : "false";
        if (crashed) rebootBtn.title = "Last boot: " + reasonCtrl.value + " (click to reboot)";
        else rebootBtn.title = "Reboot device";
    }

    // Cache-first update check: instant from the localStorage cache, background-fetches only
    // when stale (>1 h). Fire-and-forget — best-effort, never blocks the status-bar render.
    checkFirmwareUpdate(false);
}

// ---------------------------------------------------------------------------
// 8b. Firmware-update badge
// ---------------------------------------------------------------------------
// Browser-side "a newer firmware is out" check, modelled on ESP32-sveltekit's
// UpdateIndicator (the upstream firmware lineage) — our own code. The device fetches
// nothing; the browser compares the running version (FirmwareUpdateModule.version, pure
// semver) against GitHub releases and, when newer AND a compatible .bin exists, shows the
// status-bar badge. Two channels:
//   - STABLE: a device compares against the newest stable release (the /latest endpoint
//     excludes prereleases). Applies to every device.
//   - DEV (latest): a device already on a prerelease build (-dev.<N>) ALSO compares against
//     the moving `latest` release, so a stale latest build is nudged to the newest latest.
//     The `latest` release's tag is "latest" (not a semver), so its version is read from the
//     per-firmware manifest (manifest-<firmware>.json carries "version", e.g. 2.1.0-dev.7).
// A stable update wins over a dev update. Cached in localStorage (1 h TTL) so it doesn't
// slow page load; a fresh check is forced when the Firmware card opens. Best-effort: any
// failure hides the badge, never throws.

const RELEASES_API = "https://api.github.com/repos/MoonModules/projectMM/releases";
const UPDATE_TTL_MS = 60 * 60 * 1000;                     // 1 h — best-effort, well under GitHub's rate limit
const PICKER_RELEASE_KEY = "projectMM.picker.releaseTag"; // install-picker restores from this on init

function safeLocalGet(key) { try { return localStorage.getItem(key); } catch (_) { return null; } }
function safeLocalSet(key, v) { try { localStorage.setItem(key, v); } catch (_) { /* ignore */ } }

// In-flight fetches keyed by cache slot. updateStatusBar() calls checkFirmwareUpdate(false)
// every WS tick (1 Hz); on a cold cache they'd each start a duplicate releases/latest request
// before the first writes the cache. Share the pending promise so concurrent callers reuse it.
const inFlightFetches = {};

// A cached JSON fetch: returns the parsed body, re-fetching only when the cache is older than
// the TTL or `force` is set, and serving stale on a fetch failure. `key` is the cache slot.
async function cachedJson(url, key, force) {
    if (!force) {
        const raw = safeLocalGet(key);
        if (raw) {
            try {
                const obj = JSON.parse(raw);
                if (Date.now() - obj.ts < UPDATE_TTL_MS) return obj.data;
            } catch (_) { /* fall through to fetch */ }
        }
    }
    // Coalesce concurrent fetches for the same slot onto one request.
    if (inFlightFetches[key]) return inFlightFetches[key];
    const p = (async () => {
        try {
            const res = await fetch(url, { headers: { accept: "application/json" } });
            if (!res.ok) throw new Error(`HTTP ${res.status}`);
            const data = await res.json();
            safeLocalSet(key, JSON.stringify({ ts: Date.now(), data }));
            return data;
        } catch (e) {
            // console.debug, not warn: an update check failing is routine and not
            // actionable (the device may simply be offline, or GitHub rate-limited),
            // so keep it out of the default console — debug is hidden unless the user
            // opts into verbose. Both callers hit api.github.com, which sends
            // Access-Control-Allow-Origin and so reads fine from the device origin;
            // the failure path here is for the no-network / rate-limit case.
            console.debug("[update] fetch failed:", url, e && e.message ? e.message : e);
            const raw = safeLocalGet(key);                   // serve stale on failure
            if (raw) {
                try {
                    const obj = JSON.parse(raw);
                    // Refresh the timestamp so the per-tick check doesn't re-attempt a
                    // failing fetch every second — back off until the next TTL window.
                    safeLocalSet(key, JSON.stringify({ ts: Date.now(), data: obj.data }));
                    return obj.data;
                } catch (_) { /* none */ }
            }
            // No stale entry to serve: NEGATIVE-CACHE the failure (data:null) with a
            // fresh timestamp so the TTL guard above suppresses the next attempt for
            // the back-off window. Without this, every status-bar render (≈4×/s on
            // each WS push) re-runs the failing fetch — an error storm in the console
            // whenever the device is offline. A null cache hit returns "no update".
            safeLocalSet(key, JSON.stringify({ ts: Date.now(), data: null }));
            return null;
        } finally {
            delete inFlightFetches[key];                     // clear once settled, ok or not
        }
    })();
    inFlightFetches[key] = p;
    return p;
}

// Read the device's running version + firmware-variant key off the FirmwareUpdateModule.
function deviceFirmwareInfo() {
    if (!state || !state.modules) return null;
    const fw = findModule("Firmware") || (state.modules.find(m => m.type === "FirmwareUpdateModule"));
    if (!fw) return null;
    const ctrls = fw.controls || [];
    const version = (ctrls.find(c => c.name === "version") || {}).value;
    const firmware = (ctrls.find(c => c.name === "firmware") || {}).value;
    return version ? { version, firmware } : null;
}

// Light the badge for an available update. `tag` is the release the picker should pre-select
// (a vX.Y.Z stable tag, or "latest"); `label` is what the badge shows.
function showUpdateBadge(badge, tag, label) {
    badge.textContent = `⬆ ${label}`;
    badge.title = `Firmware update available: ${label} — open Firmware to install`;
    badge.dataset.tag = tag;
    badge.hidden = false;
}

// Is there a newer STABLE release than the device's version, with a compatible .bin?
// Returns the stable tag (e.g. "v2.1.0") or null. /latest excludes prereleases.
async function stableUpdate(dev, force) {
    const rel = await cachedJson(`${RELEASES_API}/latest`, "projectMM.update.latest.v1", force);
    if (!rel || !rel.tag_name) return null;
    const assetNames = (rel.assets || []).map(a => a.name);
    const hasBinary = !dev.firmware ||
        assetNames.some(n => n === `firmware-${dev.firmware}-${rel.tag_name}.bin`);
    return (isNewer(rel.tag_name, dev.version) && hasBinary) ? rel.tag_name : null;
}

// For a device already on a -dev build: is the moving `latest` release newer? Returns its
// version string (e.g. "2.1.0-dev.7") or null. The latest release's tag is "latest"; its
// version is published as the release `name` (release.yml), which the GitHub API exposes
// CORS-readably — unlike the manifest-*.json asset, whose release-asset URL redirects to
// release-assets.githubusercontent.com (no CORS header), so the device-hosted UI can't read it.
// We also require the matching firmware .bin asset so the badge never points at a build the
// device can't install.
async function devUpdate(dev, force) {
    if (!dev.firmware) return null;                          // can't match an asset without the key
    const rel = await cachedJson(`${RELEASES_API}/tags/latest`, "projectMM.update.dev.v1", force);
    const v = rel && rel.name;
    if (!v) return null;
    // Assets are versioned, not tagged: the `latest` release ships
    // firmware-<fw>-v<version>.bin (release.yml stages PREFIX="firmware-...-v$V").
    const hasBinary = (rel.assets || []).some(a => a.name === `firmware-${dev.firmware}-v${v}.bin`);
    return (hasBinary && isNewer(v, dev.version)) ? v : null;
}

// Show/hide the badge. `force` bypasses the cache (used when the Firmware card opens).
// Stable update takes precedence; a -dev device additionally checks the latest channel.
async function checkFirmwareUpdate(force) {
    const badge = document.getElementById("fw-update-badge");
    if (!badge) return;
    const dev = deviceFirmwareInfo();
    if (!dev) { badge.hidden = true; return; }

    const stableTag = await stableUpdate(dev, force);
    if (stableTag) { showUpdateBadge(badge, stableTag, stableTag); return; }

    // Only a prerelease (-dev…) build follows the moving latest channel; a stable device is
    // not nudged toward an unreleased build.
    const onPrerelease = (parse(dev.version)?.prerelease.length || 0) > 0;
    if (onPrerelease) {
        const devVer = await devUpdate(dev, force);
        if (devVer) { showUpdateBadge(badge, "latest", `latest (${devVer})`); return; }
    }
    badge.hidden = true;
}

// Badge click → pre-select the new release in the picker (it restores from PICKER_RELEASE_KEY
// on init) and open the Firmware card, so the user lands one click from Install.
function setupUpdateBadge() {
    const badge = document.getElementById("fw-update-badge");
    if (!badge) return;
    badge.addEventListener("click", () => {
        if (badge.dataset.tag) safeLocalSet(PICKER_RELEASE_KEY, badge.dataset.tag);
        selectModule("Firmware");
    });
}

// ---------------------------------------------------------------------------
// 8b. File Manager view
// ---------------------------------------------------------------------------

// Format a byte count as a short human size (matches the fs seam's uint32 sizes).
function fmSize(n) {
    if (n < 1024) return n + " B";
    if (n < 1024 * 1024) return (n / 1024).toFixed(1) + " KB";
    return (n / (1024 * 1024)).toFixed(1) + " MB";
}

// Per-module File Manager UI state, kept across state refreshes (the DOM is rebuilt on refetch, but
// the tree's expand-state + selection are the user's, not the module's). Keyed by module name.
const fmStateByMod = {};
function fmState(mod) {
    return (fmStateByMod[mod.name] ||= { expanded: new Set(["/"]), selected: "/" });
}

// Fetch one directory's children (name/isDir/size) from /api/dir. `hidden` includes dotfiles.
async function fmFetchDir(absPath, hidden) {
    const res = await fetch("/api/dir?path=" + encodeURIComponent(absPath) + (hidden ? "&hidden=1" : ""));
    if (!res.ok) throw new Error(await errorMessage(res));
    const rows = await res.json();
    return Array.isArray(rows) ? rows : [];
}

// Render the File Manager panel: a lazy expand/collapse folder tree (the standard VS Code / Explorer
// shape — an expanded folder's children are loaded from /api/dir), plus a toolbar (show
// hidden, new folder, delete on the selected node). Filesystem ops go through the module's controls
// (path/new folder/delete); browsing is pure UI over /api/dir, so the module stays minimal.
function renderFileManager(mod, host) {
    const ctrl = (n) => (mod.controls || []).find(c => c.name === n);
    const st = fmState(mod);
    // The toggle is UI-owned: seed it from the persisted control on first render, then `st` is the
    // source of truth. Reading it back from `mod` each render would revert the checkbox, since an
    // in-panel re-render runs against the same (stale) state snapshot, before /api/state updates.
    if (st.showHidden === undefined) st.showHidden = !!ctrl("show hidden")?.value;
    const hidden = st.showHidden;

    // Render into a single stable panel that we REPLACE on every re-render (expand / collapse /
    // select / op) — reusing it in place rather than appending, so the tree updates inline instead
    // of stacking a fresh copy below the old one.
    let panel = host.querySelector(":scope > .fm-panel");
    if (panel) panel.replaceChildren();
    else { panel = document.createElement("div"); panel.className = "fm-panel"; host.appendChild(panel); }

    // Show-hidden toggle: owned by the panel (not the generic control list) so flipping it can
    // re-fetch the tree with the new `hidden` filter immediately, rather than waiting for a state
    // refresh that never re-runs the /api/dir fetch. Reuses the same .switch pill markup every
    // other bool control uses (common patterns first), so it reads consistently.
    const hiddenRow = document.createElement("label");
    hiddenRow.className = "fm-hidden-toggle";
    hiddenRow.appendChild(document.createTextNode("show hidden"));
    const sw = document.createElement("span");
    sw.className = "switch";
    const hiddenBox = document.createElement("input");
    hiddenBox.type = "checkbox";
    hiddenBox.checked = hidden;
    hiddenBox.addEventListener("change", () => {
        st.showHidden = hiddenBox.checked;                    // UI-owned source of truth
        sendControl(mod.name, "show hidden", hiddenBox.checked);   // persist (best-effort, no await)
        renderFileManager(mod, host);                         // re-list with the new filter
    });
    const track = document.createElement("span");
    track.className = "switch-track";
    sw.appendChild(hiddenBox);
    sw.appendChild(track);
    hiddenRow.appendChild(sw);
    panel.appendChild(hiddenRow);

    // Select a path level (from a breadcrumb crumb): update the selection, tidy the tree to just the
    // path, and re-render. "Reveal and collapse siblings" — keep the ancestor chain root→…→crumb
    // open, fold every other branch AND the clicked node's own descendants. Selecting a directory
    // drives where ＋ folder/＋ file create and what delete targets.
    const selectPath = (absPath, isDir) => {
        st.selected = absPath;
        st.selectedIsDir = isDir;
        // Rebuild `expanded` as the ancestor chain root→…→crumb, plus the clicked node itself when
        // it's a directory (so you see one level into it) — every other branch and anything deeper
        // folds. Root is always expanded (the tree shows its children).
        const segs = absPath.split("/").filter(Boolean);   // "/.config/foo" → [".config","foo"]
        st.expanded = new Set(["/"]);
        for (let i = 0; i < segs.length; i++) {
            const p = "/" + segs.slice(0, i + 1).join("/");
            // Include every ancestor; include the clicked node only if it's a directory.
            if (i < segs.length - 1 || isDir) st.expanded.add(p);
        }
        renderFileManager(mod, host);
    };

    // Breadcrumb of the selected path, on its OWN row above the toolbar (a deep path wraps freely
    // without crowding the buttons — key on narrow displays). Click a crumb to jump there; `root`
    // deselects back to /.
    const crumbs = document.createElement("div");
    crumbs.className = "fm-crumbs";
    const mkCrumb = (label, absPath, isDir) => {
        const b = document.createElement("button");
        b.className = "fm-crumb" + (absPath === st.selected ? " fm-crumb--here" : "");
        b.textContent = label;
        b.addEventListener("click", () => selectPath(absPath, isDir));
        return b;
    };
    // (Re)build the breadcrumb from the current selection, into the stable `crumbs` element — called
    // on first render and by refreshSelectionControls() when a file click updates the selection in place.
    const rebuildCrumbs = () => {
        crumbs.replaceChildren();
        crumbs.appendChild(mkCrumb("root", "/", true));   // always present — the way back to /
        const segs = st.selected.split("/").filter(Boolean);   // "/.config/foo" → [".config","foo"]
        segs.forEach((s, i) => {
            crumbs.appendChild(document.createTextNode(" / "));
            // A crumb is a directory unless it's the last segment of a selected *file*.
            const isLast = i === segs.length - 1;
            const isDir = !isLast || st.selectedIsDir;
            crumbs.appendChild(mkCrumb(s, "/" + segs.slice(0, i + 1).join("/"), isDir));
        });
    };
    rebuildCrumbs();
    panel.appendChild(crumbs);

    // Refresh the controls that depend on the current selection WITHOUT rebuilding the tree: the
    // breadcrumb and the delete button's disabled state. Used by the in-place file-click path so a
    // file's row survives for its dblclick. (delBtn's press-twice handler reads st.selected live.)
    const refreshSelectionControls = () => {
        rebuildCrumbs();
        delBtn.disabled = st.selected === "/";
    };

    // Toolbar (own row below the breadcrumb): New folder / New file / Delete / Refresh.
    const bar = document.createElement("div");
    bar.className = "fm-bar";

    // A filesystem op is an HTTP call on /api/dir?path= (POST=mkdir, DELETE=remove) — the path
    // rides the query, so nothing is stored/persisted on the device. On failure, surface the
    // server's error; on success, re-render the tree from disk.
    const runOp = async (op, targetPath) => {
        const method = op === "delete" ? "DELETE" : "POST";
        try {
            const res = await fetch("/api/dir?path=" + encodeURIComponent(targetPath), { method });
            if (!res.ok) alert(`${op} failed: ${await errorMessage(res)}`);
        } catch (err) {   // a network error (offline / reset) would otherwise be an unhandled rejection
            alert(`${op} failed: ${err.message || err}`);
        }
        renderFileManager(mod, host);  // rebuild the tree from /api/dir (fresh listing), success or handled failure
    };

    // A new file/folder is created inside the selected node if it's a folder, else next to it (in
    // the selected file's parent). Shared by both create buttons.
    const createBase = () => (st.selectedIsDir ? st.selected : fmParent(st.selected));

    // Icon-only toolbar: each button shows a glyph; the `title` carries the word for the tooltip +
    // screen readers (the standard icon-button pattern — an icon with an accessible label).
    const newBtn = document.createElement("button");
    newBtn.className = "fm-tool fm-tool--icon";
    newBtn.textContent = "📁";
    newBtn.title = "New folder — create a folder inside the selected folder";
    newBtn.addEventListener("click", async () => {
        const base = createBase();
        const name = (prompt("New folder name in " + base + ":") || "").trim();
        if (!name) return;             // blank or whitespace-only → no-op
        st.expanded.add(base);         // reveal the new child
        await runOp("new folder", joinFsPath(base, name));
    });
    bar.appendChild(newBtn);

    // New file: no module op needed — the /api/file POST creates a file at a path (empty body), the
    // same endpoint the editor saves through. Then re-render the tree from disk.
    const newFileBtn = document.createElement("button");
    newFileBtn.className = "fm-tool fm-tool--icon";
    newFileBtn.textContent = "📝";
    newFileBtn.title = "New file — create an empty file inside the selected folder";
    newFileBtn.addEventListener("click", async () => {
        const base = createBase();
        const name = (prompt("New file name in " + base + ":") || "").trim();
        if (!name) return;             // blank or whitespace-only → no-op
        const filePath = joinFsPath(base, name);
        try {
            const res = await fetch("/api/file?path=" + encodeURIComponent(filePath), {
                method: "POST", headers: { "Content-Type": "text/plain" }, body: "",
            });
            if (!res.ok) throw new Error(await errorMessage(res));
        } catch (err) {
            alert("create file failed: " + err.message);
            return;
        }
        st.expanded.add(base);         // reveal the new file
        renderFileManager(mod, host);  // re-list from disk
    });
    bar.appendChild(newFileBtn);

    // Upload: pick desktop files and stream them into the selected folder via the same /api/file
    // POST the drag-drop path uses (fmDropUpload) — a button for people who don't drag. A hidden
    // <input type=file multiple> is the recognizable browser file-picker; clicking the button opens it.
    const upBtn = document.createElement("button");
    upBtn.className = "fm-tool fm-tool--icon";
    upBtn.textContent = "↥";   // pairs with the per-row ↓ download glyph
    upBtn.title = "Upload — upload files from your computer into the selected folder";
    const upInput = document.createElement("input");
    upInput.type = "file";
    upInput.multiple = true;
    upInput.style.display = "none";
    upInput.addEventListener("change", async () => {
        const files = Array.from(upInput.files || []);
        upInput.value = "";                       // reset so re-picking the same file re-fires change
        if (!files.length) return;
        const base = createBase();
        st.expanded.add(base);                    // reveal the destination folder
        const skipped = await fmDropUpload(base, files);
        renderFileManager(mod, host);             // re-list from disk
        if (skipped.length) alert("Not uploaded:\n" + skipped.join("\n"));
    });
    upBtn.addEventListener("click", () => upInput.click());
    bar.appendChild(upBtn);
    bar.appendChild(upInput);

    const delBtn = document.createElement("button");
    delBtn.className = "fm-tool fm-tool--icon fm-tool--danger";
    delBtn.textContent = "🗑";
    delBtn.title = "Delete — delete the selected file or empty folder";
    delBtn.disabled = st.selected === "/";   // never delete the root
    armPressTwice(delBtn, () => runOp("delete", st.selected), { armedText: "✓" });
    bar.appendChild(delBtn);

    const refBtn = document.createElement("button");
    refBtn.className = "fm-tool fm-tool--icon";
    refBtn.textContent = "⟳";
    refBtn.title = "Refresh";
    refBtn.addEventListener("click", () => renderFileManager(mod, host));
    bar.appendChild(refBtn);
    panel.appendChild(bar);

    // The tree. Root ("/") is always present and expanded; its children populate asynchronously.
    const tree = document.createElement("div");
    tree.className = "fm-tree";
    // Dropping desktop files onto the tree's empty space uploads them into root.
    fmMakeDropTarget(tree, "/", hidden, () => renderFileManager(mod, host), st);
    panel.appendChild(tree);

    // Render one directory's children into `container` at `depth`, recursing into expanded folders.
    const renderChildren = async (dirPath, container, depth) => {
        let rows;
        try {
            rows = await fmFetchDir(dirPath, hidden);
        } catch (err) {
            const e = document.createElement("div");
            e.className = "fm-empty";
            e.textContent = "list failed: " + err.message;
            container.appendChild(e);
            return;
        }
        if (rows.length === 0) {
            const e = document.createElement("div");
            e.className = "fm-empty";
            e.style.paddingLeft = (depth * 16 + 20) + "px";
            e.textContent = "empty";
            container.appendChild(e);
            return;
        }
        // Folders first, then files; each alphabetical — the conventional file-manager sort.
        rows.sort((a, b) => (b.isDir - a.isDir) || a.name.localeCompare(b.name));
        for (const entry of rows) {
            const childPath = joinFsPath(dirPath, entry.name);
            const isOpen = st.expanded.has(childPath);

            const rowEl = document.createElement("div");
            rowEl.className = "fm-row" + (childPath === st.selected ? " fm-row--sel" : "");
            rowEl.style.paddingLeft = (depth * 16 + 4) + "px";

            // Chevron: only folders can expand; a file gets a spacer so names line up.
            const chev = document.createElement("span");
            chev.className = "fm-chev";
            chev.textContent = entry.isDir ? (isOpen ? "▾" : "▸") : "";
            rowEl.appendChild(chev);

            const icon = document.createElement("span");
            icon.className = "fm-icon";
            icon.textContent = entry.isDir ? (isOpen ? "📂" : "📁") : "📄";
            rowEl.appendChild(icon);

            const name = document.createElement("span");
            name.className = "fm-name";
            name.textContent = entry.name;
            rowEl.appendChild(name);

            const size = document.createElement("span");
            size.className = "fm-size";
            size.textContent = entry.isDir ? "" : fmSize(entry.size || 0);
            rowEl.appendChild(size);

            // Per-file download (device → desktop): a plain <a download> on /api/file forces a save
            // with the right name, every browser, any file type — the portable counterpart to the
            // drag-drop upload (a true drag-*out* has no cross-browser API). Folders get no ⤓;
            // folder-as-zip is backlogged (needs a bundled zip lib + recursion).
            if (!entry.isDir) {
                const dl = document.createElement("a");
                dl.className = "fm-dl";
                dl.textContent = "⤓";
                dl.title = "download";
                dl.href = "/api/file?path=" + encodeURIComponent(childPath);
                dl.setAttribute("download", entry.name);
                dl.addEventListener("click", (ev) => ev.stopPropagation());   // don't select/open
                rowEl.appendChild(dl);
            }

            // Single-click SELECTS (highlights, sets the op target for delete); a folder also
            // toggles expand. DOUBLE-click OPENS a file in the editor — the VS Code / Finder /
            // Explorer norm, and it keeps a distinct "selected but not opened" state for future
            // rename / multi-select / context-menu features.
            rowEl.addEventListener("click", (ev) => {
                ev.stopPropagation();
                st.selected = childPath;
                st.selectedIsDir = entry.isDir;
                if (entry.isDir) {
                    // A folder click also expands/collapses — rows appear/disappear, so a full
                    // re-render is needed.
                    if (isOpen) st.expanded.delete(childPath);
                    else st.expanded.add(childPath);
                    renderFileManager(mod, host);
                } else {
                    // A file click only moves the selection — update the highlight IN PLACE, never
                    // re-render. A re-render here would destroy this row mid-gesture, so the dblclick
                    // that follows a double-click would land on a fresh element and openFileEditor
                    // might not fire. Move the --sel class + refresh the selection-dependent controls.
                    for (const r of panel.querySelectorAll(".fm-row--sel")) r.classList.remove("fm-row--sel");
                    rowEl.classList.add("fm-row--sel");
                    refreshSelectionControls();
                }
            });
            if (!entry.isDir) {
                rowEl.addEventListener("dblclick", (ev) => {
                    ev.stopPropagation();
                    openFileEditor(childPath, entry.size);   // size lets the editor detect a truncated read
                });
            }

            // Drag-drop upload target: dropping desktop files onto a FOLDER row uploads them into
            // that folder (tier 1: text/config ≤8KB — see fmDropUpload).
            if (entry.isDir) fmMakeDropTarget(rowEl, childPath, hidden, () => renderFileManager(mod, host), st);
            container.appendChild(rowEl);

            // Recurse into an expanded folder (its own indented sub-container).
            if (entry.isDir && isOpen) {
                const sub = document.createElement("div");
                sub.className = "fm-subtree";
                container.appendChild(sub);
                await renderChildren(childPath, sub, depth + 1);
            }
        }
    };

    renderChildren("/", tree, 0);

    // Filesystem usage bar below the tree — the File Manager's own `filesystem` control (used /
    // total bytes from the platform). Absent (e.g. desktop fs total 0) → nothing shown.
    const fsCtrl = fmFilesystemUsage(mod);
    if (fsCtrl) {
        const usage = document.createElement("div");
        usage.className = "fm-usage";
        const name = document.createElement("span");
        name.className = "fm-usage-name";
        name.textContent = "Used";      // the bar/value is used space out of total (see the trailing label)
        const bar = document.createElement("progress");
        bar.value = Number(fsCtrl.value) || 0;
        bar.max = Number(fsCtrl.total) || 1;
        const lbl = document.createElement("span");
        lbl.className = "fm-usage-label";
        lbl.textContent = fmtProgressLabel(fsCtrl);
        usage.appendChild(name);
        usage.appendChild(bar);
        usage.appendChild(lbl);
        panel.appendChild(usage);
    }

    // "last saved" readout below the usage bar — how long ago config was persisted. The value is
    // owned by the FilesystemModule engine (non-UI); the File Manager displays it because this is
    // where filesystem state is topical (same reasoning as the usage bar).
    const savedCtrl = (mod?.controls || []).find(c => c.name === "lastSaved");
    if (savedCtrl) {
        const row = document.createElement("div");
        row.className = "fm-lastsaved";
        row.textContent = `saved: ${savedCtrl.value ?? "never"}`;
        panel.appendChild(row);
    }
}

// The File Manager's own `filesystem` usage progress control (used/total bytes), or null if the
// platform reports no partition. Rendered as the bar below the tree; skipped in the generic control
// loop so it appears only here.
function fmFilesystemUsage(mod) {
    return (mod?.controls || []).find(c => c.name === "filesystem") || null;
}

// Format a file's text for the editor, by extension. JSON is re-indented (2 spaces) so the persisted
// config files are readable; anything that doesn't parse is shown verbatim rather than erroring.
// Extension seam for later: MoonLive `.ml` source wants syntax *highlighting* (a color layer over
// the textarea), not reformatting — that's a bigger editor change, added when MoonLive `.ml` files
// land on disk.
function fmPrettify(text, relPath) {
    if (relPath.endsWith(".json")) {
        try { return JSON.stringify(JSON.parse(text), null, 2); } catch (_) {}
    }
    return text;
}

// The parent directory of an absolute path ("/a/b" → "/a", "/a" → "/").
function fmParent(absPath) {
    const cut = absPath.lastIndexOf("/");
    return cut <= 0 ? "/" : absPath.slice(0, cut);
}

// Join a dir + name with one slash (UI-side path building for the editor's path= query).
function joinFsPath(dir, name) {
    return dir.endsWith("/") ? dir + name : dir + "/" + name;
}

// Drag-drop upload (desktop → device). The device streams the body straight to the file (any size,
// binary-safe), so the only client-side bound is a sanity cap matching the device's kUploadMax; a
// file over it is skipped with a visible note (no silent drop). A too-big-for-free-space file is
// also rejected device-side with a "not enough space" message.
const FM_UPLOAD_CAP = 256 * 1024;   // matches HttpServerModule::kUploadMax

// Wire an element as a drop target that uploads dropped files into `destDir`, then re-renders.
function fmMakeDropTarget(el, destDir, hidden, rerender, st) {
    el.addEventListener("dragover", (e) => {
        e.preventDefault();
        e.stopPropagation();
        el.classList.add("fm-row--drop");
    });
    el.addEventListener("dragleave", (e) => {
        e.stopPropagation();
        el.classList.remove("fm-row--drop");
    });
    el.addEventListener("drop", async (e) => {
        e.preventDefault();
        e.stopPropagation();
        el.classList.remove("fm-row--drop");
        const files = Array.from(e.dataTransfer?.files || []);
        if (!files.length) return;
        const skipped = await fmDropUpload(destDir, files);
        if (destDir !== "/") st.expanded.add(destDir);   // reveal where they landed
        rerender();
        if (skipped.length) {
            // Report what wasn't uploaded (over the size cap or a write error) rather than dropping
            // it silently. The limit is derived from FM_UPLOAD_CAP so the text never drifts from it.
            alert(`Not uploaded (over ${fmSize(FM_UPLOAD_CAP)} or failed):\n` + skipped.join("\n"));
        }
    });
}

// Upload each dropped file into destDir via /api/file. Returns the names skipped (too big / failed)
// so the caller can report them. The File blob is sent as the body directly — the browser streams
// its raw bytes (binary-safe), matching the device's streamed, byte-exact write.
async function fmDropUpload(destDir, files) {
    const skipped = [];
    for (const file of files) {
        if (file.size > FM_UPLOAD_CAP) { skipped.push(file.name + " (" + fmSize(file.size) + ")"); continue; }
        try {
            const res = await fetch("/api/file?path=" + encodeURIComponent(joinFsPath(destDir, file.name)), {
                method: "POST", headers: { "Content-Type": "application/octet-stream" }, body: file,
            });
            if (!res.ok) throw new Error(await errorMessage(res));   // surfaces "not enough space (N free)" etc.
        } catch (err) {
            skipped.push(file.name + " (" + err.message + ")");
        }
    }
    return skipped;
}

// Open a modal text editor for the file at `relPath`. Loads via GET /api/file (streamed whole, any
// size), saves via POST. A file that isn't valid text (a NUL byte, or UTF-8 decode damage) loads
// read-only so a lossy re-save can't corrupt it. Uses the native <dialog> — no bespoke overlay code.
async function openFileEditor(relPath, expectedSize) {
    const dlg = document.createElement("dialog");
    dlg.className = "fm-editor";
    dlg.innerHTML =
        '<form method="dialog" class="fm-editor-head">' +
        '  <span class="fm-editor-path"></span>' +
        '  <button value="close" class="fm-editor-x" title="close">✕</button>' +
        '</form>' +
        '<textarea class="fm-editor-body" spellcheck="false"></textarea>' +
        '<div class="fm-editor-foot">' +
        '  <span class="fm-editor-status"></span>' +
        '  <button class="action-btn fm-editor-save">Save</button>' +
        '</div>';
    dlg.querySelector(".fm-editor-path").textContent = relPath;
    const body = dlg.querySelector(".fm-editor-body");
    const status = dlg.querySelector(".fm-editor-status");
    const saveBtn = dlg.querySelector(".fm-editor-save");
    document.body.appendChild(dlg);
    dlg.addEventListener("close", () => dlg.remove());
    dlg.showModal();

    try {
        const res = await fetch("/api/file?path=" + encodeURIComponent(relPath));
        // Surface the server's own message (e.g. "not found") rather than a bare status code.
        if (!res.ok) throw new Error(await errorMessage(res));
        const text = await res.text();
        // Truncation guard: serveFileContents streams the whole file but stops short on a read error
        // (a filesystem fault mid-stream). Saving a short read back would overwrite the file with a
        // truncated copy — so if the received byte count is under the size the listing reported, load
        // read-only. TextEncoder gives the byte length (text.length is chars, not bytes).
        if (typeof expectedSize === "number" &&
            new TextEncoder().encode(text).length < expectedSize) {
            body.value = text;
            body.readOnly = true;
            saveBtn.disabled = true;
            status.textContent = "truncated read — read-only (save would corrupt the file)";
        // The editor is text/config only: a <textarea> can't faithfully round-trip non-text bytes,
        // so a re-save would corrupt the file. Treat it as binary — read-only, save disabled — if it
        // has a NUL OR if res.text()'s UTF-8 decode left a replacement char (U+FFFD), which means the
        // bytes weren't valid UTF-8 and are already lossy in the textarea. Use the per-row ⤓ to
        // download such files intact.
        } else if (text.indexOf("\0") !== -1 || text.indexOf("�") !== -1) {
            body.value = text;
            body.readOnly = true;
            saveBtn.disabled = true;
            status.textContent = "binary / non-text file — read-only";
        } else {
            body.value = fmPrettify(text, relPath);
        }
    } catch (err) {
        body.value = "";
        status.textContent = "load failed: " + err.message;
        saveBtn.disabled = true;   // never let a Save post an empty body over a file that failed to load
    }

    saveBtn.addEventListener("click", async () => {
        status.textContent = "saving…";
        try {
            const res = await fetch("/api/file?path=" + encodeURIComponent(relPath), {
                method: "POST",
                headers: { "Content-Type": "text/plain" },
                body: body.value,
            });
            if (!res.ok) throw new Error(await errorMessage(res));
            status.textContent = "saved";
        } catch (err) {
            status.textContent = "save failed: " + err.message;
        }
    });
}

// ---------------------------------------------------------------------------
// 9. Boot
// ---------------------------------------------------------------------------

document.addEventListener("DOMContentLoaded", init);
