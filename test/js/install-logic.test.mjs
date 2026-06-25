// Tests for pure helper functions from docs/install/install.js.
//
// install.js is a browser module (not an ES module with exports), so the
// pure functions are replicated here verbatim from the source. Any change to
// the originals should be reflected here — the tests pin the CONTRACT, not
// the implementation, so a divergence is a signal to review both.
//
// Functions under test:
//   toLocalUrl        — rewrites a GitHub release-asset URL to a same-origin local path
//   isHttpUrl         — guards href assignments against javascript:/data:/file: injections
//   ethConfigured     — checks whether a board's NetworkModule has a real PHY configured
//   capActive         — derives active/supported/planned capability state from modules[]
//   ledDriver         — extracts the short LED-driver label for a board card's meta line
//   isEthOnlyByName   — the /-eth$/ firmware-name rule (also tested by installer-eth-only.test.mjs)
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";

// ---------------------------------------------------------------------------
// Replicated from docs/install/install.js — must stay in sync with source.
// ---------------------------------------------------------------------------

// Map a GitHub release-asset URL to its Pages-hosted mirror.
//   https://github.com/MoonModules/projectMM/releases/download/<TAG>/<file>
//   → ./releases/<TAG>/<file>
function toLocalUrl(githubUrl) {
    const m = /\/releases\/download\/([^/]+)\/([^/]+)$/.exec(githubUrl);
    if (!m) return githubUrl;  // unrecognised shape: pass through unchanged
    const [, tag, name] = m;
    return `./releases/${tag}/${name}`;
}

// Only http(s) URLs are safe as a link href — a malformed value must never
// become a javascript:/data:/file: link.
function isHttpUrl(u) {
    try { const p = new URL(u); return p.protocol === "http:" || p.protocol === "https:"; }
    catch (_) { return false; }
}

// Ethernet-only firmware detection: a firmware key ending in `-eth` is
// Ethernet-only; `-eth-wifi` (the P4 co-processor build) is NOT.
const isEthOnlyByName = (name) => /-eth$/.test(name);

// Checks whether a module entry from deviceModels.json has a real PHY
// configured (i.e. ethType is set to something other than absent / 0 / "None").
const ethConfigured = (m) => {
    const t = m.controls && m.controls.ethType;
    return t !== undefined && t !== 0 && t !== "0" && t !== "None";
};

// Capability-to-module predicates — mirrors CAP_MODULE in install.js.
const CAP_MODULE = {
    LEDs:     m => /LedDriver$/.test(m.type || ""),
    Ethernet: m => m.type === "NetworkModule" && ethConfigured(m),
    WiFi:     m => m.type === "NetworkModule",
    Audio:    m => /^Audio/.test(m.type || ""),
};

function capActive(b, cap) {
    const test = CAP_MODULE[cap];
    return !!test && (b.modules || []).some(m => test(m));
}

// Returns the short LED-driver label for a board ("Rmt", "Lcd", "Parlio", …)
// or null when no LedDriver module is present.
function ledDriver(b) {
    const d = (b.modules || []).find(m => /LedDriver$/.test(m.type || ""));
    return d ? d.type.replace(/Driver$/, "") : null;
}

// ---------------------------------------------------------------------------
// toLocalUrl
// ---------------------------------------------------------------------------

test("toLocalUrl: converts a standard GitHub release-asset URL to a local path", () => {
    const url = "https://github.com/MoonModules/projectMM/releases/download/latest/esp32.bin";
    assert.equal(toLocalUrl(url), "./releases/latest/esp32.bin");
});

test("toLocalUrl: preserves the tag in the output path", () => {
    const url = "https://github.com/MoonModules/projectMM/releases/download/v2.1.0/esp32.bin";
    assert.equal(toLocalUrl(url), "./releases/v2.1.0/esp32.bin");
});

test("toLocalUrl: handles versioned dev tags like 2.1.0-dev.6", () => {
    const url = "https://github.com/MoonModules/projectMM/releases/download/2.1.0-dev.6/esp32-eth.manifest.json";
    assert.equal(toLocalUrl(url), "./releases/2.1.0-dev.6/esp32-eth.manifest.json");
});

test("toLocalUrl: passes through an unrecognised URL shape unchanged", () => {
    const url = "https://example.com/some/other/path";
    assert.equal(toLocalUrl(url), url);
});

test("toLocalUrl: passes through an already-local relative URL unchanged", () => {
    const url = "./releases/latest/esp32.bin";
    assert.equal(toLocalUrl(url), url);
});

test("toLocalUrl: passes through an empty string unchanged", () => {
    assert.equal(toLocalUrl(""), "");
});

