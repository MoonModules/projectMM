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
//   [str_len]       1..23, length of board name in bytes
//   [str_bytes]     ASCII-printable 0x20..0x7E only
function encodeSetBoardPayload(board) {
    const nameBytes = new TextEncoder().encode(board);
    if (nameBytes.length === 0 || nameBytes.length > 23) {
        throw new Error(`board name length ${nameBytes.length}: must be 1..23`);
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

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

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
     * @param {boolean} [opts.eraseBefore=false] - when true, eraseFlash()
     *   before writeFlash. Wipes the entire chip including LittleFS (saved
     *   WiFi credentials, board name). Adds ~12 s. Default false because
     *   a normal re-flash overwrites in place and users usually want
     *   persistent state to survive a firmware bump.
     * @param {(stage: string, detail?: object) => void} opts.onProgress
     *   Stages: request-port, connect-flash, fetch-firmware, erase,
     *   flash, reboot, connect-improv, wifi-creds-form, provisioning,
     *   set-board, done. flash also carries { pct }. connect-flash carries
     *   { chipName } once detection succeeds.
     * @param {() => Promise<{ssid: string, password: string}>} opts.uiWaitForCreds
     *   Host page resolves this when the user fills in the WiFi form.
     * @param {(detail: {url: string, board: string, alreadyOnline?: boolean}) => void} opts.onSuccess
     *   `alreadyOnline:true` is set when the device booted into
     *   STATE_PROVISIONED (saved WiFi from a previous flash) — the SDK's
     *   initialize() fails and the orchestrator skips both the WiFi form
     *   and the SET_BOARD push. `url` and `board` are empty in that case;
     *   the host page shows a "device may already be online" message.
     * @param {(stage: string, error: Error) => void} opts.onError
     * @param {(line: string) => void} [opts.onLog] - optional: each line
     *   esptool-js writes to its "terminal" gets forwarded here. Host
     *   appends them to a collapsible <pre> in the modal so the user can
     *   see what happened on a failure.
     */
    async start({ manifestUrl, board, eraseBefore = false,
                   onProgress, uiWaitForCreds, onSuccess, onError, onLog }) {
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
            write: (s) => onLog(s),
        } : undefined;
        // ImprovSerial's logger arg is a console-shape object. We pass
        // `console` directly (proven working in the prior commit). A custom
        // wrapper here broke initialize() in testing — the SDK calls more
        // methods than just log/warn/error/debug (likely .info or .trace)
        // and undefined-method calls throw inside the SDK's await chain.
        // If we ever want SDK lines in the modal log too, wire a Proxy that
        // forwards every method to console and tees log/info to onLog.
        const improvLogger = console;

        try {
            trackProgress("request-port");
            port = await navigator.serial.requestPort({});

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
            // main() returns the human-readable chip identification, e.g.
            // "ESP32-D0WD-V3". Surface it in the modal so the user sees
            // detection succeeded before the 10-30s download + flash wait.
            const chipName = await esploader.main();
            trackProgress("connect-flash", { chipName });

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
            // the new endpoint.
            await new Promise(r => setTimeout(r, 2000));

            trackProgress("connect-improv");
            // Reopen the same SerialPort handle for the Improv connection.
            // 115200 is the standard Improv-Serial baudrate; the device's
            // ImprovProvisioningModule listens on UART0 at this rate (see
            // src/platform/esp32/platform_esp32_improv.cpp).
            await port.open({ baudRate: 115200 });
            improvClient = new ImprovSerial(port, improvLogger);
            // ImprovSerial's initialize() throws "Improv Wi-Fi Serial not
            // detected" when the device is already on WiFi (saved credentials
            // from a previous flash). The device reports STATE_PROVISIONED
            // immediately in that case; the SDK expects STATE_AUTHORIZED.
            // Treat this as a soft-success: skip WiFi provisioning and the
            // SET_BOARD push, surface a status message, and let the user
            // visit the device manually. They can set board via MoonDeck.
            let alreadyOnline = false;
            try {
                await improvClient.initialize();
            } catch (e) {
                const msg = (e && e.message) ? String(e.message) : String(e);
                if (msg.includes("Improv Wi-Fi Serial not detected")) {
                    alreadyOnline = true;
                } else {
                    throw e;
                }
            }

            if (alreadyOnline) {
                // Best-effort cleanup of the SDK that didn't complete its
                // handshake, then exit via the success path with a flag
                // the UI uses to render the "already online" message.
                try { if (improvClient) await improvClient.close(); } catch (_) { /* ignore */ }
                improvClient = null;
                trackProgress("done");
                onSuccess({
                    url: "",   // unknown — device didn't speak Improv back
                    board: "",
                    alreadyOnline: true,
                });
                return;
            }

            trackProgress("wifi-creds-form");
            const { ssid, password } = await uiWaitForCreds();

            trackProgress("provisioning");
            // provision() throws on failure (timeout, bad creds). 30s
            // timeout matches the device-side wait at
            // platform_esp32_improv.cpp::improvHandleProvision.
            await improvClient.provision(ssid, password, 30000);
            const deviceUrl = improvClient.nextUrl;
            if (!deviceUrl) {
                throw new Error("provision succeeded but no device URL returned");
            }

            // Push SET_BOARD vendor RPC if the user picked a board.
            // ImprovSerial holds the writable lock — close it first so we
            // can write our own raw frame. We're done with ImprovSerial
            // anyway (provision is the last standard command we need).
            if (board) {
                trackProgress("set-board");
                await improvClient.close();
                improvClient = null;
                await sendSetBoardFrame(port, board);
                // The device-side handler responds with RpcResponse, but we
                // don't wait for it — failures (validation rejection, etc.)
                // are reported via ErrorState which we'd need to re-open a
                // reader to see. Best-effort: device persists in next 2s
                // via FilesystemModule's debounced save. If push silently
                // failed, the field stays empty and the user can pick the
                // board via MoonDeck later.
                // Small grace period so the device's UART task finishes
                // processing before we close the port.
                await new Promise(r => setTimeout(r, 200));
            }

            trackProgress("done");
            onSuccess({ url: deviceUrl, board: board || "" });
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
     * Erase-only flow: request a port and wipe the chip. No flash, no
     * provision. Used by the "Erase" button on a "Your devices" entry.
     *
     * @param {object} opts
     * @param {(stage: string) => void} opts.onProgress
     * @param {() => void} opts.onSuccess
     * @param {(stage: string, error: Error) => void} opts.onError
     */
    async eraseOnly({ onProgress, onSuccess, onError, onLog }) {
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
            write: (s) => onLog(s),
        } : undefined;

        try {
            trackProgress("request-port");
            port = await navigator.serial.requestPort({});

            trackProgress("connect-flash");
            transport = new Transport(port, false);
            esploader = new ESPLoader({
                transport,
                // Match start()'s 460800 — same chip-compat reasoning.
                baudrate: 460800,
                romBaudrate: 115200,
                terminal: espTerminal,
            });
            await esploader.main();

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
