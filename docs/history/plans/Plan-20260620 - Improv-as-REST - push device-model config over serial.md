# Plan — Improv-as-REST: push device-model config over serial

*Approved 2026-06-20. Saved per the CLAUDE.md "Plan before implementing" rule.*

## Context

**The problem.** When you flash a device from the deployed web installer (`https://moonmodules.org/projectMM/install/`) and pick a device model, the model's defaults (Grid 8×8, AudioSpectrum + RandomMap, the LED driver, brightness, …) are supposed to be applied to the device. Today they often aren't.

**Why.** The installer page is served over **HTTPS**; the ESP32 only serves **HTTP**. Browsers block an HTTPS page from calling `http://<device>/api/...` (mixed-content). So the original **push** (installer POSTs directly to the device's REST API) silently fails on the deployed site. The current workaround is a **pull/handoff**: the installer hands you a `?deviceModel=` URL, and the *device's own page* fetches the catalog from Pages and applies it — but that only runs if the user opens that exact link, which is easy to miss. The `deviceModel` *name* already arrives fine because it's pushed over **Improv (serial)**, which bypasses the network entirely.

**The fix (product-owner decision).** Generalise that: **"Improv = the REST API over serial."** During provisioning the installer holds the USB serial port, so push the *whole* configuration over serial as a sequence of REST-equivalent operations — no HTTP, no mixed-content, no pull, no user click. It works for WiFi *and* Ethernet devices (serial exists regardless of network) and removes the mixed-content special-casing.

**What "REST over serial" means concretely.** An `APPLY_OP` Improv frame is just the **serial envelope** (the same wire framing `SET_DEVICE_MODEL` already uses: magic/type/length/checksum) carrying a **REST operation as its payload** — literally `{"op":"add","type":...,"id":...,"parent":...}` or `{"op":"set","module":...,"control":...,"value":...}`, the **same JSON an HTTP `POST /api/modules` / `/api/control` body carries**. On the device the op routes to the **exact same apply-core** the HTTP handler calls, so a REST call over the network and an `APPLY_OP` over serial **execute identical code**. The new command byte `0xFC` exists only because `0xFE` is hardcoded to "payload = a device-model name string"; `0xFC` means "payload = a REST op." Wire model (decided): **primitive ops** — one op per frame (`add` / `set` / `clear-children`). The device needs **no catalog knowledge** (the installer owns catalog semantics, as it does today in JS).

**The handoff/pull is removed entirely** (product-owner decision). Serial push covers the install; **MoonDeck** covers configuring an already-running device (it talks plain HTTP on the LAN — no mixed-content — so it keeps the direct REST API); **re-flash** covers the rest. So this change *deletes* the device-side catalog self-fetch and the whole `?deviceModel=` machinery — a large subtraction, and the device firmware no longer reaches out to Pages at all (a domain-neutrality + security win).

## Approach (verified against the code, via the Explore pass)

Three confirmed seams make this a wiring job, not new infrastructure:

1. **Apply-core extraction (core).** `HttpServerModule::handleAddModule` (HttpServerModule.cpp:591) and `handleSetControl` (:447) are JSON-body-driven and only touch the `TcpConnection` to call `sendResponse`. Extract the apply core of each into transport-free methods that return an `ApplyResult` (the enum already exists, Control.h:377):
   - `ApplyResult applyAddModule(const char* type, const char* id, const char* parentId)` — lines 619–671: id-uniqueness + single-instance-skip (idempotent), resolve parent, `ModuleFactory::create`, `setName`, `addChild`, `ensureUniqueName`, `onBuildControls`/`setup`/`onBuildState`, `buildState`, `noteDirty`.
   - `ApplyResult applySetControl(const char* module, const char* control, const char* valueJson)` — lines 455–511: find module, the `enabled` fast-path, `applyControlValue`, `rebuildControls`, `onUpdate`, `noteDirty`, conditional `buildState`. (`applyControlValue` reads the value out of a JSON body by key, Control.cpp:203 — the serial path hands it a tiny `{"value":...}` string, same as HTTP.)
   - `bool applyClearChildren(const char* parentName)` — the enumerate-then-delete the handoff's `clearModuleChildren` does, for `replaceChildren`.
   The HTTP handlers become thin wrappers: parse body → call the apply-core → map `ApplyResult` to `sendResponse`. Net: no behaviour change for HTTP; the logic now has one home both transports share (the duplication win, per CLAUDE.md minimalism — not a line saving).

2. **Serial transport = a new Improv vendor RPC carrying one op (platform + a tiny core seam).** Mirror the existing `SET_DEVICE_MODEL` (0xFE) / `SET_TX_POWER` (0xFD) vendor RPCs: add **`APPLY_OP` (0xFC)**. Payload = a compact op the device parses and routes:
   - Frame fits the `kImprovMaxPayload = 128` budget (ImprovFrame.h:31). Most ops fit one frame. A rare long value (a big `pins` list) chunks across frames into a small reassembly buffer (the SET_DEVICE_MODEL fixed-buffer + atomic-ready pattern, generalised).
   - **Producer/consumer across the task boundary** (the established pattern): the Improv task (platform_esp32_improv.cpp) writes the received op into a module-owned buffer + sets an atomic `opReady`; `ImprovProvisioningModule::loop` (ImprovProvisioningModule.h:96) polls it (exactly like `pendingDeviceModelReady_`) and calls the apply-core on the **main loop** (so the factory/tree mutation isn't on the Improv task). The device acks each op with an empty `RpcResponse` (like SET_DEVICE_MODEL), so the installer can pace/await.
   - Op encoding: a tiny JSON object (`{"op":"add","type":"...","id":"...","parent":"..."}` etc.), parsed with the flat `JsonUtil` helpers (`parseString`/`parseInt`) the rest of core already uses. (JSON, not a bespoke binary TLV: same shape as the REST body, host-testable, recognisable.)

3. **Installer: push ops over serial, delete the HTTP/handoff paths (JS).** `tryHttpInjectBoard` (install-orchestrator.js:384) already walks a catalog entry (replaceChildren pre-pass, then per-module add + per-control set). **Repurpose that walk** to emit ops instead of HTTP: `sendConfigOverSerial(port, board)` walks the same entry and, per unit, sends an `APPLY_OP` frame via the existing `buildImprovFrame` + `port.writable.getWriter()` send (mirror `sendSetBoardFrame`, :176), awaiting each ack so order is preserved. The provision flow (`start()`) calls it **right after `SET_DEVICE_MODEL`, while it still owns the port**. Then **delete** the HTTP fan-out (`tryHttpInjectBoard`'s HTTP version, `canFetchHttp`) and the whole handoff (`pendingBoardPush`, the `?deviceModel=` link decoration, the auto-open + "Open device & apply defaults" button).

## Files

**Core (apply-core extraction + the op seam):**
1. **Edit** `src/core/HttpServerModule.h` + `.cpp` — add `applyAddModule` / `applySetControl` / `applyClearChildren` (transport-free, return `ApplyResult`/bool); refactor `handleAddModule` / `handleSetControl` / the delete-children path into thin wrappers calling them. Add `applyOp(const char* opJson)` that parses `op` and dispatches to the three.
2. **Edit** `src/core/ImprovProvisioningModule.h` — add the `pendingOp_` buffer + `pendingOpReady_` atomic (mirror `pendingDeviceModel_`), wire it to platform init, and in `loop()` poll-and-apply by calling the HttpServerModule apply-core (add an applier handle the same way it holds `scheduler_`/`systemModule_`).
3. **Edit** `src/platform/platform.h` + `platform_esp32_improv.cpp` — `improvProvisioningInit` gains `opOut`/`opOutLen`/`opReady` (+ reassembly state); add `IMPROV_CMD_APPLY_OP = 0xFC`, `improvHandleApplyOp` (validate, reassemble if chunked, copy to `opOut`, set `opReady`, ack), and a dispatch branch in `improvDispatchFrame`. Desktop stub (`platform_desktop.cpp`) gains the extra params (no-op).

**Installer (push over serial; delete HTTP-push + handoff):**
4. **Edit** `docs/install/install-orchestrator.js` — add `IMPROV_CMD_APPLY_OP = 0xFC`, `encodeApplyOp(op)` + chunker, `sendApplyOpFrame(port, op)` (mirror `sendSetBoardFrame`), `sendConfigOverSerial(port, board)` (walk the entry → ops), call it in `start()` after `SET_DEVICE_MODEL`. **Delete** `tryHttpInjectBoard`'s HTTP version, `clearDeviceChildren`, `deviceFetch`, `canFetchHttp`, the `httpBoardOk`/`pendingBoardPush` plumbing through `onSuccess`.
5. **Edit** `docs/install/index.html` — success screen becomes simple: "Applied {board} defaults." (or "Kept existing config" when unticked). **Delete** the auto-open, the `done-apply` button + its CSS, the `?deviceModel=` link decoration / `withParam` / `pendingBoardPush` logic in `handleSuccess`.
6. **Edit** `src/ui/app.js` — **delete** `consumePendingDeviceModelParam`, `clearModuleChildren`, `DEVICE_MODELS_JSON_URL`, and the call site (~:158). The device no longer fetches the catalog or interprets `?deviceModel=`. The big firmware subtraction.

**Tests + docs:**
7. **New** `test/unit/core/unit_HttpServerModule_apply.cpp` + register in `test/CMakeLists.txt` — host-test the extracted apply-core directly (no HTTP): `applyAddModule` adds + dedups + single-instance-skips; `applySetControl` writes + range-rejects; `applyClearChildren` empties a container; `applyOp` routes each op type; malformed op → graceful error.
8. **Edit** `docs/moonmodules/core/ImprovProvisioningModule.md` + `SystemModule.md` + `docs/install/README.md` — document the `APPLY_OP` (0xFC) wire op as "a REST operation in an Improv frame, applied by the same core as `/api/modules` + `/api/control`", and the "config pushed over serial during provisioning" flow. **Remove** the handoff / `?deviceModel=` docs. Update `docs/architecture.md` installer + live-reconfig sections with the "Improv = REST over serial" framing and the deletion of the device-side catalog-fetch.

## Verification

- **Host:** `cmake --build build` (0 warnings), `ctest` (incl. the new apply-core test), `uv run scripts/scenario/run_scenario.py`, `check_specs.py`, `check_platform_boundary.py`, `check_devices.py`.
- **ESP32 build** (`build_esp32.py --firmware esp32s3-n16r8` + `esp32` classic) — compiles the new vendor RPC under `-Werror`.
- **Serial APPLY_OP probe** — send a hand-built `APPLY_OP` frame to a connected S3, confirm the op applies (e.g. set Grid width 8) + the device acks. Pins the wire contract without the browser.
- **Real install (the actual fix):** from the local preview, flash the S3 with erase + apply-defaults; confirm it comes up as 8×8 + AudioSpectrum + RandomMap with the serial monitor showing the ops applied, **no handoff link involved**. Repeat on the P4. Confirm no duplicate AudioModule. Serial push is now the *only* install-time path, so preview and deployed behave identically — the HTTPS-vs-HTTP difference that caused the original bug no longer exists.

## Risks / notes

- **Scope:** ~150–250 lines C++ (apply-core extraction is mostly moving existing lines) + ~40–60 lines JS + a no-op desktop-stub param bump. Bounded — every primitive already exists.
- **Net likely a line reduction, definitely a duplication reduction.** The handoff deletion removes a substantial chunk; the new serial op path is small (reuses the apply-core + the entry-walk).
- **Reassembly only for rare long ops.** Common path single-frame; cap the reassembly buffer and fail-safe (drop + error ack) on overflow.
- **Apply on the main loop, not the Improv task** (producer/consumer atomic) — same discipline `pendingDeviceModelReady_` follows.
- **Eth-only / no-Improv-at-boot:** out of scope here. With the handoff removed, an eth-only device's catalog defaults are applied via MoonDeck (direct REST on LAN) until the eth-Improv-listener lands. (Backlog: "Improv listener on eth-only boot" → then serial push is universal.)
- **Ordering:** ops apply in entry order (clear-children before adds; add before its controls); preserved by sequential send + per-op ack.
