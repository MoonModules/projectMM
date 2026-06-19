// projectMM install orchestrator — replaces ESP Web Tools' install button.
//
// Why a custom orchestrator: ESP Web Tools 10.x holds the SerialPort
// exclusively (OS-level Web Serial exclusivity), and its post-PROVISIONED
// state-changed event is fired inside the dialog's shadow DOM — invisible
// to the host page. That made it impossible for Step 2 of the
// board-injection plan to push the picked board to the device after WiFi
// provisioning (and silently broke devices.js's "Your devices" auto-add
// for the same reason). Step 3 owns the SerialPort end-to-end so both
// fixes can land.
//
// Flow:
//   1. user clicks Install → requestPort()
//   2. esptool-js flashes the binaries from the manifest
//   3. release the flash locks, hand the same port to ImprovSerial
//   4. show a WiFi creds form, await user input
//   5. provision via Improv standard SEND_WIFI_CREDENTIALS
//   6. send SET_BOARD vendor RPC (0xFE) with the picked board name
//   7. callback with { url, board } so the host page populates
//      "Your devices" + shows a "Visit device" link
//
// Dependencies via CDN ES modules. Pinned exact versions because version
// churn in either lib could silently break the flow (esptool-js' API has
// shifted across minor versions; improv-wifi-serial-sdk's writePacketToStream
// is private and could be renamed). Date the pins so future bumps are
// intentional. Pinned 2026-06-03.
import { ESPLoader, Transport } from "https://unpkg.com/esptool-js@0.4.7/bundle.js?module";
import { ImprovSerial } from "https://unpkg.com/improv-wifi-serial-sdk@2.5.0/dist/serial.js?module";

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// SET_BOARD vendor RPC command ID. High end of the conventional 0x80-0xFE
// vendor extension range to maximize headroom against future Improv-spec
// expansion into the low vendor range. Matches the device-side handler at
// src/platform/esp32/platform_esp32_improv.cpp.
const IMPROV_CMD_SET_BOARD = 0xFE;

// SET_TX_POWER vendor RPC command ID — the pre-association TX-power cap for
// boards whose LDO browns out at full power (a thin on-module LDO or marginal
// USB supply, e.g. some S2/S3 mini-class boards). Their boards.json
// cap (controls.Network.txPowerSetting) used to arrive only via the HTTP
// fan-out AFTER the device was online, which a browning-out board can never
// reach: it fails WiFi auth at 20 dBm first (proven on the bench 2026-06-10).
// Sent BEFORE provisioning so the very first association runs capped.
// Matches the device-side handler at src/platform/esp32/platform_esp32_improv.cpp.
const IMPROV_CMD_SET_TX_POWER = 0xFD;

// Improv frame type for RPC commands (matches src/core/ImprovFrame.h).
const IMPROV_FRAME_TYPE_RPC = 0x03;

// Magic bytes that prefix every Improv frame. ASCII "IMPROV".
const IMPROV_MAGIC = [0x49, 0x4d, 0x50, 0x52, 0x4f, 0x56];

// ---------------------------------------------------------------------------
// Manifest parser
// ---------------------------------------------------------------------------

// Fetches an ESP Web Tools manifest and returns the flashable parts as
// { chipFamily, parts: [{ url, offset }] }. Validates the shape so a
// missing builds[] or parts[] errors here rather than confusing
// esptool-js with undefined data.
//
// Manifest shape (single build per file — projectMM generates one per
// firmware variant; see scripts/build/generate_manifest.py):
//   { name, version, builds: [{ chipFamily, parts: [{ path, offset }] }] }
async function fetchManifest(manifestUrl) {
    // Resolve the manifest URL against the document so a relative path like
    // "./releases/latest/manifest-esp32.json" (what toLocalUrl returns on the
    // preview/Pages-hosted setup) becomes absolute. Without this, the
    // `new URL(".", manifestUrl)` below would throw "Invalid base URL" since
    // URL's two-arg form needs the second arg to be absolute.
    const absManifestUrl = new URL(manifestUrl, document.baseURI).toString();
    const res = await fetch(absManifestUrl);
    if (!res.ok) throw new Error(`manifest HTTP ${res.status}`);
    const m = await res.json();
    if (!m || !Array.isArray(m.builds) || m.builds.length === 0) {
        throw new Error("manifest missing builds[]");
    }
    const build = m.builds[0];
    if (!Array.isArray(build.parts) || build.parts.length === 0) {
        throw new Error("manifest build missing parts[]");
    }
    // Resolve part paths against the (now absolute) manifest URL.
    const base = new URL(".", absManifestUrl);
    return {
        chipFamily: build.chipFamily,
        parts: build.parts.map(p => ({
            url: new URL(p.path, base).toString(),
            offset: p.offset,
        })),
    };
}

// Convert an ArrayBuffer to a "binary string" — one JS character per byte,
// codes 0x00-0xFF. esptool-js's writeFlash expects each fileArray entry's
// `data` in this shape (it iterates via .charCodeAt()). Chunked at 16 KB
// because `String.fromCharCode(...big_array)` blows the call stack on
// large inputs (a 1.2 MB app image would otherwise spread ~1.2M arguments).
function bufferToBinaryString(buffer) {
    const bytes = new Uint8Array(buffer);
    const CHUNK = 16384;
    let out = "";
    for (let i = 0; i < bytes.length; i += CHUNK) {
        out += String.fromCharCode.apply(
            null,
            bytes.subarray(i, Math.min(i + CHUNK, bytes.length))
        );
    }
    return out;
}

// ---------------------------------------------------------------------------
// Improv frame encoder (fallback for the private writePacketToStream)
// ---------------------------------------------------------------------------

// Mirrors buildImprovFrame in src/core/ImprovFrame.h. The wire format is:
//   [I][M][P][R][O][V][version=1][type][length][payload×length][checksum]
// Checksum = sum-mod-256 of the first 9+length bytes.
function buildImprovFrame(type, payload) {
    const len = payload.length;
    const frame = new Uint8Array(6 + 1 + 1 + 1 + len + 1);
    frame.set(IMPROV_MAGIC, 0);
    frame[6] = 0x01;   // version
    frame[7] = type;
    frame[8] = len;
    frame.set(payload, 9);
    let sum = 0;
    for (let i = 0; i < 9 + len; i++) sum = (sum + frame[i]) & 0xff;
    frame[9 + len] = sum;
    return frame;
}

// Encodes the SET_BOARD RPC payload that the device parser at
// platform_esp32_improv.cpp::improvHandleSetBoard expects.
//
// RPC payload (inside the Improv frame, before the checksum):
//   [0xFE]          command
//   [data_len]      1 + str_len
//   [str_len]       1..31, length of board name in bytes
//   [str_bytes]     ASCII-printable 0x20..0x7E only
//
// The 31-char cap mirrors BoardModule::boardKey_'s 32-byte buffer
// (sizeof - 1 for NUL); the device-side handler validates against
// g_improv.boardOutLen dynamically, so the wire spec follows the buffer.
function encodeSetBoardPayload(board) {
    const nameBytes = new TextEncoder().encode(board);
    if (nameBytes.length === 0 || nameBytes.length > 31) {
        throw new Error(`board name length ${nameBytes.length}: must be 1..31`);
    }
    const out = new Uint8Array(3 + nameBytes.length);
    out[0] = IMPROV_CMD_SET_BOARD;
    out[1] = 1 + nameBytes.length;
    out[2] = nameBytes.length;
    out.set(nameBytes, 3);
    return out;
}

