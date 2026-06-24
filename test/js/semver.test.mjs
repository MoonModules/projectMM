// Semver compare contract — pins the parse + precedence rules src/ui/semver.js implements
// (semver.org §2/§9/§11). The firmware "update available" badge relies on these: a clean stable
// release must out-rank a `-dev` build of the same core, and a real bump must register as newer.
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { parse, compare, isNewer } from "../../src/ui/semver.js";

test("parse: core, prerelease, leading v, build metadata", () => {
    assert.deepEqual(parse("2.0.0"), { major: 2, minor: 0, patch: 0, prerelease: [] });
    assert.deepEqual(parse("v2.1.0"), { major: 2, minor: 1, patch: 0, prerelease: [] });  // leading v tolerated
    assert.deepEqual(parse("2.1.0-dev"), { major: 2, minor: 1, patch: 0, prerelease: ["dev"] });
    assert.deepEqual(parse("1.0.0-rc.2"), { major: 1, minor: 0, patch: 0, prerelease: ["rc", "2"] });
    assert.deepEqual(parse("1.2.3+build.7"), { major: 1, minor: 2, patch: 3, prerelease: [] });  // build dropped
    assert.equal(parse("not-a-version"), null);
    assert.equal(parse("2.0"), null);          // incomplete core
    assert.equal(parse(undefined), null);
});

test("compare: numeric core ordering", () => {
    assert.equal(compare("2.0.0", "1.0.0"), 1);
    assert.equal(compare("1.0.0", "2.0.0"), -1);
    assert.equal(compare("2.1.0", "2.0.5"), 1);   // minor beats patch
    assert.equal(compare("2.0.1", "2.0.0"), 1);
    assert.equal(compare("2.0.0", "2.0.0"), 0);
    assert.equal(compare("v2.0.0", "2.0.0"), 0);  // leading v doesn't change value
});

test("compare: a release out-ranks a prerelease of the same core (§11)", () => {
    assert.equal(compare("2.1.0", "2.1.0-dev"), 1);    // stable > dev — the badge's core case
    assert.equal(compare("2.1.0-dev", "2.1.0"), -1);
    assert.equal(compare("1.0.0-rc.1", "1.0.0"), -1);
});

test("compare: prerelease identifier precedence (§11)", () => {
    assert.equal(compare("1.0.0-alpha", "1.0.0-beta"), -1);          // ASCII: alpha < beta
    assert.equal(compare("1.0.0-rc.2", "1.0.0-rc.1"), 1);           // numeric identifiers as numbers
    assert.equal(compare("1.0.0-rc.10", "1.0.0-rc.2"), 1);         // 10 > 2 numerically (not string)
    assert.equal(compare("1.0.0-1", "1.0.0-alpha"), -1);          // numeric id < alphanumeric id
    assert.equal(compare("1.0.0-rc.1.1", "1.0.0-rc.1"), 1);       // longer set wins when prefix equal
});

test("compare: unparseable side sorts lowest (never falsely claims newer)", () => {
    assert.equal(compare("garbage", "1.0.0"), -1);
    assert.equal(compare("1.0.0", "garbage"), 1);
    assert.equal(compare("garbage", "junk"), 0);
});

test("isNewer: the update-available predicate", () => {
    assert.equal(isNewer("2.1.0", "2.0.0"), true);      // newer stable available
    assert.equal(isNewer("2.0.0", "2.0.0"), false);     // up to date
    assert.equal(isNewer("2.0.0", "2.1.0-dev"), false); // dev build is AHEAD of latest stable → no update
    assert.equal(isNewer("1.0.0", "2.0.0"), false);     // device newer than candidate
});
