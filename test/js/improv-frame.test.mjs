// Improv frame-contract tests — pin the wire format the device C++
// (src/core/ImprovFrame.h), Python (scripts/build/improv_provision.py), and the
// installer JS (docs/install/improv-frame.js) must all agree on byte-for-byte.
// The golden vectors here are asserted identically in test/python/test_improv_frame.py
// so the JS and Python builders can't drift; they're hand-verified against the C++
// checksum (sum-mod-256) too. Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import {
    buildImprovFrame,
    encodeApplyOpFrames,
    APPLY_OP_CHUNK_MAX,
    IMPROV_CMD_APPLY_OP,
    IMPROV_CMD_SET_TX_POWER,
    IMPROV_FRAME_TYPE_RPC,
    IMPROV_MAGIC,
} from "../../docs/install/improv-frame.js";

const hex = (u8) => Array.from(u8).map((b) => b.toString(16).padStart(2, "0")).join(" ");
const bytes = (s) => Array.from(new TextEncoder().encode(s));

test("frame layout: magic, version, type, length, payload, checksum", () => {
    const frame = buildImprovFrame(IMPROV_FRAME_TYPE_RPC, new Uint8Array([0x01]));
    assert.deepEqual(Array.from(frame.subarray(0, 6)), IMPROV_MAGIC, "magic");
    assert.equal(frame[6], 0x01, "version");
    assert.equal(frame[7], IMPROV_FRAME_TYPE_RPC, "type");
    assert.equal(frame[8], 1, "length");
    assert.equal(frame[9], 0x01, "payload");
    assert.equal(frame.length, 11, "total = 9 header + 1 payload + 1 checksum");
});

test("checksum is sum-mod-256 of the first 9+length bytes", () => {
    const payload = new Uint8Array([0xAA, 0xBB, 0xCC]);
    const frame = buildImprovFrame(IMPROV_FRAME_TYPE_RPC, payload);
    let sum = 0;
    for (let i = 0; i < frame.length - 1; i++) sum = (sum + frame[i]) & 0xff;
    assert.equal(frame[frame.length - 1], sum);
});

test("golden vector G1: buildImprovFrame(RPC, [0x01])", () => {
    // Shared with test/python G1. Hand-verified checksum 0xe3.
    const frame = buildImprovFrame(IMPROV_FRAME_TYPE_RPC, new Uint8Array([0x01]));
    assert.equal(hex(frame), "49 4d 50 52 4f 56 01 03 01 01 e3");
});

test("golden vector G2: a small APPLY_OP set op is a single frame", () => {
    const op = { op: "set", module: "Grid", control: "width", value: 8 };
    const frames = encodeApplyOpFrames(op);
    assert.equal(frames.length, 1, "fits one frame");
    const f = frames[0];
    assert.equal(f[7], IMPROV_FRAME_TYPE_RPC);
    assert.equal(f[9 + 0], IMPROV_CMD_APPLY_OP, "payload[0] = 0xFC");
    assert.equal(f[9 + 1], 0, "seq");
    assert.equal(f[9 + 2], 1, "last");
    // payload after the 3-byte header is the op JSON, byte-identical
    assert.deepEqual(Array.from(f.subarray(9 + 3, 9 + f[8])), bytes(JSON.stringify(op)));
});

test("golden vector G3: a >125-byte op chunks into ordered frames", () => {
    const op = { op: "set", module: "X", control: "pins", value: "1".repeat(140) };
    const json = JSON.stringify(op);
    assert.ok(new TextEncoder().encode(json).length > APPLY_OP_CHUNK_MAX, "forces >1 chunk");
    const frames = encodeApplyOpFrames(op);
    assert.equal(frames.length, 2);
    // frame 0: seq 0, last 0, full chunk
    assert.equal(frames[0][9 + 1], 0, "f0 seq");
    assert.equal(frames[0][9 + 2], 0, "f0 not-last");
    assert.equal(frames[0][8] - 3, APPLY_OP_CHUNK_MAX, "f0 carries a full chunk");
    // frame 1: seq 1, last 1, remainder
    assert.equal(frames[1][9 + 1], 1, "f1 seq");
    assert.equal(frames[1][9 + 2], 1, "f1 last");
    // reassembling the chunks reproduces the op JSON exactly
    let reassembled = [];
    for (const f of frames) reassembled.push(...f.subarray(9 + 3, 9 + f[8]));
    assert.deepEqual(reassembled, bytes(json));
});