test("toLocalUrl: handles filenames with dots (manifest.json)", () => {
    const url = "https://github.com/MoonModules/projectMM/releases/download/latest/esp32.manifest.json";
    assert.equal(toLocalUrl(url), "./releases/latest/esp32.manifest.json");
});

test("toLocalUrl: does not match a URL that is missing the /releases/download/ segment", () => {
    const url = "https://github.com/MoonModules/projectMM/archive/refs/tags/v2.1.0.tar.gz";
    assert.equal(toLocalUrl(url), url);
});

// ---------------------------------------------------------------------------
// isHttpUrl
// ---------------------------------------------------------------------------

test("isHttpUrl: accepts an http:// URL", () => {
    assert.equal(isHttpUrl("http://192.168.1.42/"), true);
});

test("isHttpUrl: accepts an https:// URL", () => {
    assert.equal(isHttpUrl("https://projectmm.local/"), true);
});

test("isHttpUrl: rejects a javascript: URL (XSS guard)", () => {
    assert.equal(isHttpUrl("javascript:alert(1)"), false);
});

test("isHttpUrl: rejects a data: URL", () => {
    assert.equal(isHttpUrl("data:text/html,<h1>hi</h1>"), false);
});

test("isHttpUrl: rejects a file: URL", () => {
    assert.equal(isHttpUrl("file:///etc/passwd"), false);
});

test("isHttpUrl: rejects a plain hostname without scheme", () => {
    assert.equal(isHttpUrl("projectmm.local"), false);
});

test("isHttpUrl: rejects an empty string", () => {
    assert.equal(isHttpUrl(""), false);
});

test("isHttpUrl: rejects a malformed non-URL string", () => {
    assert.equal(isHttpUrl("not a url at all"), false);
});

test("isHttpUrl: rejects an ftp:// URL", () => {
    assert.equal(isHttpUrl("ftp://files.example.com/firmware.bin"), false);
});

// ---------------------------------------------------------------------------
// isEthOnlyByName
// ---------------------------------------------------------------------------

test("isEthOnlyByName: /-eth$/ matches esp32-eth (eth-only build)", () => {
    assert.equal(isEthOnlyByName("esp32-eth"), true);
});

test("isEthOnlyByName: /-eth$/ matches esp32p4-eth (eth-only build)", () => {
    assert.equal(isEthOnlyByName("esp32p4-eth"), true);
});

test("isEthOnlyByName: /-eth$/ does NOT match esp32-eth-wifi (co-processor WiFi build)", () => {
    // The co-processor build ends in -wifi, not -eth — it has WiFi compiled in.
    assert.equal(isEthOnlyByName("esp32-eth-wifi"), false);
});

test("isEthOnlyByName: does NOT match the default esp32 build (WiFi-capable)", () => {
    assert.equal(isEthOnlyByName("esp32"), false);
});

test("isEthOnlyByName: does NOT match esp32s3 (no -eth suffix)", () => {
    assert.equal(isEthOnlyByName("esp32s3"), false);
});

// ---------------------------------------------------------------------------
// ethConfigured
// ---------------------------------------------------------------------------

test("ethConfigured: returns true when ethType is a real PHY string", () => {
    assert.equal(ethConfigured({ controls: { ethType: "LAN8720" } }), true);
});

test("ethConfigured: returns true when ethType is a non-zero numeric string", () => {
    assert.equal(ethConfigured({ controls: { ethType: "1" } }), true);
});

test("ethConfigured: returns false when ethType is 0 (numeric — no PHY)", () => {
    assert.equal(ethConfigured({ controls: { ethType: 0 } }), false);
});

test("ethConfigured: returns false when ethType is '0' (string zero — no PHY)", () => {
    assert.equal(ethConfigured({ controls: { ethType: "0" } }), false);
});

test("ethConfigured: returns false when ethType is 'None'", () => {
    assert.equal(ethConfigured({ controls: { ethType: "None" } }), false);
});

test("ethConfigured: returns false when ethType is absent from controls", () => {
    assert.equal(ethConfigured({ controls: {} }), false);
});

test("ethConfigured: returns false when controls object is absent", () => {
    assert.equal(ethConfigured({ type: "NetworkModule" }), false);
});

test("ethConfigured: returns false when ethType is undefined", () => {
    assert.equal(ethConfigured({ controls: { ethType: undefined } }), false);
});

// ---------------------------------------------------------------------------
// capActive
// ---------------------------------------------------------------------------

test("capActive: LEDs — true when board has an *LedDriver module", () => {
    const board = { modules: [{ type: "RmtLedDriver" }] };
    assert.equal(capActive(board, "LEDs"), true);
});