// Sends the SET_BOARD frame on a port we own. ImprovSerial's
// writePacketToStream is private (verified in improv-wifi-serial-sdk@2.5.0's
// serial.d.ts), so we encode the frame ourselves and write raw bytes.
// ImprovSerial holds the writable stream's lock during its lifetime — to
// get our own writer we temporarily release ImprovSerial's hold by
// disconnecting then reconnecting. Easier alternative: write while
// ImprovSerial is still active is blocked, so we close ImprovSerial first
// (we're done with it — the WiFi provision succeeded) and write directly.
async function sendSetBoardFrame(port, board) {
    const payload = encodeSetBoardPayload(board);
    const frame = buildImprovFrame(IMPROV_FRAME_TYPE_RPC, payload);
    const writer = port.writable.getWriter();
    try {
        await writer.write(frame);
    } finally {
        writer.releaseLock();
    }
}

// Sends the SET_TX_POWER frame ([0xFD][1][dBm]) on a port we own — called
// BEFORE ImprovSerial takes the port's locks, so no close/reopen dance is
// needed. Fire-and-forget like SET_BOARD: the device acks with RpcResponse
// we don't read; the HTTP fan-out later re-applies the same boards.json
// value as the late fallback.
async function sendSetTxPowerFrame(port, dBm) {
    const frame = buildImprovFrame(IMPROV_FRAME_TYPE_RPC,
                                   new Uint8Array([IMPROV_CMD_SET_TX_POWER, 1, dBm & 0xFF]));
    const writer = port.writable.getWriter();
    try {
        await writer.write(frame);
    } finally {
        writer.releaseLock();
    }
}

// Read the device's boot serial log for the two facts the browser can't get any other
// way: the device's IP and its mDNS `<deviceName>.local` address. projectMM appends
// machine-parseable `MM_IP=<dotted-quad>` and `MM_DEVICE=<deviceName>.local` tokens to
// its once-per-second tick log over USB (std::printf → reaches the USB-CDC console,
// unlike ESP_LOGI) whenever Ethernet or WiFi-STA has an address — so a board that comes
// up on Ethernet, or boots with saved WiFi credentials, announces its own address (works
// on every OS). The deviceName is the device's single identity (mDNS name = AP name =
// DHCP hostname), so `<deviceName>.local` is exactly what resolves on the LAN. The tokens
// ride the periodic tick line for the device's first 60 s of uptime, so the read below
// is timing-independent — whenever the installer reopens the port within that window (it
// reads at ~3–15 s after reset), the next tick carries them. Afterwards the device's IP
// comes from the REST API, so the tokens stop to keep the perf line clean.
//
// IP + device name only: once the host has the IP it reads everything else (chip,
// firmware, modules, heap, …) from the live device's REST API (`http://<ip>/api/…`),
// which is richer and always current — no reason to scrape more fields from a boot log.
// The device name is the one exception that CAN'T come from REST: on the HTTPS Pages site
// a fetch to the plain-http device is blocked by mixed-content (see canFetchHttp), so the
// `.local` address has to ride the serial log too. Adding any OTHER device info to the
// installer = call the REST API with this IP, not grow this parser.
//
// Returns { ip, mdns } — ip is the dotted-quad ("" on timeout), mdns is the
// "<deviceName>.local" address ("" if the firmware predates MM_DEVICE). A fresh device
// with no credentials never connects, so never prints MM_IP → ip "" → caller falls back
// to Improv provisioning + manual entry. Caller owns opening the port; this acquires/
// releases one reader.
//
// MM_IP and MM_DEVICE are printed on the same tick line, but serial reads chunk
// arbitrarily — MM_IP can land in one read and MM_DEVICE in the next. So we don't return
// the instant MM_IP matches: we return as soon as BOTH are seen, and if only the IP has
// shown by a short grace window after it (older firmware that never sends MM_DEVICE) we
// return IP-only. Both regexes re-run over the growing buffer each read, so a token split
// across chunks is matched once all its bytes have arrived.
async function readBootIp(port, timeoutMs) {
    if (!port || !port.readable) return { ip: "", mdns: "" };
    const decoder = new TextDecoder();
    let buf = "";
    let ip = "", mdns = "";
    const reader = port.readable.getReader();
    const deadline = Date.now() + timeoutMs;
    // Once the IP is seen, keep reading only a short grace window for MM_DEVICE (it may be
    // in the next chunk), not the whole budget — so a device on older firmware that never
    // sends MM_DEVICE still returns its IP promptly. 1500ms covers same-tick-line chunk
    // separation without a noticeable wait.
    const DEVICE_GRACE_MS = 1500;
    let graceDeadline = Infinity;
    try {
        while (Date.now() < Math.min(deadline, graceDeadline)) {
            const remaining = Math.min(deadline, graceDeadline) - Date.now();
            // Race each read against the remaining budget so a silent device
            // (no output) doesn't block past the timeout.
            const timed = new Promise(res => setTimeout(() => res({ timeout: true }), remaining));
            const result = await Promise.race([reader.read(), timed]);
            if (result.timeout || result.done) break;
            buf += decoder.decode(result.value, { stream: true });
            if (!ip) {
                // Primary: the explicit machine token. Fallback: the human-readable
                // "… — Eth/WiFi: <ip>" status line (firmware predating MM_IP).
                const m = buf.match(/MM_IP=(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})/)
                       || buf.match(/(?:Eth|WiFi):\s*(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})/);
                if (m) { ip = m[1]; graceDeadline = Date.now() + DEVICE_GRACE_MS; }
            }
            if (!mdns) {
                const d = buf.match(/MM_DEVICE=(\S+\.local)/);
                if (d) mdns = d[1];
            }
            if (ip && mdns) return { ip, mdns };   // both in → done
            if (buf.length > 8192) buf = buf.slice(-2048);  // cap; keep a tail for split lines
        }
    } catch (_) {
        // Read error (port hiccup) — return whatever we captured (IP-only or nothing).
    } finally {
        try { await reader.cancel(); } catch (_) { /* ignore */ }
        try { reader.releaseLock(); } catch (_) { /* ignore */ }
    }
    return { ip, mdns };
}

// ---------------------------------------------------------------------------
// HTTP fallback for Improv-less paths (alreadyOnline, esp32-eth)
// ---------------------------------------------------------------------------