test("APPLY_OP always emits at least one frame (so `last` always sends)", () => {
    const frames = encodeApplyOpFrames({});
    assert.equal(frames.length, 1);
    assert.equal(frames[0][9 + 2], 1, "last=1 on the lone frame");
});

// ---------------------------------------------------------------------------
// PR regression: IMPROV_CMD_SET_DEVICE_MODEL (0xFE) removed from improv-frame.js
// ---------------------------------------------------------------------------

test("IMPROV_CMD_SET_DEVICE_MODEL is NOT exported (removed in this PR)", async () => {
    // The SET_DEVICE_MODEL RPC (0xFE) was a dedicated vendor command; it has been
    // removed — the device model is now just one `set` op in the APPLY_OP stream.
    // Verify the export no longer exists so callers can't accidentally reference it.
    const mod = await import("../../docs/install/improv-frame.js");
    assert.equal(
        mod.IMPROV_CMD_SET_DEVICE_MODEL,
        undefined,
        "IMPROV_CMD_SET_DEVICE_MODEL must not be exported",
    );
    // 0xFE must not appear as a value for any named export either
    const values = Object.values(mod).filter(v => typeof v === "number");
    assert.ok(!values.includes(0xFE), "0xFE must not be exported as any constant");
});

// ---------------------------------------------------------------------------
// IMPROV_CMD_SET_TX_POWER — still exported, value and wire format
// ---------------------------------------------------------------------------

test("IMPROV_CMD_SET_TX_POWER equals 0xFD", () => {
    assert.equal(IMPROV_CMD_SET_TX_POWER, 0xFD);
});

test("SET_TX_POWER frame layout: [cmd][1][dBm] three-byte payload", () => {
    // The orchestrator sends: buildImprovFrame(IMPROV_FRAME_TYPE_RPC,
    //   new Uint8Array([IMPROV_CMD_SET_TX_POWER, 1, dBm & 0xFF]))
    const dBm = 8;
    const payload = new Uint8Array([IMPROV_CMD_SET_TX_POWER, 0x01, dBm]);
    const frame = buildImprovFrame(IMPROV_FRAME_TYPE_RPC, payload);
    assert.equal(frame[7], IMPROV_FRAME_TYPE_RPC, "type byte");
    assert.equal(frame[8], 3, "payload length = 3");
    assert.equal(frame[9], IMPROV_CMD_SET_TX_POWER, "payload[0] = 0xFD");
    assert.equal(frame[10], 0x01, "payload[1] = length-of-value = 1");
    assert.equal(frame[11], dBm, "payload[2] = dBm value");
    assert.equal(frame.length, 13, "total frame length");
});

test("golden vector: SET_TX_POWER(8 dBm) frame bytes", () => {
    // Hand-verified: IMPROV_MAGIC + version(1) + type(3) + length(3) +
    //   [0xFD, 0x01, 0x08] + checksum(0xEA).
    // Checksum = (0x49+0x4d+0x50+0x52+0x4f+0x56+0x01+0x03+0x03+0xFD+0x01+0x08) mod 256
    //          = 746 mod 256 = 234 = 0xEA.
    const frame = buildImprovFrame(
        IMPROV_FRAME_TYPE_RPC,
        new Uint8Array([IMPROV_CMD_SET_TX_POWER, 0x01, 8]),
    );
    assert.equal(hex(frame), "49 4d 50 52 4f 56 01 03 03 fd 01 08 ea");
});

test("SET_TX_POWER frame: dBm=0 (lift cap) encodes correctly", () => {
    const frame = buildImprovFrame(
        IMPROV_FRAME_TYPE_RPC,
        new Uint8Array([IMPROV_CMD_SET_TX_POWER, 0x01, 0]),
    );
    assert.equal(frame[11], 0, "dBm=0 lifts the cap");
    assert.equal(frame.length, 13);
});

