// S31 web-flash guard contract — the installer special-cases chips esptool-js
// (the browser flasher) can't handle, so a connect-flash failure shows "flash via
// the CLI" guidance instead of a bare timeout. The guard keys on a chip-family
// string (install.js `WEB_FLASH_UNSUPPORTED_CHIPS`); that string must match the
// `chip` a board in deviceModels.json actually reports, or the guidance never fires.
//
// Background: the ESP32-S31's ROM magic collides with the classic ESP32's, and
// esptool-js has only a magic table (no S31 secondary detection), so a browser
// flash can't work safely — CLI flashing (esptool.py) is the path. See
// install.js's WEB_FLASH_UNSUPPORTED_CHIPS comment + docs/backlog/backlog-core.md.
//
// This test pins two things so the guard can't silently break:
//   1. install.js declares ESP32-S31 in WEB_FLASH_UNSUPPORTED_CHIPS, and
//   2. a deviceModels.json board reports chip "ESP32-S31" (the exact guard string),
// so the connect-flash message logic actually triggers for the S31 board.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..");

const installJs = readFileSync(join(ROOT, "docs", "install", "install.js"), "utf8");
const boards = JSON.parse(
    readFileSync(join(ROOT, "docs", "install", "deviceModels.json"), "utf8")
);

// The chip families install.js flags as not-browser-flashable. Parsed from the
// source so the test reads the real declaration (no hard-coded copy that could drift).
function unsupportedChips() {
    const m = installJs.match(/WEB_FLASH_UNSUPPORTED_CHIPS\s*=\s*new Set\(\[([^\]]*)\]\)/);
    assert.ok(m, "install.js must declare WEB_FLASH_UNSUPPORTED_CHIPS as a Set literal");
    return m[1].split(",").map((s) => s.trim().replace(/^["']|["']$/g, "")).filter(Boolean);
}

test("install.js flags ESP32-S31 as not browser-flashable", () => {
    assert.ok(
        unsupportedChips().includes("ESP32-S31"),
        "WEB_FLASH_UNSUPPORTED_CHIPS must include ESP32-S31 (esptool-js has no safe S31 support)"
    );
});

test("the guard only fires on the connect-flash stage", () => {
    // The message must be scoped to connect-flash — other stages keep their own
    // diagnostics. Pin that the guard condition ANDs the stage check.
    assert.match(
        installJs,
        /stage\s*===\s*["']connect-flash["']\s*&&\s*WEB_FLASH_UNSUPPORTED_CHIPS\.has/,
        "the unsupported-chip guidance must be gated on stage === 'connect-flash'"
    );
});

test("every flagged chip matches a real deviceModels.json board chip", () => {
    // A guard string with no board would be dead code; pin that each flagged chip
    // is one a catalog board actually reports, so getSelectedBoardChip() can match it.
    const boardChips = new Set(boards.map((b) => b.chip));
    for (const chip of unsupportedChips()) {
        assert.ok(
            boardChips.has(chip),
            `WEB_FLASH_UNSUPPORTED_CHIPS has "${chip}" but no deviceModels.json board reports ` +
            `that chip — the guidance would never fire. Reconcile install.js with the catalog.`
        );
    }
});

test("the S31 board points at the esp32s31 firmware (so the CLI hint names the right build)", () => {
    const s31 = boards.find((b) => b.chip === "ESP32-S31");
    assert.ok(s31, "deviceModels.json must have an ESP32-S31 board");
    assert.ok(
        Array.isArray(s31.firmwares) && s31.firmwares.includes("esp32s31"),
        "the S31 board's firmwares must include esp32s31 (the build flash_esp32.py expects)"
    );
});
