// MoonCloud Stats server contract: what reaches storage, and what cannot.
//
// The report endpoint is open to anyone, so its input is untrusted twice over: a field the
// firmware never sends can still arrive, and a field it does send can be any length. These tests
// pin the two rules that keep the privacy policy true on the server side as well as the device
// side: only allowlisted fields are stored, and the country is derived at the edge rather than
// from anything the caller supplies.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const source = readFileSync(new URL("../../mooncloud/worker.js", import.meta.url), "utf8");

// The worker is an ES module written for the Workers runtime, so it is exercised here by pulling
// the pure helpers out of it rather than by booting a fake runtime: `clean` is where every
// storage decision is made, and it is the thing worth pinning.
const cleanSource = source.slice(
  source.indexOf("function clean("),
  source.indexOf("async function handleReport")
);
const ALLOWED = JSON.parse(
  source.slice(source.indexOf("const ALLOWED = ["), source.indexOf("];") + 1)
    .replace("const ALLOWED = ", "")
    .replace(/,(\s*)\]/, "]")
    .replace(/\/\/.*$/gm, "")
);
const clean = new Function(
  "ALLOWED", "MAX_STRING", "MAX_MODULES",
  `${cleanSource}; return clean;`
)(ALLOWED, 64, 64);

test("a report cannot smuggle a field the firmware never sends", () => {
  const row = clean(
    {
      installationId: "c26086e8da9b6fb2d4b81e4ca71f97e5",
      chip: "ESP32-S3",
      // None of these are in the allowlist. A device would never send them; someone posting by
      // hand might, and the server must not create columns for them.
      deviceName: "ewoud-livingroom",
      mac: "2A:DA:B7:C6:A0:94",
      ssid: "Travelrouter",
      password: "hunter2",
      latitude: "52.37",
    },
    "NL"
  );

  assert.equal(row.deviceName, undefined);
  assert.equal(row.mac, undefined);
  assert.equal(row.ssid, undefined);
  assert.equal(row.password, undefined);
  assert.equal(row.latitude, undefined);
  assert.equal(row.chip, "ESP32-S3");
});

test("the country comes from the edge, never from the report", () => {
  // A caller claiming a different country must not be believed: the value is the one Cloudflare
  // resolved, and the whole reason it is trustworthy is that no address was stored to derive it.
  const row = clean({ installationId: "x".repeat(32), country: "AQ" }, "NL");
  assert.equal(row.country, "NL");
});

test("an unknown country is recorded rather than left empty", () => {
  const row = clean({ installationId: "x".repeat(32) }, undefined);
  assert.equal(row.country, "??");
});

test("the received date is a day, not a moment", () => {
  // A timestamp precise to the second, combined with a country, is a fingerprint. A date is not.
  const row = clean({ installationId: "x".repeat(32) }, "NL");
  assert.match(row.receivedAt, /^\d{4}-\d{2}-\d{2}$/);
});

test("oversized values are dropped rather than truncated into storage", () => {
  const row = clean(
    { installationId: "x".repeat(32), chip: "E".repeat(500) },
    "NL"
  );
  assert.equal(row.chip, undefined);
});

test("the module list is bounded and flattened", () => {
  const row = clean(
    {
      installationId: "x".repeat(32),
      modules: ["System", "Network", ...Array(200).fill("Filler")],
    },
    "NL"
  );
  assert.equal(row.modules.split(",").length, 64);
  assert.ok(row.modules.startsWith("System,Network"));
});

test("a non-string sneaking into the module list is dropped", () => {
  const row = clean(
    { installationId: "x".repeat(32), modules: ["System", 42, null, "Network"] },
    "NL"
  );
  assert.equal(row.modules, "System,Network");
});

test("the stats endpoint selects only aggregates, never a row", () => {
  // The endpoint is world-readable, so a query that returned installationId would publish exactly
  // the identifier the privacy policy promises to hold rather than show.
  const stats = source.slice(source.indexOf("async function handleStats"));
  assert.ok(stats.includes("COUNT(DISTINCT installationId)"));
  assert.ok(!/SELECT\s+installationId/i.test(stats));
  assert.ok(!/SELECT\s+\*/i.test(stats));
});

// MoonTalk post validation: the shape a device actually sends must be ACCEPTED.
//
// Written after a change truncated the sender to 8 characters at storage while the validator still
// required 32, so every message a device sent was rejected with 400 and the board silently stopped
// receiving anything. Nothing caught it: the device cleared its box, the UI showed no error, and no
// test covered the post path at all. These pin both halves of that contract.
test("a message from a device is accepted, and a malformed sender is not", () => {
    // The validator's length check and what the handler stores must agree. A slice narrower than
    // the check means nothing can ever pass it.
    const stored = source.match(/const sender = typeof msg\.sender === "string" \? msg\.sender\.slice\(0, (\d+)\)/);
    assert.ok(stored, "the talk handler must derive `sender` from the request");
    const check = source.match(/sender\.length !== (\d+)\)/);
    assert.ok(check, "the talk handler must validate the sender length");
    assert.equal(stored[1], check[1],
                 "the stored sender and the validated length must match, or every post is rejected");

    // And the published form is the SHORT one, which is the property the truncation was meant to
    // protect: stored in full, published at 8.
    assert.match(source, /m\.sender\.slice\(0, SENDER_CHARS\)/,
                 "a published message must carry only the id prefix");
});