test("SET_TX_POWER frame: dBm=21 (max) encodes correctly", () => {
    const frame = buildImprovFrame(
        IMPROV_FRAME_TYPE_RPC,
        new Uint8Array([IMPROV_CMD_SET_TX_POWER, 0x01, 21]),
    );
    assert.equal(frame[11], 21);
});

// ---------------------------------------------------------------------------
// buildImprovFrame edge cases
// ---------------------------------------------------------------------------

test("buildImprovFrame throws if payload exceeds 255 bytes", () => {
    const oversized = new Uint8Array(256);
    assert.throws(
        () => buildImprovFrame(IMPROV_FRAME_TYPE_RPC, oversized),
        /255/,
        "must mention the 255 limit in the error",
    );
});

test("buildImprovFrame accepts payload of exactly 255 bytes", () => {
    const maxPayload = new Uint8Array(255).fill(0x01);
    const frame = buildImprovFrame(IMPROV_FRAME_TYPE_RPC, maxPayload);
    assert.equal(frame[8], 255, "length byte = 255");
    assert.equal(frame.length, 6 + 1 + 1 + 1 + 255 + 1, "total frame size");
});

// ---------------------------------------------------------------------------
// encodeApplyOpFrames boundary cases
// ---------------------------------------------------------------------------

test("exactly APPLY_OP_CHUNK_MAX bytes of JSON stays in one frame", () => {
    // Craft an op whose JSON is exactly APPLY_OP_CHUNK_MAX bytes.
    // {"op":"set","module":"X","control":"c","value":"<padding>"}
    const prefix = '{"op":"set","module":"X","control":"c","value":"';
    const suffix = '"}';
    const pad = APPLY_OP_CHUNK_MAX - prefix.length - suffix.length;
    // Guard: if pad < 0 the test premise is broken.
    assert.ok(pad >= 0, "prefix+suffix fits within APPLY_OP_CHUNK_MAX");
    const value = "A".repeat(pad);
    const op = { op: "set", module: "X", control: "c", value };
    const json = JSON.stringify(op);
    assert.equal(new TextEncoder().encode(json).length, APPLY_OP_CHUNK_MAX, "JSON is exactly the chunk max");
    const frames = encodeApplyOpFrames(op);
    assert.equal(frames.length, 1, "fits in one frame");
    assert.equal(frames[0][9 + 2], 1, "last=1");
});

test("APPLY_OP_CHUNK_MAX + 1 bytes of JSON produces exactly two frames", () => {
    // Build an op whose JSON exceeds the chunk max by exactly one byte.
    const prefix = '{"op":"set","module":"X","control":"c","value":"';
    const suffix = '"}';
    const pad = APPLY_OP_CHUNK_MAX - prefix.length - suffix.length + 1;
    assert.ok(pad >= 0, "prefix+suffix fits within APPLY_OP_CHUNK_MAX+1");
    const value = "A".repeat(pad);
    const op = { op: "set", module: "X", control: "c", value };
    const jsonLen = new TextEncoder().encode(JSON.stringify(op)).length;
    assert.equal(jsonLen, APPLY_OP_CHUNK_MAX + 1);
    const frames = encodeApplyOpFrames(op);
    assert.equal(frames.length, 2);
    assert.equal(frames[0][9 + 1], 0, "frame 0: seq=0");
    assert.equal(frames[0][9 + 2], 0, "frame 0: not last");
    assert.equal(frames[1][9 + 1], 1, "frame 1: seq=1");
    assert.equal(frames[1][9 + 2], 1, "frame 1: last=1");
    // The two chunks reassemble to the original JSON exactly.
    const enc = new TextEncoder().encode(JSON.stringify(op));
    const chunk0 = enc.subarray(0, APPLY_OP_CHUNK_MAX);
    const chunk1 = enc.subarray(APPLY_OP_CHUNK_MAX);
    assert.deepEqual(Array.from(frames[0].subarray(9 + 3, 9 + frames[0][8])), Array.from(chunk0));
    assert.deepEqual(Array.from(frames[1].subarray(9 + 3, 9 + frames[1][8])), Array.from(chunk1));
});