// User types `192.168.1.42` or `MM-XXXX.local` (or even a full `http://…/`);
// normalize to a trailing-slash http URL that `new URL(path, base)` can resolve
// against. Bare hostnames default to http (the device serves http only).
function normalizeDeviceUrl(input) {
    let s = String(input || "").trim();
    if (!s) return "";
    if (!/^https?:\/\//i.test(s)) s = "http://" + s;
    try {
        const u = new URL(s);
        // Drop any path/query the user pasted — they're typing an address,
        // not a deep link. Trailing slash is what new URL("api/control", u)
        // wants for clean resolution.
        u.pathname = "/";
        u.search = "";
        u.hash = "";
        return u.toString();
    } catch (_) {
        return "";
    }
}

// Mixed-content guard: the installer page can only fetch() a plain http://
// device URL when the page itself is on http:// (i.e. localhost preview).
// On https://moonmodules.org the browser blocks the fetch silently — caller
// must fall through to the query-param handoff via the Visit button. file:
// pages count as http for this purpose.
function canFetchHttp(deviceUrl) {
    if (!deviceUrl) return false;
    let target;
    try { target = new URL(deviceUrl); } catch (_) { return false; }
    if (target.protocol !== "http:") return true;  // https device → no block
    return window.location.protocol !== "https:";
}

// Fan out every `controls.<Module>.<control>` field for `board` from the
// same-origin `./boards.json` into the device's `/api/control`. Mirrors
// what the device UI's `consumePendingBoardParam()` does for the Inject-
// button path — keeps preview-mode parity with production. Returns true
// if every POST returned 2xx, false otherwise (any failure short-circuits
// further pushes so a half-applied state is visible in the logs).
// 5s per-request timeout — generous for the bench; a typo'd IP fails fast.
async function tryHttpInjectBoard(deviceUrl, board) {
    let entry;
    try {
        const res = await fetch("./boards.json", { signal: AbortSignal.timeout(5000) });
        if (!res.ok) return false;
        const catalog = await res.json();
        entry = Array.isArray(catalog)
            ? catalog.find(b => b && b.name === board)
            : null;
    } catch (_) {
        return false;
    }
    if (!entry) return false;
    // Each entry is a list of module-with-controls units:
    //   { type, id, parent_id?, controls? }
    // Per module: add it first (when it has a parent_id — a fresh flash has no
    // user-added modules like AudioModule, so a control write would 404), then set
    // its controls. A module without parent_id is boot-wired/top-level (Board under
    // System, Network) that already exists — skip the add, just set controls. The
    // add is idempotent (an existing id returns 200). This is the install flow, so
    // any failure aborts the inject (all-or-nothing).
    const modules = Array.isArray(entry.modules) ? entry.modules : [];
    for (const m of modules) {
        if (!m || typeof m !== "object") continue;
        // id keys both the module add and every control write below; a unit
        // without one is malformed catalog data — skip it rather than POST a
        // body the device can't route.
        if (typeof m.id !== "string" || m.id === "") continue;
        if (m.parent_id && m.type) {
            try {
                const res = await fetch(new URL("api/modules", deviceUrl), {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({ type: m.type, id: m.id, parent_id: m.parent_id }),
                    signal: AbortSignal.timeout(5000),
                });
                if (!res.ok) return false;
            } catch (_) {
                return false;
            }
        }
        const controls = m.controls;
        if (!controls || typeof controls !== "object") continue;
        for (const [controlName, value] of Object.entries(controls)) {
            try {
                const res = await fetch(new URL("api/control", deviceUrl), {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({
                        module: m.id,
                        control: controlName,
                        value,
                    }),
                    signal: AbortSignal.timeout(5000),
                });
                if (!res.ok) return false;
            } catch (_) {
                return false;
            }
        }
    }
    return true;
}

// Inline of esptool-js@0.4.7's ESPLoader.main, with attempts=2 instead of
// the default 7. The default takes ~60 s to fail when the user picks a
// non-ESP32 serial port (e.g. Bluetooth-Incoming-Port on macOS — opens
// fine, just doesn't speak the ESP32 ROM bootloader protocol); 2 attempts
// brings that to ~10 s while still allowing one retry for legitimately-
// flaky USB-Serial bridges (the LOLIN D32's CH340 sometimes needs a
// second go on the RTS/DTR pulse). The rest of the body is reproduced
// verbatim from esptool-js's ESPLoader.main — when the SDK pin bumps,
// re-verify the sequence still matches upstream.
// esptool's getChipDescription() reports the specific silicon (e.g.
// "ESP32-D0WD-V3", "ESP32-S3 (QFN56) (revision v0.2)", "ESP32-C6 (revision v0.0)"),
// but the picker compares against boards.json's coarse `chip` FAMILY — the same
// vocabulary build_esp32's TARGET_TO_FAMILY defines and the ESP Web Tools manifest
// carries as `chipFamily` ("ESP32", "ESP32-S3", "ESP32-P4", and any future
// S2/C3/C6/… as projectMM grows to support every ESP32-family chip). Without
// normalising, a classic ESP32 matches NO board
// (filter) and the flash guard false-warns on a correct flash.
//
// Normalise by KEEPING the family token and dropping the package/revision tail —
// not by collapsing distinct chips. A bare "ESP32-<X>…" keeps "ESP32-<X>"; classic
// silicon (ESP32-D0WD*, ESP32-PICO-*, ESP32-U4WDH — no second family token) maps to
// plain "ESP32". This is forward-proof: a new ESP32-C5 needs no code change here.
//
// The ESP32 token is matched ANYWHERE in the string, not anchored at the start:
// esptool-js prefixes "unknown " when it doesn't fully recognise a chip (newer
// silicon than the bundled esptool — observed: "unknown ESP32-P4 (revision v1.3)"),
// and anchoring on ^ESP32 would let that prefix defeat the match and pass the raw
// string through (→ no board matches, false "flash anyway?" warning).
function chipFamily(chipName) {
    const s = String(chipName || "").trim();
    // ESP32-<LETTER+DIGITS> is a sub-family (S3/P4/S2/C3/C6/C5/H2/…).
    const sub = s.match(/ESP32-([A-Z]\d+)\b/);
    if (sub) return "ESP32-" + sub[1];
    if (/ESP32\b/.test(s)) return "ESP32";   // classic — no sub-family token
    return s;                                // truly unrecognised — pass through so it's visible
}

async function connectAndDescribeChip(esploader) {
    await esploader.connect("default_reset", 2);
    const chipName = await esploader.chip.getChipDescription(esploader);
    esploader.info("Chip is " + chipName);
    esploader.info("Features: " + (await esploader.chip.getChipFeatures(esploader)));
    esploader.info("Crystal is " + (await esploader.chip.getCrystalFreq(esploader)) + "MHz");
    esploader.info("MAC: " + (await esploader.chip.readMac(esploader)));
    if (typeof esploader.chip.postConnect !== "undefined") {
        await esploader.chip.postConnect(esploader);
    }
    await esploader.runStub();
    if (esploader.romBaudrate !== esploader.baudrate) {
        await esploader.changeBaud();
    }
    return chipName;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Set by detect(); consumed by start(). navigator.serial allows ONE open handle
// per port, and connectAndDescribeChip leaves the port open at 460800 with the
// stub loaded — so detect() keeps that connection and start() reuses it instead
// of re-opening (which would double-open and fail). Cleared once start() takes
// it, on any detect() failure, or via clearDetected() when the user re-picks a
// port. { port, transport, esploader, chipName } | null.
let _detected = null;

// Tear down whatever detect() left open and release the port back to the OS.
async function releaseDetected() {
    if (!_detected) return;
    const { transport, port } = _detected;
    _detected = null;
    try { await transport.disconnect(); } catch (_) { /* already gone */ }
    try { await port.close(); } catch (_) { /* already closed */ }
}

export const installer = {
    /**
     * Drive the full install flow: request port, flash via esptool-js,
     * provision WiFi via Improv, push SET_BOARD if a board was picked,
     * report success with the device URL.
     *
     * @param {object} opts
     * @param {string} opts.manifestUrl - URL to an ESP Web Tools manifest
     * @param {string} [opts.board] - board name from boards.json to push
     *   via SET_BOARD after provisioning. Omit / empty for "(any board)".
     * @param {number|null} [opts.txPower] - boards.json
     *   controls.Network.txPowerSetting for the picked board (whole dBm).
     *   When set, the SET_TX_POWER vendor RPC is pushed BEFORE provisioning
     *   so brown-out-prone boards associate at the capped power. Omit /
     *   null when the board has no cap.
     * @param {boolean} [opts.eraseBefore=false] - when true, eraseFlash()
     *   before writeFlash. Wipes the entire chip including LittleFS (saved
     *   WiFi credentials, board name). Adds ~12 s. Default false because
     *   a normal re-flash overwrites in place and users usually want
     *   persistent state to survive a firmware bump.
     * @param {(stage: string, detail?: object) => void} opts.onProgress
     *   Stages: request-port, connect-flash, fetch-firmware, erase,
     *   flash, reboot, connect-improv, set-tx-power, wifi-creds-form,
     *   provisioning, set-board, done. flash also carries { pct }. connect-flash carries
     *   { chipName } once detection succeeds.
     * @param {() => Promise<{ssid: string, password: string}>} opts.uiWaitForCreds
     *   Host page resolves this when the user fills in the WiFi form.
     * @param {() => Promise<{action: "ip"|"skip"|"retry", url?: string}>} [opts.uiWaitForIp]
     *   Host page resolves this when the user picks an action on the fallback
     *   "needs-ip" form. Only invoked when Improv didn't produce a URL
     *   (alreadyOnline branch, or an Improv-less firmware like esp32-eth).
     *   Actions:
     *     - "ip"    — `url` is the typed value; orchestrator continues with
     *                 HTTP-injection path.
     *     - "skip"  — host shows the legacy "find on network" message; device
     *                 is not added.
     *     - "retry" — re-run Improv `initialize()` on a fresh client; on
     *                 success continue to the wifi-creds form, on failure
     *                 re-prompt. Cheap second chance for slow-booting boards
     *                 (LOLIN S3 mini etc.) that lost the post-flash race.
     * @param {(retrying: boolean) => void} [opts.uiShowNeedsIpRetrying]
     *   Host page toggles the dialog into "retry in flight" mode (inputs +
     *   buttons disabled, spinner visible). Called with `true` before each
     *   retry attempt, then with `false` if the retry came back as a
     *   transient failure (so the next prompt is interactable). Retry
     *   success doesn't call back — the host's next render takes over
     *   (the wifi-creds form section replaces the needs-ip section).
     *   Optional; degrade gracefully when omitted (older host pages still
     *   get retry behaviour, just without the spinner).
     * @param {() => Promise<void>} [opts.uiWaitForPortRetry]
     *   Host resolves this when the user clicks the Try-again button in
     *   the wrong-port section. Fires after the orchestrator's probe-open
     *   on the user's pre-picked port fails (wrong device picked from the
     *   OS list, or stale unplugged-and-replugged handle). Gating the OS-
     *   picker re-prompt behind this click lets the user actually see the
     *   guidance message — the OS picker is modal and covers the install
     *   modal. Optional; degrade gracefully to a silent re-prompt when
     *   omitted (older host pages just lose the guidance section).
     * @param {(detail: {url: string, mdns?: string, board: string, viaHttp?: boolean, httpBoardOk?: boolean, alreadyOnline?: boolean}) => void} opts.onSuccess
     *   `mdns` is the device's `<deviceName>.local` address from the boot serial
     *   (deviceName is the single identity — mDNS = AP = DHCP hostname all follow
     *   it), or "" if the firmware predates the MM_DEVICE token. The host shows it
     *   as the clickable success link when present (survives a DHCP IP change),
     *   falling back to the IP `url` otherwise.
     *   `alreadyOnline:true` is set when the device booted into
     *   STATE_PROVISIONED (saved WiFi from a previous flash) — the SDK's
     *   initialize() fails and the orchestrator falls through to the
     *   needs-ip prompt. `viaHttp:true` is set whenever the URL came from
     *   the user-typed IP form (alreadyOnline OR Improv-less firmware).
     *   `httpBoardOk:true` is set when an HTTP `/api/control` push of the
     *   picked board succeeded inside the orchestrator — only possible in
     *   localhost preview, since HTTPS Pages can't fetch the device's HTTP
     *   URL (mixed-content). Host uses `httpBoardOk` to skip the pending-
     *   board query-param handoff in `addProvisionedDevice`.
     * @param {(stage: string, error: Error) => void} opts.onError
     * @param {(line: string) => void} [opts.onLog] - optional: each line
     *   esptool-js writes to its "terminal" gets forwarded here. Host
     *   appends them to a collapsible <pre> in the modal so the user can
     *   see what happened on a failure.
     * @param {SerialPort} [opts.port] - optional pre-picked SerialPort.
     *   When provided the orchestrator skips its own requestPort() prompt
     *   and uses this handle directly. Falls back to requestPort() when
     *   omitted/null OR when opening this handle fails (stale grant after
     *   the device was unplugged and replugged).
     */
    async start({ manifestUrl, board, txPower = null, eraseBefore = false, port: prePickedPort,
                   onProgress, uiWaitForCreds, uiWaitForIp, uiShowNeedsIpRetrying,
                   uiWaitForPortRetry,
                   onSuccess, onError, onLog }) {
        if (!navigator.serial) {
            onError("setup", new Error("Web Serial API not available — use Chrome, Edge, or Opera"));
            return;
        }

        let port;
        let transport;
        let esploader;
        let improvClient;
        // Track which stage we last advertised so the catch reports the
        // failing stage rather than a generic "flow". User-visible diagnostic.
        let lastStage = "setup";
        const trackProgress = (stage, detail) => { lastStage = stage; onProgress(stage, detail); };

        // esptool-js's terminal interface — see IEspLoaderTerminal in
        // esptool-js/lib/esploader.d.ts. Forwards every line into onLog so
        // the modal's collapsible log panel can show it. clean() resets the
        // panel between writeFlash phases; we just forward as a marker line.
        const espTerminal = onLog ? {
            clean: () => {},
            writeLine: (s) => onLog(s),
            // Filter single-character writes — esptool's _connectAttempt
            // emits "." per sync packet and "_" per attempt boundary,
            // which fills the log with noise that looks bogus to a user
            // (one column of dots, then underscores, no context). The
            // writeLine path still carries the actual progress lines
            // ("esptool.js", "Serial port", "Connecting...", chip-info,
            // erase / write progress, baud-rate switch).
            write: (s) => { if (s && s.length > 1) onLog(s); },
        } : undefined;
        // ImprovSerial's logger arg is a console-shape object. We need
        // every method (.log, .info, .warn, .error, .debug, .trace, …) to
        // be defined — the SDK may call any of them, and an undefined-method
        // call would throw inside its await chain and look like a timeout
        // (this exact regression hit during testing when the orchestrator
        // passed a hand-rolled object). A Proxy forwarding everything to
        // console + teeing log/info/debug to onLog gives us both: the
        // SDK never sees a missing method, AND its chatter reaches the
        // modal's log panel.
        const improvLogger = new Proxy(console, {
            get(target, prop) {
                const fn = target[prop];
                if (typeof fn !== "function") return fn;
                if (onLog && (prop === "log" || prop === "info" || prop === "debug")) {
                    return (...args) => {
                        fn.apply(target, args);
                        try { onLog("[improv] " + args.map(String).join(" ")); }
                        catch (_) { /* don't let log errors break the SDK */ }
                    };
                }
                return fn.bind(target);
            },
        });

        try {
            trackProgress("request-port");
            // Use the pre-picked port when the host page provided one (the
            // "USB Port" dropdown on the install page). Otherwise surface
            // the native picker here. requestPort is user-gesture
            // protected; we can call it because Install click is itself
            // a user gesture (chained through onInstall → installer.start).
            //
            // Probe the pre-picked handle by opening + closing it before
            // handing to esptool-js. Two kinds of failure get caught here:
            //   - Wrong port: user picked a non-ESP32 entry from the OS
            //     list (Bluetooth-Incoming-Port, LG Monitor Controls,
            //     debug consoles — Web Serial's requestPort doesn't
            //     filter, so the user can grant any serial-shaped device).
            //     open() throws because nothing is listening or the OS
            //     reserves the device.
            //   - Stale handle: device was unplugged + replugged since
            //     the user picked it; the SerialPort object survives but
            //     the underlying USB endpoint is gone.
            // On either failure we surface wrong-port-retry to the host
            // (so the modal renders specific guidance) and re-prompt with
            // `requestPort()`. The browser's existing permission grant
            // means the picker pops without an authorization dialog;
            // the user picks a different port (wrong-port case) or the
            // same port now replugged (stale case).
            //
            // Reuse a detect() connection when present: the port is already
            // open at 460800 with the stub loaded and the chip described, so we
            // skip the probe-open + Transport + connect entirely and jump
            // straight to fetch/flash. Consumed here — start() now owns teardown
            // (the finally block disconnects transport + closes port as usual).
            if (_detected) {
                ({ port, transport, esploader } = _detected);
                const chipName = _detected.chipName;
                _detected = null;
                trackProgress("connect-flash", { chipName });
            } else {
            let probeFailed = false;
            if (prePickedPort) {
                try {
                    await prePickedPort.open({ baudRate: 115200 });
                    // Reset RTS/DTR to the inactive (high) state esptool-js
                    // expects on a fresh port. Without this, the probe-
                    // close transition can leave the bridge chip's
                    // control lines in an indeterminate state, and
                    // esptool's subsequent bootloader-entry RTS/DTR
                    // pulse fails to trigger a reset — symptom: "Detecting
                    // chip…" hangs ~60 s then "Failed to connect with the
                    // device" on a port that's actually a healthy ESP32.
                    try {
                        await prePickedPort.setSignals({
                            requestToSend: true,
                            dataTerminalReady: true,
                        });
                    } catch (_) { /* not all platforms expose setSignals; harmless to skip */ }
                    await prePickedPort.close();
                    port = prePickedPort;
                } catch (e) {
                    if (onLog) onLog(`[orchestrator] picked port doesn't open (${e.message || e}); re-prompting — pick a different one`);
                    probeFailed = true;
                }
            }
            if (!port) {
                // Probe-failed path: the OS port picker is modal and
                // covers the install modal, so any guidance set in
                // #connecting-detail and immediately followed by
                // requestPort() is invisible. Gate the re-prompt
                // behind a host-rendered "Try again" section so the
                // user sees the wrong-port guidance BEFORE the OS
                // picker covers the page. Host omits the callback on
                // older versions — degrade to the silent re-prompt.
                if (probeFailed) {
                    trackProgress("wrong-port-retry");
                    if (uiWaitForPortRetry) await uiWaitForPortRetry();
                    trackProgress("request-port");
                }
                port = await navigator.serial.requestPort({});
            }

            trackProgress("connect-flash");
            transport = new Transport(port, false);
            // 460800 baud is ESP Web Tools' default — a well-tested compromise
            // that works across CH340 / CP2102 / FT232 USB-serial chips and
            // cheap cables. 921600 is faster but caused FLASH_DEFL_BEGIN
            // timeouts on the LOLIN D32's CH340. Bootloader sync runs at
            // 115200 first (romBaudrate); esptool-js negotiates up after.
            esploader = new ESPLoader({
                transport,
                baudrate: 460800,
                romBaudrate: 115200,
                terminal: espTerminal,
            });
            const chipName = await connectAndDescribeChip(esploader);
            trackProgress("connect-flash", { chipName });
            } // end of detect()-reuse else branch

            // Fetch the manifest + all part binaries before erasing flash.
            // Failing here (CDN issue, bad manifest) leaves the device
            // intact — better than mid-flash failure with an erased chip.
            trackProgress("fetch-firmware");
            const manifest = await fetchManifest(manifestUrl);
            const fileArray = [];
            for (const part of manifest.parts) {
                const res = await fetch(part.url);
                if (!res.ok) {
                    throw new Error(`part fetch HTTP ${res.status}: ${part.url}`);
                }
                const buf = await res.arrayBuffer();
                // esptool-js 0.4.7's writeFlash expects each part's `data`
                // as a binary string (one char per byte) — it iterates with
                // .charCodeAt() inside. Uint8Array fails with "charCodeAt is
                // not a function". Convert in 16 KB chunks to avoid blowing
                // the call-stack on a 1.2 MB app image.
                fileArray.push({
                    data: bufferToBinaryString(buf),
                    address: part.offset,
                });
            }

            // Optional standalone eraseFlash() — opt-in via the "Erase chip
            // first" checkbox on the install page. Default off because a
            // normal install overwrites the regions writeFlash touches and
            // preserves user state (WiFi credentials, board name on
            // LittleFS), which is what most users want. Tick the box when
            // switching firmware variants whose partition tables differ, or
            // when wiping the device for a clean install. ~12 s on a 4 MB
            // chip; status text covers it so the user doesn't think it
            // hung.
            if (eraseBefore) {
                trackProgress("erase");
                await esploader.eraseFlash();
            }

            trackProgress("flash", { pct: 0 });
            await esploader.writeFlash({
                fileArray,
                flashMode: "keep",
                flashFreq: "keep",
                flashSize: "keep",
                compress: true,
                reportProgress: (idx, written, total) => {
                    const pct = total > 0 ? Math.round(100 * written / total) : 0;
                    // Don't bump lastStage on every progress tick — keep it as
                    // "flash" set just above; intermediate ticks are detail only.
                    onProgress("flash", { pct, fileIdx: idx });
                },
            });

            trackProgress("reboot");
            // hard_reset toggles DTR/RTS to reset the chip after flash.
            await esploader.hardReset();

            // Hand the port from esptool-js to ImprovSerial. The device
            // hard-resets, the USB CDC endpoint re-enumerates, and the
            // existing SerialPort can go stale ("Port is not readable" on
            // first read). Fix: close + reopen the same handle so the OS
            // re-establishes the connection. This works on most chips
            // (CP2102, FT232) and survives the re-enumeration on CH340 too,
            // because Web Serial caches the user's prior port grant; we
            // don't need to call requestPort again. ESP Web Tools' "close
            // the modal, reopen, re-pick the port" workaround is just an
            // uglier version of the same idea — they don't have continuous
            // control of the SerialPort so they can't do it transparently.
            await transport.disconnect();
            transport = null;
            esploader = null;
            try { await port.close(); } catch (_) { /* may already be closed */ }

            // Wait for the device's bootloader → app handoff + USB re-enum.
            // Without this, port.open() raises before the kernel re-attaches
            // the new endpoint. 3 s rather than 2 s because the ESP32-S3
            // native-USB path reaches "app ready, Improv task installed"
            // at ~1.85 s after reset (boot log measurement on an
            // ESP32-S3-DevKitC clone), and the original 2 s window left no margin for host-
            // side USB re-enum (extra ~100-300 ms on macOS). The retry
            // button on the needs-ip dialog still catches the long tail —
            // this just makes the common case land without a retry click.
            await new Promise(r => setTimeout(r, 3000));

            trackProgress("connect-improv");
            // Reopen the same SerialPort handle for the Improv connection.
            // 115200 is the standard Improv-Serial baudrate; the device's
            // ImprovProvisioningModule listens on UART0 at this rate (see
            // src/platform/esp32/platform_esp32_improv.cpp).
            //
            // Some USB-serial chips (rare CH340 silicon revisions, mis-driven
            // adapters) don't survive the close+reopen cleanly — the OS handle
            // ends up stale and port.open() throws. Catch that and prompt for
            // a fresh requestPort(); the browser's permission grant from the
            // earlier requestPort means it surfaces a picker but no auth
            // dialog. User picks the same physical port; we get a fresh
            // SerialPort handle. Slightly worse UX (extra click) than the
            // transparent reopen, but never silently fails.
            try {
                await port.open({ baudRate: 115200 });
            } catch (openErr) {
                if (onLog) onLog(`[orchestrator] port.open() failed (${openErr.message}); falling back to requestPort()`);
                port = await navigator.serial.requestPort({});
                await port.open({ baudRate: 115200 });
            }
            // Read the boot log first: a device that comes up on Ethernet, or boots
            // with saved WiFi credentials, prints its IP (MM_IP=…) and is ALREADY
            // ONLINE — no Improv provisioning needed. Detecting it here turns the old
            // "device didn't respond over USB, type its IP" chore into an automatic
            // success with the IP pre-found. A fresh device with no credentials never
            // connects, so readBootIp times out → "" and we fall through to the normal
            // TX-power + Improv provisioning flow below.
            //
            // The device re-emits MM_IP= every second on its tick log (it rides the
            // periodic tick line in main.cpp) for its first 60 s of uptime, so this read
            // is timing-independent: it just has to overlap any one tick in that window.
            // 12 s budget (port open from ~3 s to ~15 s after reset) sits comfortably
            // inside the 60 s and covers DHCP variance (measured ~7 s on the P4-NANO)
            // without making the fresh-device fallback feel slow.
            trackProgress("connect-improv");
            const { ip: bootIp, mdns: bootMdns } = await readBootIp(port, 12000);

            let deviceUrl = "";
            let deviceMdns = "";   // "<deviceName>.local" from the boot log, "" if unknown
            let viaHttp = false;
            let needsIp = false;
            let alreadyOnline = false;

            if (bootIp) {
                if (onLog) onLog(`[orchestrator] device already online at ${bootIp}${bootMdns ? ` (${bootMdns})` : ""} (from boot serial) — skipping Improv`);
                deviceUrl = `http://${bootIp}/`;
                deviceMdns = bootMdns;
                viaHttp = true;
                alreadyOnline = true;
            } else {
            // Pre-association TX-power cap (weak-power brown-out fix): push it
            // while we still own the port, before ImprovSerial locks it.
            // The device applies + persists it within a second — long
            // before the user finishes the WiFi form below.
            if (txPower != null) {
                trackProgress("set-tx-power");
                if (onLog) onLog(`[orchestrator] SET_TX_POWER ${txPower} dBm (boards.json cap)`);
                await sendSetTxPowerFrame(port, txPower);
                await new Promise(r => setTimeout(r, 200));
            }
            improvClient = new ImprovSerial(port, improvLogger);
            // ImprovSerial's initialize() throws "Improv Wi-Fi Serial not
            // detected" in two distinct cases that look identical to the SDK
            // but mean different things to us:
            //   1. Device booted with saved WiFi credentials — it's at
            //      STATE_PROVISIONED already; SDK expects STATE_AUTHORIZED.
            //   2. Firmware doesn't include the Improv listener at all
            //      (esp32-eth and other Improv-less builds — no UART RPC
            //      task is running).
            // Both share the same fallback path: skip provisioning + RPC
            // push, prompt the user for the device IP/hostname, and let
            // the post-flash flow (HTTP push in preview, query-param hand-
            // off via Visit in production) handle the board injection.
            // Try Improv `initialize()` once. On the "not detected" error
            // we drop into a retry loop: show the needs-ip dialog, let the
            // user choose retry / typed-IP / skip. Retry tears the SDK
            // down, re-opens the port, and runs `initialize()` again on
            // a fresh ImprovSerial. Cheap second chance for slow-booting
            // boards (LOLIN S3 mini etc.) that lose the post-flash race —
            // the device-side Improv task isn't installed until after
            // NetworkModule::setup() completes (~1.8 s on ESP32-S3), and
            // the host's 2 s reopen wait can land before it's ready.
            const isImprovNotDetected = (e) => {
                // Tied to the pinned `improv-wifi-serial-sdk` version at
                // the top of this file — if you bump the SDK, re-verify
                // the exact text of its "not detected" path (the SDK
                // doesn't currently expose a typed error or error code we
                // could match on instead).
                const msg = (e && e.message) ? String(e.message) : String(e);
                return msg.includes("Improv Wi-Fi Serial not detected");
            };
            // Errors we treat as retryable on the needs-ip dialog —
            // device-side timing race, host-side port-lock race, or
            // SDK reader-lock not-yet-released. All correspond to "try
            // again in a moment" rather than "something is structurally
            // broken." Centralised so future SDK bumps have one site to
            // update; otherwise these strings drift across the file.
            const isTransientImprovError = (e) => {
                if (isImprovNotDetected(e)) return true;
                const msg = (e && e.message) ? String(e.message) : String(e);
                return msg.includes("Port is not ready")
                    || msg.includes("PortNotReady")
                    || msg.includes("locked");
            };
            try {
                await improvClient.initialize();
            } catch (e) {
                if (isImprovNotDetected(e)) {
                    needsIp = true;
                    alreadyOnline = true;  // best-guess label for the modal copy
                } else {
                    throw e;
                }
            }
            } // end of `else` (device was not already online from the boot log)

            // Retry loop. Entered when the first initialize() failed and
            // exited either:
            //   - user clicks Retry and a fresh initialize() succeeds
            //     → needsIp flips back to false, fall through to the
            //       normal wifi-creds-form path below.
            //   - user types an IP + Add → break with viaHttp=true.
            //   - user clicks Skip → early return.
            while (needsIp) {
                // Drop the current SDK before showing the dialog so a Retry
                // click can rebuild a fresh client. Same teardown the
                // original code did unconditionally — now also runs at the
                // top of each retry iteration.
                try { if (improvClient) await improvClient.close(); } catch (_) { /* ignore */ }
                improvClient = null;
                if (!uiWaitForIp) {
                    // uiWaitForIp may be omitted on older host pages —
                    // degrade gracefully to the legacy empty-URL exit
                    // (no retry button without the dialog to host it).
                    trackProgress("done");
                    onSuccess({ url: "", board: "", alreadyOnline: true });
                    return;
                }
                trackProgress("needs-ip");
                const ipResult = await uiWaitForIp();
                if (!ipResult || ipResult.action === "skip") {
                    // User skipped the IP prompt; mirror the old behaviour.
                    trackProgress("done");
                    onSuccess({ url: "", board: "", alreadyOnline });
                    return;
                }
                if (ipResult.action === "ip") {
                    // normalizeDeviceUrl returns "" on whitespace-only or
                    // unparseable input. The `required` attribute on the
                    // input element catches empty submits, but a trim
                    // here means a value like "   " still escapes that
                    // guard — treat the empty result as "user didn't
                    // give us a usable address" and stay in the loop
                    // so they can try again rather than fall through
                    // to a downstream fetch that would fail differently.
                    deviceUrl = normalizeDeviceUrl(ipResult.url || "");
                    if (deviceUrl) {
                        viaHttp = true;
                        break;
                    }
                    if (onLog) onLog(`[orchestrator] empty / unparseable IP — re-prompting`);
                    continue;
                }
                // action === "retry": rebuild the SDK on the same port.
                // The port stays open across improvClient.close() (close
                // only releases the SDK's reader); the 250 ms sleep lets
                // the SDK's disconnect event fire and the reader-lock on
                // port.readable release before we acquire a new reader.
                // Empirically tuned — browsers sometimes need a microtask
                // flush past cancel-return before getReader() sees the
                // unlocked state.
                if (uiShowNeedsIpRetrying) uiShowNeedsIpRetrying(true);
                await new Promise(r => setTimeout(r, 250));
                // Re-push the TX-power cap on every retry: a slow-booting
                // board may have missed the first frame entirely.
                if (txPower != null) {
                    try { await sendSetTxPowerFrame(port, txPower); } catch (_) { /* best-effort */ }
                    await new Promise(r => setTimeout(r, 200));
                }
                try {
                    improvClient = new ImprovSerial(port, improvLogger);
                    await improvClient.initialize();
                    // Retry succeeded — exit the loop and continue into
                    // the normal wifi-creds-form path. uiShowNeedsIpRetrying
                    // is left in `true` state; the host page's next render
                    // (the WiFi creds form) replaces the dialog content.
                    needsIp = false;
                } catch (e) {
                    // Transient → re-show the dialog so the user can try
                    // again. Anything else is genuinely broken and bubbles.
                    const msg = (e && e.message) ? String(e.message) : String(e);
                    if (isTransientImprovError(e)) {
                        if (onLog) onLog(`[orchestrator] retry failed (${msg}); re-prompting`);
                        if (uiShowNeedsIpRetrying) uiShowNeedsIpRetrying(false);
                    } else {
                        throw e;
                    }
                }
            }

            if (!needsIp && !viaHttp) {
                trackProgress("wifi-creds-form");
                const { ssid, password } = await uiWaitForCreds();

                // Skip path: user clicked Skip in the creds form (or
                // submitted an empty SSID). Don't call provision() with
                // empty creds — the device-side handler rejects empty
                // SSIDs as ErrorState, which would surface as a misleading
                // "provisioning failed" error. Instead exit cleanly with
                // no deviceUrl; the host's onSuccess handler treats an
                // empty url as "user opted into AP fallback, walk them
                // through joining MM-XXXX manually" (see Step 2 in
                // docs/install/index.html and the closeModal path in
                // handleSuccess).
                //
                // Note on the two "skip" shapes: `uiWaitForIp()` returns
                // a tagged union ({action: "skip"|"ip"|"retry"}) because
                // the needs-ip dialog has three distinct outcomes the
                // orchestrator dispatches on. `onSuccess({url:""})` is a
                // sentinel because the outbound result has only two
                // outcomes ("we got a URL" vs "we didn't"). Different
                // shapes for different cardinalities, not drift.
                if (!ssid) {
                    trackProgress("done");
                    onSuccess({ url: "", board: "", alreadyOnline: false });
                    return;
                }

                trackProgress("provisioning");
                // provision() throws on failure (timeout, bad creds). 30s
                // timeout matches the device-side wait at
                // platform_esp32_improv.cpp::improvHandleProvision.
                await improvClient.provision(ssid, password, 30000);
                deviceUrl = improvClient.nextUrl;
                if (!deviceUrl) {
                    throw new Error("provision succeeded but no device URL returned");
                }

                // Push SET_BOARD vendor RPC if the user picked a board.
                // ImprovSerial holds the writable lock — close it first so
                // we can write our own raw frame. We're done with
                // ImprovSerial anyway (provision is the last standard
                // command we need).
                if (board) {
                    trackProgress("set-board");
                    await improvClient.close();
                    improvClient = null;
                    await sendSetBoardFrame(port, board);
                    // The device-side handler responds with RpcResponse,
                    // but we don't wait for it — failures (validation
                    // rejection, etc.) are reported via ErrorState which
                    // we'd need to re-open a reader to see. Best-effort:
                    // device persists in next 2s via FilesystemModule's
                    // debounced save. If push silently failed, the field
                    // stays empty and the user can pick the board via
                    // MoonDeck later.
                    // Small grace period so the device's UART task
                    // finishes processing before we close the port.
                    await new Promise(r => setTimeout(r, 200));
                }
            }

            // HTTP injection attempt — fans out the boards.json `controls.*`
            // entries to the device's `/api/control`, mirroring what the
            // device UI's `consumePendingBoardParam` does for the Inject-
            // button path. Runs for BOTH paths:
            //   - needsIp (typed IP): the only board-injection path, since
            //     SET_BOARD over serial wasn't possible.
            //   - Improv-success: SET_BOARD already pushed `Board.board` over
            //     serial, but every OTHER field in `controls.*` (e.g.
            //     `Network.txPowerSetting` for the weak-power WiFi cap) needs
            //     this fan-out to reach the device. Without it the board
            //     identifier lands but the per-board tweaks don't.
            // Gated by `canFetchHttp(deviceUrl)` — on HTTPS Pages the
            // browser blocks fetches to http:// device URLs (mixed-content);
            // those users get the controls via the `?board=` query-param
            // handoff after clicking Visit. Successful HTTP push tells the
            // host page to skip the pending-board handoff via `httpBoardOk`.
            let httpBoardOk = false;
            if (board && canFetchHttp(deviceUrl)) {
                if (onLog) onLog(`[orchestrator] attempting HTTP inject for board="${board}" to ${deviceUrl}`);
                httpBoardOk = await tryHttpInjectBoard(deviceUrl, board);
                if (onLog) onLog(`[orchestrator] HTTP inject ${httpBoardOk ? "succeeded" : "failed"}`);
            } else if (board && onLog) {
                // Skipped — most commonly HTTPS Pages → http:// device URL
                // blocked by mixed-content. The `?board=` handoff via the
                // Visit button picks up the controls fan-out same-origin.
                onLog(`[orchestrator] HTTP inject skipped (cross-origin / mixed-content); relying on ?board= handoff`);
            }

            trackProgress("done");
            onSuccess({
                url: deviceUrl,
                mdns: deviceMdns,   // "<deviceName>.local" from the boot serial, "" if unknown
                board: board || "",
                viaHttp,
                httpBoardOk,
                alreadyOnline,
            });
        } catch (e) {
            // Best-effort cleanup on error so subsequent attempts can
            // re-request the same port without it being stuck open.
            onError(lastStage, e);
        } finally {
            try {
                if (improvClient) await improvClient.close();
            } catch (_) { /* ignore */ }
            try {
                if (esploader && transport) await transport.disconnect();
            } catch (_) { /* ignore */ }
            try {
                if (port) await port.close();
            } catch (_) { /* ignore */ }
        }
    },

    /**
     * Detect the connected chip family WITHOUT flashing. Opens the serial port,
     * reads the chip via the same connectAndDescribeChip path start() uses, and
     * KEEPS the connection open — stored in _detected so a following start()
     * reuses it (no double-open of the single Web Serial handle, no second OS
     * picker, no repeated bootloader-entry dance). Returns the chip-family
     * string ("ESP32" / "ESP32-S3" / ...). Web installer only — the on-device
     * OTA UI has no local serial and never wires this.
     *
     * @param {object} opts
     * @param {SerialPort} [opts.port] - host's pre-picked port; if omitted,
     *   detect() prompts requestPort() itself.
     * @param {(line: string) => void} [opts.onLog]
     * @returns {Promise<string>} the chip-family description
     */
    async detect({ port: prePickedPort, onLog } = {}) {
        if (!navigator.serial) {
            throw new Error("Web Serial API not available — use Chrome, Edge, or Opera");
        }
        // Drop any prior detect that start() never consumed, so we don't leak
        // an open handle when the user clicks Detect twice.
        await releaseDetected();

        const port = prePickedPort || await navigator.serial.requestPort({});
        // Trimmed espTerminal (same shape as start()'s) so chip-info lines reach
        // the log panel; the single-char filter drops esptool's "." / "_" noise.
        const espTerminal = onLog ? {
            clean: () => {},
            writeLine: (s) => onLog(s),
            write: (s) => { if (s && s.length > 1) onLog(s); },
        } : undefined;
        const transport = new Transport(port, false);
        const esploader = new ESPLoader({
            transport,
            baudrate: 460800,
            romBaudrate: 115200,
            terminal: espTerminal,
        });
        try {
            const chipName = await connectAndDescribeChip(esploader);
            // Keep the raw silicon name in _detected for the flash-progress log
            // (more informative there); hand the picker the FAMILY, which is what
            // it compares against boards.json's `chip`.
            _detected = { port, transport, esploader, chipName };
            return chipFamily(chipName);
        } catch (e) {
            // Failed before we could stash it — release the port so the next
            // attempt (or a manual flow) can re-open cleanly.
            try { await transport.disconnect(); } catch (_) { /* ignore */ }
            try { await port.close(); } catch (_) { /* ignore */ }
            throw e;
        }
    },

    /**
     * Release a pending detect() connection (e.g. the user re-picked the port,
     * so the detected handle is now stale). Safe to call when nothing is pending.
     */
    async clearDetected() {
        await releaseDetected();
    },

    /**
     * Erase-only flow: request a port and wipe the chip. No flash, no
     * provision. Used by the "Erase" button on a "Your devices" entry.
     *
     * @param {object} opts
     * @param {(stage: string) => void} opts.onProgress
     * @param {() => void} opts.onSuccess
     * @param {(stage: string, error: Error) => void} opts.onError
     */
    async eraseOnly({ port: prePickedPort, onProgress, uiWaitForPortRetry,
                       onSuccess, onError, onLog }) {
        if (!navigator.serial) {
            onError("setup", new Error("Web Serial API not available — use Chrome, Edge, or Opera"));
            return;
        }

        let port;
        let transport;
        let esploader;
        let lastStage = "setup";
        const trackProgress = (stage) => { lastStage = stage; onProgress(stage); };
        const espTerminal = onLog ? {
            clean: () => {},
            writeLine: (s) => onLog(s),
            // Filter single-character writes — esptool's _connectAttempt
            // emits "." per sync packet and "_" per attempt boundary,
            // which fills the log with noise that looks bogus to a user
            // (one column of dots, then underscores, no context). The
            // writeLine path still carries the actual progress lines
            // ("esptool.js", "Serial port", "Connecting...", chip-info,
            // erase / write progress, baud-rate switch).
            write: (s) => { if (s && s.length > 1) onLog(s); },
        } : undefined;

        try {
            trackProgress("request-port");
            // Pre-picked port: same probe-and-settle pattern as start().
            // See the comment there for the wrong-port / stale-handle
            // cases this catches and why the post-close settling wait
            // matters on macOS.
            let probeFailed = false;
            if (prePickedPort) {
                try {
                    await prePickedPort.open({ baudRate: 115200 });
                    // See start() for why this signal-reset matters.
                    try {
                        await prePickedPort.setSignals({
                            requestToSend: true,
                            dataTerminalReady: true,
                        });
                    } catch (_) { /* setSignals optional */ }
                    await prePickedPort.close();
                    port = prePickedPort;
                } catch (e) {
                    if (onLog) onLog(`[orchestrator] picked port doesn't open (${e.message || e}); re-prompting — pick a different one`);
                    probeFailed = true;
                }
            }
            if (!port) {
                if (probeFailed) {
                    trackProgress("wrong-port-retry");
                    if (uiWaitForPortRetry) await uiWaitForPortRetry();
                    trackProgress("request-port");
                }
                port = await navigator.serial.requestPort({});
            }

            trackProgress("connect-flash");
            transport = new Transport(port, false);
            esploader = new ESPLoader({
                transport,
                // Match start()'s 460800 — same chip-compat reasoning.
                baudrate: 460800,
                romBaudrate: 115200,
                terminal: espTerminal,
            });
            await connectAndDescribeChip(esploader);

            trackProgress("erase");
            await esploader.eraseFlash();

            trackProgress("reboot");
            await esploader.hardReset();

            trackProgress("done");
            onSuccess();
        } catch (e) {
            onError(lastStage, e);
        } finally {
            try {
                if (transport) await transport.disconnect();
            } catch (_) { /* ignore */ }
            try {
                if (port) await port.close();
            } catch (_) { /* ignore */ }
        }
    },
};
