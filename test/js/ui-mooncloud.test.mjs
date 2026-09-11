// MoonCloud card contracts, pinned in the source the way ui-visibility does.
//
// Every MoonCloud bug found by hand tonight lived in app.js, not in the firmware: the C++ tests
// were right that the device posted the message and cleared its control, while the browser showed
// stale text and a stale board. That is the class these pin.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const app = readFileSync(join(ROOT, "src", "ui", "app.js"), "utf8");

test("MoonTalk adds no special case to the shared control path", () => {
    // MoonTalk is a proof of the plumbing a later member reuses, so it earns its place by adding
    // NOTHING to the UI. An earlier shape carried twelve special cases here: two module-scoped
    // globals, a control renderer branch keyed on the literal control name, an incremental message
    // cache and a reload callback. Each was a workaround for the one before it, and each broke the
    // next thing. A second member has to be able to follow the same path.
    for (const leak of ["moonTalkCache", "moonTalkReload", 'ctrl.name === "message"']) {
        assert.ok(!app.includes(leak),
                  `${leak} is a MoonTalk special case in the shared UI`);
    }
});

test("one refresh rule serves every MoonCloud member", () => {
    // Consent granted sends a report, a settled message posts one, and a later member will have its
    // own trigger: all three change what the server holds, and the card shows the server. A hook per
    // control was two copies of one idea with two different guessed delays, and the message one was
    // already wrong once messages gained a settle window.
    const i = app.indexOf('"MoonStatsModule", "MoonTalkModule"');
    assert.ok(i > 0, "no shared MoonCloud refresh rule in sendControl");
    const rule = app.slice(i, i + 400);
    assert.ok(rule.includes("refetchState"), "the rule must refresh the card");
    assert.ok(!/\.value\s*=\s*""/.test(rule), "it must not reach into the DOM");

    // Re-checked rather than timed once: how long the device takes to act is its own business, and
    // a single delay is a guess that goes stale the moment a member's timing changes.
    assert.match(rule, /for \(const delay of \[/,
                 "the refresh must be retried rather than fired once after a guessed delay");

    // And the per-control hooks are gone.
    assert.ok(!app.includes('controlName === "message"'),
              "no message-specific refresh hook");
    assert.ok(!app.includes('moduleName === "Stats" && controlName === "consent"'),
              "no consent-specific refresh hook");
});

test("both MoonCloud fetches use one compiled-in address", () => {
    // There is ONE MoonCloud, so its address is a constant rather than a control. A field on the
    // card invited editing a value whose only correct setting is the default, and a typo there
    // means reports vanish silently, because a failed report is never retried. Moving the server
    // is a release.
    assert.ok(app.includes("const kMoonCloudUrl ="), "the address must be a single constant");
    for (const path of ["/api/stats", "/api/talk"]) {
        assert.ok(app.includes(`kMoonCloudUrl + "${path}"`),
                  `${path} must be built from that constant`);
    }
    // And nothing reads it from state any more.
    assert.ok(!app.includes("moonCloudBase("), "the per-container resolver is gone");
    assert.ok(!app.includes("moonTalkUrl("), "the per-child resolver is gone");
});

test("consent is asked once, above the control, with no duplicate buttons", () => {
    // The consent dropdown already offers Yes / Not now / Never, so a button row beside it is the
    // same choice rendered twice and costs a block of the card to say nothing new.
    const i = app.indexOf("function renderMoonCloudConsent(");
    assert.ok(i > 0, "no consent renderer");
    const fn = app.slice(i, i + 1600);
    assert.ok(fn.includes('Number(consent.value) !== 0'),
              "the explanation shows only while the question is unanswered");
    assert.ok(!fn.includes("mooncloud-consent-buttons"),
              "no second set of consent buttons beside the dropdown");
});
