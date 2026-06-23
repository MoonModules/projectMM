// Tests for the pushBoard helper added in scripts/moondeck_ui/app.js.
//
// The pushBoard function is a browser-environment closure (defined inside
// renderDevices, not exported), so we test the BEHAVIOURAL CONTRACT it enforces:
//
//   1. Success is determined by the JSON body field `j.ok`, NOT by the HTTP
//      status `r.ok` — because a device timeout mid-fan-out can return HTTP 200
//      while the body carries `{"ok": false}`.
//   2. A fetch() rejection (network error, AbortSignal timeout) calls onDone(false).
//   3. A body with `{"ok": true}` calls onDone(true).
//   4. A body with `{"ok": false}` calls onDone(false) even when HTTP 200.
//   5. onDone is optional; omitting it doesn't throw on any path.
//
// These are extracted as a standalone equivalent of the closure so the
// behavioural contract is verifiable without a DOM or browser globals.

import { test } from "node:test";
import assert from "node:assert/strict";

// ---------------------------------------------------------------------------
// Equivalent implementation of the pushBoard closure from app.js (verbatim
// logic, extracted for testability). If the app.js implementation changes,
// update this mirror to stay in sync.
// ---------------------------------------------------------------------------
function makePushBoard(fetchImpl, ip) {
    return function pushBoard(board, onDone) {
        fetchImpl("/api/push-board", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ ip, board }),
            signal: AbortSignal.timeout(10000),
        }).then(r => r.json()).then(j => onDone && onDone(!!j.ok))
          .catch(() => onDone && onDone(false));
    };
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a fake fetch that resolves with a JSON body.
function fakeFetch(body, status = 200) {
    return (_url, _opts) => Promise.resolve({
        ok: status >= 200 && status < 300,
        status,
        json: () => Promise.resolve(body),
    });
}

// Build a fake fetch that rejects (network error / timeout).
function failingFetch(err = new Error("fetch failed")) {
    return (_url, _opts) => Promise.reject(err);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

test("pushBoard calls onDone(true) when JSON body has ok:true (HTTP 200)", () => {
    return new Promise((resolve, reject) => {
        const fetch = fakeFetch({ ok: true });
        const pushBoard = makePushBoard(fetch, "192.168.1.42");
        pushBoard("MyBoard", (result) => {
            try {
                assert.equal(result, true, "onDone should receive true");
                resolve();
            } catch (e) {
                reject(e);
            }
        });
    });
});

test("pushBoard calls onDone(false) when JSON body has ok:false even though HTTP 200", () => {
    // HTTP 200 alone cannot be used to declare success — a device timeout
    // mid-fan-out returns HTTP 200 with {"ok": false} in the body.
    return new Promise((resolve, reject) => {
        const fetch = fakeFetch({ ok: false }, 200);
        const pushBoard = makePushBoard(fetch, "192.168.1.42");
        pushBoard("MyBoard", (result) => {
            try {
                assert.equal(result, false, "onDone should receive false on body ok:false");
                resolve();
            } catch (e) {
                reject(e);
            }
        });
    });
});

test("pushBoard calls onDone(false) on a fetch rejection (network error)", () => {
    return new Promise((resolve, reject) => {
        const fetch = failingFetch(new Error("Network unreachable"));
        const pushBoard = makePushBoard(fetch, "192.168.1.1");
        pushBoard("MyBoard", (result) => {
            try {
                assert.equal(result, false, "onDone should receive false on network error");
                resolve();
            } catch (e) {
                reject(e);
            }
        });
    });
});

test("pushBoard calls onDone(false) when fetch times out (AbortError)", () => {
    return new Promise((resolve, reject) => {
        const abortErr = new DOMException("The operation was aborted.", "AbortError");
        const fetch = failingFetch(abortErr);
        const pushBoard = makePushBoard(fetch, "10.0.0.5");
        pushBoard("SlowBoard", (result) => {
            try {
                assert.equal(result, false, "onDone should receive false on AbortError");
                resolve();
            } catch (e) {
                reject(e);
            }
        });
    });
});

test("pushBoard with no onDone does not throw on success", () => {
    // Fire-and-forget path: boardPicker change handler passes no callback.
    return new Promise((resolve) => {
        const fetch = fakeFetch({ ok: true });
        const pushBoard = makePushBoard(fetch, "192.168.1.42");
        pushBoard("MyBoard");   // onDone omitted — must not throw
        // Give the micro-task queue a tick to let the promise chain settle.
        setTimeout(resolve, 50);
    });
});

test("pushBoard with no onDone does not throw on fetch rejection", () => {
    return new Promise((resolve) => {
        const fetch = failingFetch();
        const pushBoard = makePushBoard(fetch, "192.168.1.42");
        pushBoard("MyBoard");   // onDone omitted — must not throw
        setTimeout(resolve, 50);
    });
});

test("pushBoard sends the board name and device IP in the POST body", () => {
    return new Promise((resolve, reject) => {
        let capturedBody;
        const fetch = (url, opts) => {
            capturedBody = JSON.parse(opts.body);
            return Promise.resolve({
                ok: true,
                json: () => Promise.resolve({ ok: true }),
            });
        };
        const pushBoard = makePushBoard(fetch, "172.16.0.10");
        pushBoard("projectMM testbench S3", (result) => {
            try {
                assert.equal(result, true);
                assert.equal(capturedBody.ip, "172.16.0.10", "IP forwarded to server");
                assert.equal(capturedBody.board, "projectMM testbench S3", "board name forwarded");
                resolve();
            } catch (e) {
                reject(e);
            }
        });
    });
});

test("pushBoard targets POST /api/push-board", () => {
    return new Promise((resolve, reject) => {
        let capturedUrl;
        let capturedMethod;
        const fetch = (url, opts) => {
            capturedUrl = url;
            capturedMethod = opts.method;
            return Promise.resolve({
                ok: true,
                json: () => Promise.resolve({ ok: true }),
            });
        };
        const pushBoard = makePushBoard(fetch, "192.168.1.1");
        pushBoard("SomeBoard", () => {
            try {
                assert.equal(capturedUrl, "/api/push-board");
                assert.equal(capturedMethod, "POST");
                resolve();
            } catch (e) {
                reject(e);
            }
        });
    });
});

test("pushBoard treats a truthy j.ok (non-boolean) as success", () => {
    // JSON parsing can yield numbers or strings; guard against !!j.ok coercion.
    return new Promise((resolve, reject) => {
        const fetch = fakeFetch({ ok: 1 });   // number 1, not boolean true
        const pushBoard = makePushBoard(fetch, "192.168.1.1");
        pushBoard("Board", (result) => {
            try {
                assert.equal(result, true, "!!1 === true");
                resolve();
            } catch (e) {
                reject(e);
            }
        });
    });
});

test("pushBoard treats a falsy j.ok (0) as failure", () => {
    return new Promise((resolve, reject) => {
        const fetch = fakeFetch({ ok: 0 });
        const pushBoard = makePushBoard(fetch, "192.168.1.1");
        pushBoard("Board", (result) => {
            try {
                assert.equal(result, false, "!!0 === false");
                resolve();
            } catch (e) {
                reject(e);
            }
        });
    });
});