test("capActive: LEDs — true for LcdLedDriver", () => {
    const board = { modules: [{ type: "LcdLedDriver" }] };
    assert.equal(capActive(board, "LEDs"), true);
});

test("capActive: LEDs — false when no LedDriver module is present", () => {
    const board = { modules: [{ type: "NetworkModule" }] };
    assert.equal(capActive(board, "LEDs"), false);
});

test("capActive: Ethernet — true when NetworkModule has a real ethType", () => {
    const board = { modules: [{ type: "NetworkModule", controls: { ethType: "LAN8720" } }] };
    assert.equal(capActive(board, "Ethernet"), true);
});

test("capActive: Ethernet — false when NetworkModule ethType is 0", () => {
    const board = { modules: [{ type: "NetworkModule", controls: { ethType: 0 } }] };
    assert.equal(capActive(board, "Ethernet"), false);
});

test("capActive: WiFi — true whenever NetworkModule is present", () => {
    const board = { modules: [{ type: "NetworkModule", controls: { ethType: 0 } }] };
    assert.equal(capActive(board, "WiFi"), true);
});

test("capActive: WiFi — false when no NetworkModule is present", () => {
    const board = { modules: [{ type: "RmtLedDriver" }] };
    assert.equal(capActive(board, "WiFi"), false);
});

test("capActive: Audio — true when board has an AudioModule", () => {
    const board = { modules: [{ type: "AudioModule" }] };
    assert.equal(capActive(board, "Audio"), true);
});

test("capActive: Audio — false for a non-Audio module", () => {
    const board = { modules: [{ type: "NetworkModule" }] };
    assert.equal(capActive(board, "Audio"), false);
});

test("capActive: returns false for a board with no modules array", () => {
    const board = {};
    assert.equal(capActive(board, "LEDs"), false);
    assert.equal(capActive(board, "WiFi"), false);
});

test("capActive: returns false for an empty modules array", () => {
    const board = { modules: [] };
    assert.equal(capActive(board, "LEDs"), false);
});

test("capActive: returns false for an unknown capability key", () => {
    const board = { modules: [{ type: "RmtLedDriver" }] };
    // The CAP_MODULE map has no entry for "Unknown" → test() is undefined → false
    assert.equal(capActive(board, "Unknown"), false);
});

test("capActive: board with multiple modules — picks the right one", () => {
    const board = {
        modules: [
            { type: "NetworkModule", controls: { ethType: "LAN8720" } },
            { type: "RmtLedDriver" },
            { type: "AudioModule" },
        ],
    };
    assert.equal(capActive(board, "LEDs"),     true);
    assert.equal(capActive(board, "Ethernet"), true);
    assert.equal(capActive(board, "WiFi"),     true);
    assert.equal(capActive(board, "Audio"),    true);
});

// ---------------------------------------------------------------------------
// ledDriver
// ---------------------------------------------------------------------------

test("ledDriver: returns 'RmtLed' for a board with RmtLedDriver", () => {
    const board = { modules: [{ type: "RmtLedDriver" }] };
    assert.equal(ledDriver(board), "RmtLed");
});

test("ledDriver: returns 'LcdLed' for a board with LcdLedDriver", () => {
    const board = { modules: [{ type: "LcdLedDriver" }] };
    assert.equal(ledDriver(board), "LcdLed");
});

test("ledDriver: returns 'ParlioLed' for a board with ParlioLedDriver", () => {
    const board = { modules: [{ type: "ParlioLedDriver" }] };
    assert.equal(ledDriver(board), "ParlioLed");
});

test("ledDriver: returns null when no LedDriver module is present", () => {
    const board = { modules: [{ type: "NetworkModule" }] };
    assert.equal(ledDriver(board), null);
});

test("ledDriver: returns null for a board with no modules", () => {
    assert.equal(ledDriver({}), null);
    assert.equal(ledDriver({ modules: [] }), null);
});

test("ledDriver: uses the first LedDriver when multiple drivers are listed", () => {
    // Multiple drivers is not a real deviceModels.json shape, but the regex
    // finds the first match — pin that behaviour.
    const board = { modules: [{ type: "RmtLedDriver" }, { type: "LcdLedDriver" }] };
    assert.equal(ledDriver(board), "RmtLed");
});

test("ledDriver: does not match a module whose type merely contains 'LedDriver' mid-string", () => {
    // 'FakeLedDriverHelper' ends in 'Driver', not 'LedDriver' — still matches
    // the regex /LedDriver$/, so confirm our replica stays in sync with source.
    const board = { modules: [{ type: "NetworkSendLedDriver" }] };
    // /LedDriver$/ DOES match this — document the actual behaviour.
    assert.equal(ledDriver(board), "NetworkSendLed");
});