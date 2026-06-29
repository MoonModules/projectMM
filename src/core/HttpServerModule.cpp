// HttpServerModule implementation. Public surface and class layout live in
// HttpServerModule.h. Per the project policy in CLAUDE.md, core service modules
// that bridge to the platform (HTTP server, WebSocket framing, JSON state push)
// split into .h + .cpp so implementation edits don't cascade-recompile every TU
// that includes the header.

#include "core/HttpServerModule.h"

#include "core/Scheduler.h"
#include "core/ModuleFactory.h"
#include "core/JsonUtil.h"
#include "core/JsonSink.h"
#include "core/Sha1.h"
#include "core/Base64.h"
#include "core/FilesystemModule.h"
#include "core/FirmwareUpdateModule.h"
#include "core/SystemModule.h"      // deviceName() for the WLED /json/info shim
#include "platform/platform.h"
#include "ui/ui_embedded.h"

#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace mm {

void HttpServerModule::onBuildControls() {
    controls_.addUint16("port", port);
}

void HttpServerModule::setup() {
    if (!server_.open(port)) {
        std::printf("HTTP server failed to open port %u\n", port);
    }
}

void HttpServerModule::teardown() {
    previewSend_.active = false;   // drop any in-flight send before the clients go (body is borrowed)
    for (auto& ws : wsClients_) ws.close();
    server_.close();
}

void HttpServerModule::loop20ms() {
    // Drain the in-flight resumable preview frame on the TRANSPORT-poll cadence (20 ms), NOT the
    // per-render-tick loop(): pushing frame bytes to the socket must not be charged to the LED
    // render hot path. The render tick stays free of preview work; the preview frame rate is
    // bounded by this 20 ms drain cadence (a few fps at large full-res frames) — an acceptable
    // trade, since the preview is a *view* and the LEDs are not. This drain is the consumer-side
    // transport step, kept as a standalone call so it sits cleanly on the render/transport seam
    // (architecture.md § Parallelism). Drain BEFORE accept so a connection burst can't starve an
    // active send. No-op when nothing is in flight.
    drainPreviewSend();
    // Read any inbound WS frames: the native WLED app SETS state (its on/off + brightness
    // slider) by SENDING a {on,bri} text frame over /ws, not by HTTP POST — so we must read
    // the socket, not only push to it. Cheap (non-blocking, usually nothing pending).
    pollWledStateFromWebSockets();
    // Accept one HTTP connection per tick.
    auto conn = server_.accept();
    if (conn.valid()) handleConnection(conn);
}

void HttpServerModule::loop1s() {
    pushStateToWebSockets();
}

void HttpServerModule::handleConnection(platform::TcpConnection& conn) {
    uint8_t buf[2048];
    int totalRead = 0;

    // Read the request. read() is non-blocking (-1 = nothing pending yet), so the render
    // loop is never stalled waiting for bytes (a blocking socket timeout used to freeze the
    // whole loop). A just-accepted connection's request normally lands in the same read; if
    // not, allow a SHORT bounded wait (≤ ~5 ms total) for it, then bail — an idle/half-open
    // connection costs at most that, and the steady-state (nothing pending) costs ~0.
    for (int empties = 0; totalRead < static_cast<int>(sizeof(buf) - 1);) {
        int n = conn.read(buf + totalRead, sizeof(buf) - 1 - totalRead);
        if (n > 0) {
            totalRead += n;
            buf[totalRead] = 0;
            if (std::strstr(reinterpret_cast<char*>(buf), "\r\n\r\n")) break;
            empties = 0;                 // got data — reset the patience counter
        } else if (n == 0) {
            return;                      // peer closed
        } else {                          // -1 = nothing pending yet
            if (totalRead > 0) break;    // had a partial then nothing more — process it
            if (++empties > 5) break;    // fresh conn, no bytes after ~5 ms — give up
            platform::delayMs(1);
        }
    }

    if (totalRead == 0) { conn.close(); return; }
    buf[totalRead] = 0;
    auto* req = reinterpret_cast<char*>(buf);

    // If headers arrived but the body is still in flight, read the rest. read() is
    // non-blocking (-1 = nothing pending yet), so the body can land a TCP segment after the
    // headers — wait briefly between empty reads (the same bounded retry as the header
    // phase) instead of breaking on the first -1, which would route a TRUNCATED body into
    // the permissive JSON helpers (a silent partial control write). If the full declared
    // body still hasn't arrived within the budget, reject with 400 rather than process it.
    auto* headerEnd = std::strstr(req, "\r\n\r\n");
    if (headerEnd) {
        auto* clh = std::strstr(req, "Content-Length:");
        if (clh) {
            int contentLen = std::atoi(clh + 15);
            int headerSize = static_cast<int>(headerEnd + 4 - req);
            int bodyNeeded = headerSize + contentLen;
            if (bodyNeeded > static_cast<int>(sizeof(buf) - 1))
                bodyNeeded = static_cast<int>(sizeof(buf) - 1);   // cap to buffer
            for (int empties = 0; totalRead < bodyNeeded;) {
                int n = conn.read(buf + totalRead, sizeof(buf) - 1 - totalRead);
                if (n > 0) { totalRead += n; empties = 0; }
                else if (n == 0) break;                    // peer closed
                else { if (++empties > 50) break; platform::delayMs(1); }  // ~50 ms for the body
            }
            buf[totalRead] = 0;
            if (totalRead < bodyNeeded) {                  // body never fully arrived
                sendResponse(conn, 400, "application/json",
                             "{\"error\":\"incomplete request body\"}");
                return;
            }
        }
    }

    // Parse method and path
    char method[8] = {};
    char path[128] = {};
    std::sscanf(req, "%7s %127s", method, path);
    // Strip any query string before route matching — every strcmp() below
    // expects a bare path. RFC 3986 §3.4: the query starts at the first '?'
    // and is not part of the path. Browsers send `/?foo=bar` for query-on-
    // root; without this split the GET / route falls through to 404. The web
    // installer's Inject button hits us as `/?deviceModel=<name>` to hand off the
    // deviceModels.json entry — see docs/moonmodules/core/SystemModule.md.
    char* queryStart = std::strchr(path, '?');
    if (queryStart) *queryStart = 0;

    // Check for WebSocket upgrade (case-insensitive header check)
    if (std::strcmp(method, "GET") == 0 && std::strcmp(path, "/ws") == 0 &&
        (std::strstr(req, "Upgrade: websocket") || std::strstr(req, "upgrade: websocket") ||
         std::strstr(req, "Upgrade: WebSocket"))) {
        handleWebSocketUpgrade(conn, req);
        return; // don't close — connection is now a WebSocket
    }

    // Read POST body if present
    // Body pointer (headerEnd already found above)
    char* body = headerEnd ? const_cast<char*>(headerEnd) + 4 : nullptr;

    // Route
    if (std::strcmp(method, "GET") == 0) {
        if (std::strcmp(path, "/") == 0) serveFile(conn, "index.html", "text/html");
        else if (std::strcmp(path, "/app.js") == 0) serveFile(conn, "app.js", "application/javascript");
        else if (std::strcmp(path, "/install-picker.js") == 0) serveFile(conn, "install-picker.js", "application/javascript");
        else if (std::strcmp(path, "/semver.js") == 0) serveFile(conn, "semver.js", "application/javascript");
        else if (std::strcmp(path, "/preview3d.js") == 0) serveFile(conn, "preview3d.js", "application/javascript");
        else if (std::strcmp(path, "/style.css") == 0) serveFile(conn, "style.css", "text/css");
        else if (std::strcmp(path, "/moonlight-logo.png") == 0) serveFile(conn, "moonlight-logo.png", "image/png");
        else if (std::strcmp(path, "/api/state") == 0) serveState(conn);
        else if (std::strcmp(path, "/api/system") == 0) serveSystem(conn);
        else if (std::strcmp(path, "/api/types") == 0) serveTypes(conn);
        // WLED-compatibility shim: the native WLED apps (and Home Assistant's WLED
        // integration) discover a device via mDNS `_wled._tcp` then VALIDATE it by
        // GETting /json/info and checking it's WLED-shaped. Serving a minimal
        // WLED-compatible info makes a projectMM device appear in those apps — and is a
        // useful independent cross-check that our mDNS advertise resolves.
        else if (std::strcmp(path, "/json/info") == 0) serveWledInfo(conn);
        // WLED state + the combined state+info (`/json/si`) the app reads for its device
        // card: on/off, brightness, and the segment's primary colour (which the app uses
        // as the card tint). serveWledState reads live brightness from the Drivers module.
        else if (std::strcmp(path, "/json/state") == 0) serveWledState(conn);
        else if (std::strcmp(path, "/json/si") == 0) serveWledStateInfo(conn);
        else sendResponse(conn, 404, "text/plain", "Not found");
    } else if (std::strcmp(method, "POST") == 0) {
        // POST /api/modules/<name>/move with body {"to":N}.
        // Strict-suffix check: path must end with "/move" exactly (rejects "/movex").
        const size_t pathLen = std::strlen(path);
        const bool isMoveRoute =
            std::strncmp(path, "/api/modules/", 13) == 0 &&
            pathLen > 18 &&
            std::strcmp(path + pathLen - 5, "/move") == 0;
        // POST /api/modules/<name>/replace with body {"type":"<TypeName>"}.
        // Strict-suffix check, same as the move route.
        const bool isReplaceRoute =
            std::strncmp(path, "/api/modules/", 13) == 0 &&
            pathLen > 21 &&
            std::strcmp(path + pathLen - 8, "/replace") == 0;
        if (std::strcmp(path, "/api/control") == 0 && body) {
            handleSetControl(conn, body);
        } else if (std::strcmp(path, "/api/modules") == 0 && body) {
            handleAddModule(conn, body);
        } else if (isMoveRoute && body) {
            char nameBuf[32] = {};
            size_t nameLen = pathLen - 13 - 5;  // strip "/api/modules/" prefix and "/move" suffix
            // Reject rather than truncate — a truncated name could match a
            // different module than the client intended.
            if (nameLen >= sizeof(nameBuf)) {
                sendResponse(conn, 400, "application/json", "{\"error\":\"module name too long\"}");
            } else {
                std::memcpy(nameBuf, path + 13, nameLen);
                nameBuf[nameLen] = 0;
                handleMoveModule(conn, nameBuf, body);
            }
        } else if (isReplaceRoute && body) {
            char nameBuf[32] = {};
            size_t nameLen = pathLen - 13 - 8;  // strip "/api/modules/" prefix and "/replace" suffix
            if (nameLen >= sizeof(nameBuf)) {
                sendResponse(conn, 400, "application/json", "{\"error\":\"module name too long\"}");
            } else {
                std::memcpy(nameBuf, path + 13, nameLen);
                nameBuf[nameLen] = 0;
                handleReplaceModule(conn, nameBuf, body);
            }
        } else if (std::strcmp(path, "/json/state") == 0 && body) {
            // WLED-compatibility: the native WLED app POSTs {on,bri,…} here to control the
            // device. We map it onto the Drivers brightness control so the app's on/off +
            // brightness slider drive the real output.
            handleWledState(conn, body);
        } else if (std::strcmp(path, "/api/reboot") == 0) {
            handleReboot(conn);
        } else if (std::strcmp(path, "/api/firmware/url") == 0 && body) {
            handleFirmwareUrl(conn, body);
        } else {
            sendResponse(conn, 404, "text/plain", "Not found");
        }
    } else if (std::strcmp(method, "DELETE") == 0) {
        // DELETE /api/modules/ModuleName
        if (std::strncmp(path, "/api/modules/", 13) == 0) {
            handleDeleteModule(conn, path + 13);
        } else {
            sendResponse(conn, 404, "text/plain", "Not found");
        }
    } else if (std::strcmp(method, "OPTIONS") == 0) {
        // CORS preflight. The browser sends OPTIONS before any cross-origin
        // POST with a non-simple Content-Type (e.g. application/json), which
        // covers every /api/control and /api/modules write the web installer
        // makes from preview / localhost. Without this branch the dispatcher
        // fell through to 405 Method Not Allowed and the browser silently
        // blocked the subsequent POST. The response carries the same
        // Access-Control-Allow-Origin: * the actual response already does,
        // plus the methods + headers we accept on the API surface. 204 (no
        // body) is the conventional preflight reply.
        //
        // Path-agnostic: we return 204 for OPTIONS to ANY path, even ones
        // that would 404 on a real GET/POST. Most public servers narrow
        // preflight to known API routes; we don't bother because the
        // device's HTTP surface is tiny and lives behind the user's LAN.
        // A scanner hitting OPTIONS /random gets a CORS-OK 204 rather
        // than a 404 — informational only, no behaviour change.
        sendPreflightResponse(conn);
    } else {
        sendResponse(conn, 405, "text/plain", "Method not allowed");
    }

    conn.close();
}

void HttpServerModule::sendPreflightResponse(platform::TcpConnection& conn) {
    // 204 No Content is the standard preflight success reply. The
    // Access-Control-Allow-* headers tell the browser what cross-origin
    // requests we accept on the API. Max-Age caches the preflight for an
    // hour so subsequent same-session POSTs go straight through.
    const char* response =
        "HTTP/1.1 204 No Content\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Access-Control-Max-Age: 3600\r\n"
        "Connection: close\r\n"
        "\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(response), std::strlen(response));
}

void HttpServerModule::sendResponse(platform::TcpConnection& conn, int status, const char* contentType, const char* body) {
    const char* statusText =
        status == 200 ? "OK" :
        status == 202 ? "Accepted" :
        status == 400 ? "Bad Request" :
        status == 404 ? "Not Found" :
        status == 405 ? "Method Not Allowed" :
        status == 409 ? "Conflict" :
        status == 500 ? "Internal Server Error" :
        status == 501 ? "Not Implemented" :
        "Error";
    char header[256];
    int bodyLen = static_cast<int>(std::strlen(body));
    int headerLen = std::snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        status, statusText, contentType, bodyLen);
    conn.write(reinterpret_cast<const uint8_t*>(header), headerLen);
    conn.write(reinterpret_cast<const uint8_t*>(body), bodyLen);
}

void HttpServerModule::serveFile(platform::TcpConnection& conn, const char* filename, const char* contentType) {
    // Try disk first (desktop development — live editing without rebuild)
    char filepath[256];
    std::snprintf(filepath, sizeof(filepath), "%s/%s", uiPath_, filename);

    FILE* f = std::fopen(filepath, "rb");
    if (f) {
        std::fseek(f, 0, SEEK_END);
        long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);

        char header[256];
        int headerLen = std::snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %ld\r\n"
            "Connection: close\r\n"
            "Cache-Control: no-cache\r\n"
            "\r\n",
            contentType, size);
        conn.write(reinterpret_cast<const uint8_t*>(header), headerLen);

        uint8_t chunk[1024];
        while (size > 0) {
            size_t toRead = size > static_cast<long>(sizeof(chunk)) ? sizeof(chunk) : static_cast<size_t>(size);
            size_t bytesRead = std::fread(chunk, 1, toRead, f);
            if (bytesRead == 0) break;
            conn.write(chunk, bytesRead);
            size -= static_cast<long>(bytesRead);
        }
        std::fclose(f);
        return;
    }

    // Fall back to embedded data (ESP32 or when disk files not found). The text
    // assets are embedded gzipped (see embed_ui.cmake) and served with
    // Content-Encoding: gzip — the browser inflates them. gzipped is false only
    // for already-compressed binaries (the PNG), which are embedded raw.
    const uint8_t* data = nullptr;
    size_t dataLen = 0;
    bool gzipped = false;
    if (std::strcmp(filename, "index.html") == 0) { data = ui::indexHtml; dataLen = ui::indexHtmlLen; gzipped = true; }
    else if (std::strcmp(filename, "app.js") == 0) { data = ui::appJs; dataLen = ui::appJsLen; gzipped = true; }
    else if (std::strcmp(filename, "install-picker.js") == 0) { data = ui::installPickerJs; dataLen = ui::installPickerJsLen; gzipped = true; }
    else if (std::strcmp(filename, "semver.js") == 0) { data = ui::semverJs; dataLen = ui::semverJsLen; gzipped = true; }
    else if (std::strcmp(filename, "preview3d.js") == 0) { data = ui::preview3dJs; dataLen = ui::preview3dJsLen; gzipped = true; }
    else if (std::strcmp(filename, "style.css") == 0) { data = ui::styleCss; dataLen = ui::styleCssLen; gzipped = true; }
    else if (std::strcmp(filename, "moonlight-logo.png") == 0) { data = ui::logoPng; dataLen = ui::logoPngLen; }

    if (!data) {
        sendResponse(conn, 404, "text/plain", "File not found");
        return;
    }

    char header[256];
    int headerLen = std::snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "%s"
        "Connection: close\r\n"
        "Cache-Control: no-cache\r\n"
        "\r\n",
        contentType, dataLen,
        gzipped ? "Content-Encoding: gzip\r\n" : "");
    conn.write(reinterpret_cast<const uint8_t*>(header), headerLen);
    conn.write(data, dataLen);
}

void HttpServerModule::serveState(platform::TcpConnection& conn) {
    const char* header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));

    JsonSink sink(conn);
    buildStateJson(sink);
    sink.flush();
}

void HttpServerModule::buildStateJson(JsonSink& sink) {
    sink.append("{\"modules\":[");

    if (scheduler_) {
        bool first = true;
        for (uint8_t m = 0; m < scheduler_->moduleCount(); m++) {
            auto* mod = scheduler_->module(m);
            if (!mod || mod == this) continue; // skip self
            if (!first) sink.append(",");
            first = false;
            writeModuleJson(sink, mod);
        }
    }

    sink.append("]}");
}

void HttpServerModule::writeModuleJson(JsonSink& sink, MoonModule* mod) {
    // Per-module header: name, role, enabled, loopTimeUs (fps/ms display),
    // classSize (static C++ object bytes) + dynamicBytes (heap), controls
    const char* roleStr = roleName(mod->role());
    const char* type = mod->typeName();
    if (!type) type = "";
    sink.appendf(
        "{\"name\":\"%s\",\"type\":\"%s\",\"role\":\"%s\",\"enabled\":%s,"
        "\"loopTimeUs\":%u,\"classSize\":%u,\"dynamicBytes\":%u",
        mod->name() ? mod->name() : "",
        type,
        roleStr,
        mod->enabled() ? "true" : "false",
        static_cast<unsigned>(mod->loopTimeUs()),
        static_cast<unsigned>(mod->classSize()),
        static_cast<unsigned>(mod->dynamicBytes()));
    writeStatus(sink, mod);
    // userEditable: omit when true (the common case) to save bytes — the UI
    // treats absent as editable, same convention as the control hidden/readonly
    // flags. Emitted only for modules that opt out (e.g. PreviewDriver), so the
    // UI hides their delete/replace affordance.
    if (!mod->userEditable()) sink.append(",\"userEditable\":false");
    sink.append(",\"controls\":[");
    writeControls(sink, mod);
    sink.append("]");

    // Children
    uint8_t cc = mod->childCount();
    if (cc > 0) {
        sink.append(",\"children\":[");
        for (uint8_t i = 0; i < cc; i++) {
            if (i > 0) sink.append(",");
            writeModuleJson(sink, mod->child(i));
        }
        sink.append("]");
    }

    sink.append("}");
}

void HttpServerModule::writeStatus(JsonSink& sink, MoonModule* mod) {
    // Only emit when the module has a status — keeps the common case lean.
    // Severity strings are stable wire format: "status", "warning", "error"
    // (matches the C++ enum names lowercased; documented in HttpServerModule.md).
    const char* s = mod->status();
    if (!s) return;
    static const char* sevStr[] = {"status", "warning", "error"};
    sink.appendf(",\"status\":\"%s\",\"severity\":\"%s\"",
                 s, sevStr[static_cast<int>(mod->severity())]);
}

void HttpServerModule::writeControls(JsonSink& sink, MoonModule* mod) {
    auto& ctrls = mod->controls();
    for (uint8_t i = 0; i < ctrls.count(); i++) {
        if (i > 0) sink.append(",");
        auto& c = ctrls[i];
        // Common wrapper for every control: {"name":...,"type":...,"value":VALUE,EXTRAS,"hidden":?}
        // Per-type VALUE + EXTRAS rendering lives in Control.cpp so the
        // wire format isn't duplicated across HttpServer/FS/scenario.
        // Password is the one exception — its API serialization XOR-obfuscates +
        // base64-encodes (writeControlValue emits plaintext, which is what
        // FilesystemModule's writeValue wants); handle it here in-line so
        // writeControlValue stays sink-neutral.
        sink.appendf("{\"name\":\"%s\",\"type\":\"%s\",\"value\":",
                     c.name, controlTypeName(c.type));
        if (c.type == ControlType::Password) {
            // The password is sent XOR-obfuscated + base64-encoded, NOT
            // in plaintext. This is deliberate obfuscation, not security:
            // the XOR key is a fixed shared constant (also in app.js), so
            // anyone can reverse it. It is a first line of defence — the
            // value is not readable at a glance in `curl /api/state` — and
            // it lets the UI's hold-to-peek reveal the stored password.
            const char* pw = static_cast<char*>(c.ptr);
            uint8_t scrambled[64];
            size_t pwLen = std::strlen(pw);
            if (pwLen > sizeof(scrambled)) pwLen = sizeof(scrambled);
            for (size_t k = 0; k < pwLen; k++) {
                scrambled[k] = static_cast<uint8_t>(pw[k]) ^ PASSWORD_XOR_KEY;
            }
            char encoded[96];
            base64Encode(std::span(scrambled).first(pwLen), std::span(encoded));
            sink.appendf("\"%s\"", encoded);
        } else {
            writeControlValue(sink, c);
        }
        writeControlMetadata(sink, c);
        // Emit optional flags only when set (common case is false; omit to save bytes).
        if (c.readonly) sink.append(",\"readonly\":true");
        sink.append(c.hidden ? ",\"hidden\":true}" : "}");
    }
}

// Apply-core: set one control's value. `valueJson` is a small JSON object holding
// the value under the "value" key ({"value":8}) — the same body the HTTP handler
// receives, so applyControlValue (which reads by key) is reused verbatim. Transport-
// free: no TcpConnection, returns an OpResult the caller maps to its own reporting.
HttpServerModule::OpResult HttpServerModule::applySetControl(
        const char* moduleName, const char* controlName, const char* valueJson) {
    MoonModule* target = findModuleByName(moduleName);
    if (!target) return OpResult::ModuleNotFound;

    // Module-level "enabled" pseudo-control.
    if (std::strcmp(controlName, "enabled") == 0) {
        target->setEnabled(mm::json::parseBool(valueJson, "value"));
        target->markDirty();
        FilesystemModule::noteDirty();
        if (scheduler_) scheduler_->buildState();
        return OpResult::Ok;
    }

    auto& ctrls = target->controls();
    for (uint8_t i = 0; i < ctrls.count(); i++) {
        auto& c = ctrls[i];
        if (std::strcmp(c.name, controlName) != 0) continue;

        // Per-type parse + validate + apply lives in Control.cpp. Non-Ok leaves the
        // storage untouched, so no rollback needed.
        ApplyResult r = applyControlValue(c, valueJson, "value");
        switch (r) {
            case ApplyResult::Ok:        break;
            case ApplyResult::OutOfRange: return OpResult::OutOfRange;
            case ApplyResult::Malformed:  return OpResult::Malformed;
            case ApplyResult::ReadOnly:   return OpResult::ReadOnly;
        }
        // Rebuild the control list after every change so onBuildControls() can
        // re-evaluate which controls are visible for the new value (a Select
        // revealing fields, etc.). clear()+onBuildControls(), cheap + idempotent.
        target->rebuildControls();
        // Three-tier control-change reaction (see MoonModule::onUpdate): onUpdate
        // always; a tree-wide buildState only when the control reshapes dims/mapping.
        target->onUpdate(controlName);
        target->markDirty();
        FilesystemModule::noteDirty();
        if (target->controlChangeTriggersBuildState(controlName) && scheduler_) {
            scheduler_->buildState();
        }
        return OpResult::Ok;
    }
    return OpResult::ControlNotFound;   // control name not on this module
}

void HttpServerModule::handleSetControl(platform::TcpConnection& conn, const char* body) {
    // Parse: {"module":"Noise","control":"scale","value":8} — the apply-core reads
    // the value out of `body` itself (so it sees the exact same JSON the API got).
    char moduleName[32] = {};
    char controlName[32] = {};
    mm::json::parseString(body, "module", moduleName, sizeof(moduleName));
    mm::json::parseString(body, "control", controlName, sizeof(controlName));

    switch (applySetControl(moduleName, controlName, body)) {
        case OpResult::Ok:
            sendResponse(conn, 200, "application/json", "{\"ok\":true}");
            return;
        case OpResult::ModuleNotFound:
            sendResponse(conn, 404, "application/json", "{\"error\":\"module not found\"}");
            return;
        case OpResult::ControlNotFound:
            sendResponse(conn, 404, "application/json", "{\"error\":\"control not found\"}");
            return;
        case OpResult::OutOfRange:
            sendResponse(conn, 400, "application/json", "{\"error\":\"value out of range\"}");
            return;
        case OpResult::Malformed:
            sendResponse(conn, 400, "application/json", "{\"error\":\"value malformed\"}");
            return;
        case OpResult::ReadOnly:
            sendResponse(conn, 400, "application/json", "{\"error\":\"control is read-only\"}");
            return;
        default:
            sendResponse(conn, 400, "application/json", "{\"error\":\"bad request\"}");
            return;
    }
}

MoonModule* HttpServerModule::findModuleByName(const char* name) {
    if (!name || name[0] == 0 || !scheduler_) return nullptr;

    for (uint8_t m = 0; m < scheduler_->moduleCount(); m++) {
        auto* mod = scheduler_->module(m);
        if (!mod) continue;
        auto* found = findInTree(mod, name);
        if (found) return found;
    }
    return nullptr;
}

MoonModule* HttpServerModule::findInTree(MoonModule* mod, const char* name) {
    if (mod->name() && std::strcmp(mod->name(), name) == 0) return mod;
    for (uint8_t i = 0; i < mod->childCount(); i++) {
        auto* found = findInTree(mod->child(i), name);
        if (found) return found;
    }
    return nullptr;
}

void HttpServerModule::serveSystem(platform::TcpConnection& conn) {
    const char* header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));

    JsonSink sink(conn);
    // maxBlock = internal-only (maxInternalAllocBlock) — the all-memory
    // variant reports ~8 MB on PSRAM boards and is meaningless as a
    // pressure signal. Same rationale as main.cpp's tick log line.
    sink.appendf(
        "{\"fps\":%u,\"tickTimeUs\":%u,\"freeHeap\":%u,\"freeInternal\":%u,\"maxBlock\":%u,\"uptime\":%u,\"modules\":[",
        static_cast<unsigned>(scheduler_ ? scheduler_->fps() : 0),
        static_cast<unsigned>(scheduler_ ? scheduler_->tickTimeUs() : 0),
        static_cast<unsigned>(platform::freeHeap()),
        static_cast<unsigned>(platform::freeInternalHeap()),
        static_cast<unsigned>(platform::maxInternalAllocBlock()),
        static_cast<unsigned>(scheduler_ ? scheduler_->elapsed() / 1000 : 0));

    // Per-module timing (walk tree recursively)
    if (scheduler_) {
        bool first = true;
        for (uint8_t i = 0; i < scheduler_->moduleCount(); i++) {
            writeModuleMetricsJson(sink, scheduler_->module(i), first);
        }
    }

    sink.append("]}");
    sink.flush();
}

// WLED-compatibility `/json/info` — the subset of WLED's info object the native WLED
// apps + Home Assistant validate when they probe a device they discovered via
// `_wled._tcp`. The clients gate on a WLED-shaped identity: `brand:"WLED"`, a real
// `vid` (build id; they reject 0), a WLED-major `ver`, and `leds.count`. We declare
// `brand:"WLED"` because the apps key on it — the same thing WLED-MM (the MoonModules
// WLED fork) does — while `product:"MoonModules"` says what this actually is. We speak
// WLED's info shape to interoperate, not to impersonate. Built fresh against WLED's
// public JSON, not copied. (Reference real WLED carries far more; this is the trimmed,
// known-sufficient field set — see docs/moonmodules/core/HttpServerModule.md.)
void HttpServerModule::serveWledInfo(platform::TcpConnection& conn) {
    const char* header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));

    // Identity: the deviceName (from SystemModule), the live IP, the MAC.
    const char* name = "projectMM";
    if (MoonModule* sys = findModuleByName("System")) {
        const char* dn = static_cast<SystemModule*>(sys)->deviceName();
        if (dn && dn[0]) name = dn;
    }
    uint8_t ip[4] = {};
    platform::ethGetIPv4(ip);
    if (!ip[0] && !ip[1] && !ip[2] && !ip[3]) platform::wifiStaGetIPv4(ip);
    uint8_t mac[6] = {};
    platform::getMacAddress(mac);

    // Field set reverse-engineered from the WLED-Android app's `Info` Moshi model
    // (model/wledapi/Info.kt): the ONLY non-nullable fields it requires are `name`, `leds`
    // (object), and `wifi` (object) — a missing one fails the JSON parse and the device is
    // silently dropped. `DeviceFirstContactService.kt` additionally rejects a device whose
    // body `mac` is empty. Every other field in the model is nullable. So this is the
    // minimal object the native app accepts: name + leds{} + wifi{} + a non-empty mac. The
    // inner Leds/Wifi fields are themselves all nullable, so empty `{}` objects parse — we
    // send a real `mac` and otherwise the smallest shapes that satisfy the parser. `brand`/
    // `product` identify us as the MoonModules WLED-compatible product (interoperate, not
    // impersonate). Confirmed on the bench: projectMM devices list in the WLED native app.
    JsonSink sink(conn);
    writeWledInfoBody(sink, name, mac);
    sink.flush();
}

// The WLED info object, written into an open sink (no HTTP header). Shared by
// /json/info and the `info` half of /json/si.
void HttpServerModule::writeWledInfoBody(JsonSink& sink, const char* name, const uint8_t mac[6]) {
    sink.appendf("{\"name\":");
    sink.writeJsonString(name);   // writes its own surrounding quotes + escaping
    // wifi: rssi/signal are nullable in the model, but sending them makes the app show a
    // signal icon instead of a crossed-out one (cosmetic — the device lists either way).
    sink.appendf(",\"mac\":\"%02x%02x%02x%02x%02x%02x\","
                 "\"leds\":{\"count\":1},\"wifi\":{\"rssi\":-50,\"signal\":100},"
                 "\"brand\":\"WLED\",\"product\":\"MoonModules\"}",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Current Drivers brightness (0–255), or 0 if Drivers isn't present. This is the global
// output brightness the WLED app's slider maps to. Read generically through the control
// list (by name + Uint8 type, via the stored pointer) so this core module needs no
// light-domain include — the same domain-neutral reach applySetControl uses to write.
uint8_t HttpServerModule::driversBrightness() {
    MoonModule* d = findModuleByName("Drivers");
    if (!d) return 0;
    const ControlList& cl = d->controls();
    for (uint8_t i = 0; i < cl.count(); i++) {
        const ControlDescriptor& c = cl[i];
        if (c.ptr && c.type == ControlType::Uint8 && std::strcmp(c.name, "brightness") == 0)
            return *static_cast<const uint8_t*>(c.ptr);
    }
    return 0;
}

// The WLED state object, written into an open sink. `on` + `bri` mirror Drivers
// brightness (off = brightness 0). `seg[0].col[0]` is the colour the WLED app tints the
// device card with: we send the LIVE first-LED RGB from Drivers, so the card mirrors what
// the device is actually showing — falling back to projectMM purple when the first LED is
// black/off or there's no output (so a dark device still reads as a distinct projectMM,
// not an indistinct black card).
void HttpServerModule::writeWledStateBody(JsonSink& sink) {
    const uint8_t bri = driversBrightness();
    uint8_t rgb[3] = {0, 0, 0};
    bool haveLed = false;
    if (MoonModule* d = findModuleByName("Drivers")) haveLed = d->firstOutputRgb(rgb);
    if (!haveLed || (rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 0)) {
        rgb[0] = 128; rgb[1] = 0; rgb[2] = 255;   // projectMM purple — the black/off default
    }
    sink.appendf("{\"on\":%s,\"bri\":%u,"
                 "\"seg\":[{\"id\":0,\"col\":[[%u,%u,%u]]}]}",
                 bri > 0 ? "true" : "false", bri, rgb[0], rgb[1], rgb[2]);
}

void HttpServerModule::serveWledState(platform::TcpConnection& conn) {
    const char* header =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));
    JsonSink sink(conn);
    writeWledStateBody(sink);
    sink.flush();
}

// /json/si — the combined {state, info} the WLED app reads in one call for its card.
void HttpServerModule::serveWledStateInfo(platform::TcpConnection& conn) {
    const char* header =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));

    const char* name = "projectMM";
    if (MoonModule* sys = findModuleByName("System")) {
        const char* dn = static_cast<SystemModule*>(sys)->deviceName();
        if (dn && dn[0]) name = dn;
    }
    uint8_t mac[6] = {};
    platform::getMacAddress(mac);

    JsonSink sink(conn);
    sink.appendf("{\"state\":");
    writeWledStateBody(sink);
    sink.appendf(",\"info\":");
    writeWledInfoBody(sink, name, mac);
    sink.appendf("}");
    sink.flush();
}

// Apply a WLED state-set body ({on?, bri?}) to the Drivers brightness control through the
// shared apply-core (the same path /api/control and Improv APPLY_OP use). `on:false` → 0;
// `on:true` with no `bri` → restore a visible default; `bri:N` → set N. Shared by the HTTP
// POST /json/state handler and the inbound-WebSocket path (the app uses both channels).
void HttpServerModule::applyWledState(const char* body) {
    int bri = -1;
    if (mm::json::hasKey(body, "bri")) bri = mm::json::parseInt(body, "bri");
    if (mm::json::hasKey(body, "on")) {
        const bool on = mm::json::parseBool(body, "on");
        if (!on) bri = 0;
        else if (bri < 0) bri = driversBrightness() > 0 ? -1 : 128;  // turn on → visible default
    }
    if (bri >= 0) {
        if (bri > 255) bri = 255;
        char valueJson[32];
        std::snprintf(valueJson, sizeof(valueJson), "{\"value\":%d}", bri);
        applySetControl("Drivers", "brightness", valueJson);
    }
}

// POST /json/state — the WLED app's HTTP control channel (its system quick-tiles + Home
// Assistant). Apply, then echo the resulting state (the app expects a State response).
void HttpServerModule::handleWledState(platform::TcpConnection& conn, const char* body) {
    applyWledState(body);
    serveWledState(conn);
}

void HttpServerModule::writeModuleMetricsJson(JsonSink& sink, MoonModule* mod, bool& first) {
    if (!mod) return;
    sink.appendf(
        "%s{\"name\":\"%s\",\"us\":%u,\"classSize\":%u,\"heap\":%u",
        first ? "" : ",",
        mod->name() ? mod->name() : "?",
        static_cast<unsigned>(mod->loopTimeUs()),
        static_cast<unsigned>(mod->classSize()),
        static_cast<unsigned>(mod->dynamicBytes()));
    writeStatus(sink, mod);
    sink.append("}");
    first = false;
    for (uint8_t i = 0; i < mod->childCount(); i++) {
        writeModuleMetricsJson(sink, mod->child(i), first);
    }
}

// Apply-core: add one module under a named parent. Transport-free; returns an
// OpResult. Idempotent on the id (an existing name returns Ok, "already there").
HttpServerModule::OpResult HttpServerModule::applyAddModule(
        const char* typeName, const char* id, const char* parentId) {
    if (!typeName || typeName[0] == 0) return OpResult::BadRequest;

    // Top-level modules (Layouts/Layers/Drivers/Filesystem/System/Network/HttpServer)
    // are policy-fixed and wired in main.cpp at boot. Only *child* adds are allowed —
    // anything else would orphan the module (never ticked, leaked).
    if (!parentId || parentId[0] == 0) return OpResult::BadRequest;

    // Idempotent: an existing module with this name is success, not an error — so a
    // re-run of the catalog inject (or a double APPLY_OP) is a no-op, not a dup. The
    // distinct AlreadyExists (vs Ok) lets the HTTP handler report "already exists" so a
    // client can tell created-now from already-there; both are success.
    if (id && id[0] != 0 && findModuleByName(id)) return OpResult::AlreadyExists;

    // Resolve the parent before allocating — failure means we never make an orphan.
    auto* parent = findModuleByName(parentId);
    if (!parent) return OpResult::ModuleNotFound;

    auto* mod = ModuleFactory::create(typeName);
    if (!mod) return OpResult::UnknownType;
    if (id && id[0] != 0) mod->setName(id);

    if (!parent->addChild(mod)) {
        delete mod;
        return OpResult::BadRequest;   // parent rejected the child
    }

    // Disambiguate a colliding name (a second "Layer" etc.) — same pass the Scheduler
    // runs after persistence load; single source of truth.
    if (scheduler_) scheduler_->ensureUniqueName(mod);

    // Lifecycle in Scheduler::setup() order: onBuildControls() (bind buffers) →
    // setup() (may read them) → onBuildState().
    mod->onBuildControls();
    mod->setup();
    mod->onBuildState();
    if (scheduler_) scheduler_->buildState();

    // Persist the new tree shape (debounced save via noteDirty).
    parent->markDirty();
    FilesystemModule::noteDirty();
    return OpResult::Ok;
}

void HttpServerModule::handleAddModule(platform::TcpConnection& conn, const char* body) {
    char typeName[32] = {};
    char id[32] = {};
    char parentId[32] = {};
    mm::json::parseString(body, "type", typeName, sizeof(typeName));
    mm::json::parseString(body, "id", id, sizeof(id));
    mm::json::parseString(body, "parent_id", parentId, sizeof(parentId));

    switch (applyAddModule(typeName, id, parentId)) {
        case OpResult::Ok:
            sendResponse(conn, 200, "application/json", "{\"ok\":true}");
            return;
        case OpResult::AlreadyExists:
            sendResponse(conn, 200, "application/json", "{\"ok\":true,\"note\":\"already exists\"}");
            return;
        case OpResult::ModuleNotFound:
            sendResponse(conn, 404, "application/json", "{\"error\":\"parent not found\"}");
            return;
        case OpResult::UnknownType:
            sendResponse(conn, 400, "application/json", "{\"error\":\"unknown type\"}");
            return;
        case OpResult::BadRequest:
        default:
            sendResponse(conn, 400, "application/json",
                         "{\"error\":\"missing type, or parent_id required (top-level modules are policy-fixed in main.cpp), or parent rejected child\"}");
            return;
    }
}

// Apply-core: DELETE every user-editable child of `parentName` (the catalog
// inject's replaceChildren — an entry's effects replace the boot defaults instead
// of stacking). Same removeChild → teardown → deleteTree the HTTP delete does.
// Code-wired children (Preview, Improv) are left in place; they aren't what a
// catalog entry replaces. Transport-free.
HttpServerModule::OpResult HttpServerModule::applyClearChildren(const char* parentName) {
    auto* parent = findModuleByName(parentName);
    if (!parent) return OpResult::ModuleNotFound;
    bool removedAny = false;
    // Iterate from the end: removeChild compacts the array, so back-to-front keeps
    // indices valid as we delete.
    for (int i = static_cast<int>(parent->childCount()) - 1; i >= 0; i--) {
        auto* c = parent->child(static_cast<uint8_t>(i));
        if (!c || !c->userEditable()) continue;
        parent->removeChild(c);
        c->teardown();
        Scheduler::deleteTree(c);
        removedAny = true;
    }
    if (removedAny) {
        if (scheduler_) scheduler_->buildState();
        parent->markDirty();
        FilesystemModule::noteDirty();
    }
    return OpResult::Ok;
}

// Apply-core dispatcher: one REST op as a JSON object. This is the wire shape the
// Improv APPLY_OP frame carries — "REST over serial". The op is a small flat object:
//   {"op":"add","type":"...","id":"...","parent":"..."}
//   {"op":"set","module":"...","control":"...","value":...}
//   {"op":"clearChildren","parent":"..."}
// For "set" the whole op JSON is handed to applySetControl, which reads "value" by
// key — the same way the HTTP /api/control handler reads it from the request body,
// so any value type rides through unchanged.
// The wire shape the Improv APPLY_OP frame carries. NOTE the serial op's add uses the
// key "parent", while the HTTP POST /api/modules body uses "parent_id" for the same
// field — both feed the one applyAddModule() core, but the two transports parse different
// JSON keys, so an HTTP payload is NOT a drop-in APPLY_OP (rename parent_id → parent). The
// serial op stays terse because every byte counts against the 128-byte frame budget; the
// discrepancy is documented in docs/moonmodules/core/ImprovProvisioningModule.md.
HttpServerModule::OpResult HttpServerModule::applyOp(const char* opJson) {
    if (!opJson) return OpResult::BadRequest;
    char op[16] = {};
    mm::json::parseString(opJson, "op", op, sizeof(op));
    if (std::strcmp(op, "add") == 0) {
        char type[32] = {}, id[32] = {}, parent[32] = {};
        mm::json::parseString(opJson, "type", type, sizeof(type));
        mm::json::parseString(opJson, "id", id, sizeof(id));
        mm::json::parseString(opJson, "parent", parent, sizeof(parent));  // "parent", not HTTP's "parent_id"
        return applyAddModule(type, id, parent);
    }
    if (std::strcmp(op, "set") == 0) {
        char module[32] = {}, control[32] = {};
        mm::json::parseString(opJson, "module", module, sizeof(module));
        mm::json::parseString(opJson, "control", control, sizeof(control));
        return applySetControl(module, control, opJson);
    }
    if (std::strcmp(op, "clearChildren") == 0) {
        char parent[32] = {};
        mm::json::parseString(opJson, "parent", parent, sizeof(parent));
        return applyClearChildren(parent);
    }
    return OpResult::BadRequest;   // unknown op
}

void HttpServerModule::handleDeleteModule(platform::TcpConnection& conn, const char* moduleName) {
    auto* mod = findModuleByName(moduleName);
    if (!mod) {
        sendResponse(conn, 404, "application/json", "{\"error\":\"module not found\"}");
        return;
    }

    // Top-level modules (Layouts/Layers/Drivers/Filesystem/System/Network/HttpServer)
    // have no parent — they're registered via Scheduler::addModule in main.cpp and the
    // top-level shape is policy-fixed. Reject the delete here instead of teardown+delete'ing
    // a module that the scheduler still holds a pointer to (which would dangle on next tick).
    auto* parent = mod->parent();
    if (!parent) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"cannot delete top-level module\"}");
        return;
    }

    // Non-editable submodules (Board, Preview, Improv) are apparatus, not
    // swappable pipeline content — refuse here so the API enforces it, not just
    // the UI's hidden delete button. They can still be disabled via their enable
    // toggle; they just can't be removed from the tree.
    if (!mod->userEditable()) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"module not deletable\"}");
        return;
    }

    // Remove from parent
    parent->removeChild(mod);

    // Tear down + recursively free the whole subtree. A bare `delete mod`
    // here would only free mod's children_ pointer array (MoonModule's
    // destructor calls `delete[] children_`); each child module the array
    // pointed to would leak. Use the same pair handleReplaceModule does.
    mod->teardown();
    Scheduler::deleteTree(mod);

    if (scheduler_) scheduler_->buildState();

    // Persist the new tree shape — marking the parent dirty rewrites its file
    // without the deleted child slot. The parent is guaranteed non-null by the
    // top-of-function check (top-level deletes are rejected as 400).
    parent->markDirty();
    FilesystemModule::noteDirty();

    sendResponse(conn, 200, "application/json", "{\"ok\":true}");
}

void HttpServerModule::handleReplaceModule(platform::TcpConnection& conn, const char* moduleName, const char* body) {
    auto* mod = findModuleByName(moduleName);
    if (!mod) {
        sendResponse(conn, 404, "application/json", "{\"error\":\"module not found\"}");
        return;
    }
    auto* parent = mod->parent();
    if (!parent) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"top-level modules cannot be replaced\"}");
        return;
    }
    // Non-editable submodules (Board, Preview, Improv) are apparatus — replacing
    // one swaps it for a different type, which is as much a removal as a delete.
    // Refuse, mirroring handleDeleteModule's guard, so the editability contract
    // holds across both endpoints.
    if (!mod->userEditable()) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"module not editable\"}");
        return;
    }
    char typeName[32] = {};
    mm::json::parseString(body, "type", typeName, sizeof(typeName));
    if (typeName[0] == 0) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"missing type\"}");
        return;
    }

    // Find the child's index within the parent.
    uint8_t index = 0;
    bool found = false;
    for (uint8_t i = 0; i < parent->childCount(); i++) {
        if (parent->child(i) == mod) { index = i; found = true; break; }
    }
    if (!found) {
        sendResponse(conn, 404, "application/json", "{\"error\":\"module not found\"}");
        return;
    }

    // Create the replacement before touching the tree — if the factory fails,
    // return early and leave the tree intact (never leave a hole).
    auto* fresh = ModuleFactory::create(typeName);
    if (!fresh) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"unknown type\"}");
        return;
    }

    // Name on replace: keep a CUSTOM name (a scenario id like "MOD", or a
    // user-renamed slot) so callers can keep addressing the slot by it. But if
    // the old name was just the old type's factory display name ("Multiply" for
    // a MultiplyModifier), let the fresh module keep its own factory name
    // ("Checkerboard") — otherwise a Multiply→Checkerboard replace leaves a
    // Checkerboard mislabelled "Multiply". `fresh` already arrives with its
    // correct default name from ModuleFactory::create, so we only override for a
    // custom name; then re-run uniqueness so two same-type siblings don't collide.
    const char* oldDefault = ModuleFactory::displayNameFor(mod->typeName(), mod->role());
    if (std::strcmp(mod->name(), oldDefault) != 0) {
        fresh->setName(mod->name());  // custom name — preserve the slot identity
    }

    // Swap in place; replaceChildAt returns the old module, which we own.
    MoonModule* old = parent->replaceChildAt(index, fresh);

    // Lifecycle on the fresh module — same phase order as the add path.
    fresh->onBuildControls();
    fresh->setup();
    fresh->onBuildState();

    // Tear down the old subtree (teardown + recursive delete) — same pair
    // FilesystemModule::applyNode uses; a bare delete would leak its children.
    if (old) {
        old->teardown();
        Scheduler::deleteTree(old);
    }

    // Disambiguate only now that the tree is in its final shape: `fresh` is in
    // place and `old` is gone. Run before this and firstByName wouldn't find
    // `fresh` (not yet linked) and would append a spurious " 2"; run after the
    // old module is removed and a genuine same-named sibling is the only thing
    // that triggers a suffix. No-op for a preserved custom name that's unique.
    if (scheduler_) scheduler_->ensureUniqueName(fresh);

    // Re-run onBuildState across the tree so Layer LUT / Drivers buffer
    // wiring re-forms — a replaced effect/driver re-wires like a freshly added one.
    if (scheduler_) scheduler_->buildState();

    // Persist: children are encoded positionally, so marking the parent dirty
    // rewrites "<index>.type" with the new typeName at the same slot.
    parent->markDirty();
    FilesystemModule::noteDirty();

    sendResponse(conn, 200, "application/json", "{\"ok\":true}");
}

void HttpServerModule::serveTypes(platform::TcpConnection& conn) {
    const char* header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));

    JsonSink sink(conn);
    sink.append("{\"types\":[");
    bool first = true;
    for (uint8_t i = 0; i < ModuleFactory::typeCount(); i++) {
        const char* name = ModuleFactory::typeName(i);
        if (!name) continue;
        ModuleRole role = ModuleFactory::typeRole(i);
        const char* roleStr = roleName(role);
        const char* docPath = ModuleFactory::typeDocPath(i);
        const char* tags = ModuleFactory::typeTags(i);
        uint8_t dim = ModuleFactory::typeDim(i);
        const char* childRoles = ModuleFactory::typeAcceptsChildRoles(i);
        // displayNameFor returns a pointer into a static buffer shared
        // across calls, so copy it to the stack before another factory
        // call (or the next loop iteration) overwrites it.
        char displayName[16];
        std::strncpy(displayName, ModuleFactory::displayNameFor(name, role), sizeof(displayName) - 1);
        displayName[sizeof(displayName) - 1] = 0;
        sink.appendf("%s{\"name\":\"%s\",\"displayName\":\"%s\",\"role\":\"%s\","
                     "\"docPath\":\"%s\",\"tags\":\"%s\",\"dim\":%u,"
                     "\"acceptsChildRoles\":\"%s\",\"defaults\":{",
                     first ? "" : ",", name, displayName, roleStr,
                     docPath ? docPath : "", tags ? tags : "",
                     static_cast<unsigned>(dim),
                     childRoles ? childRoles : "");
        writeTypeDefaults(sink, name);
        sink.append("}}");
        first = false;
    }
    sink.append("]}");
    sink.flush();
}

void HttpServerModule::writeTypeDefaults(JsonSink& sink, const char* typeName) {
    MoonModule* probe = ModuleFactory::create(typeName);
    if (!probe) return;
    probe->onBuildControls();
    auto& cs = probe->controls();
    bool first = true;
    for (uint8_t i = 0; i < cs.count(); i++) {
        auto& c = cs[i];
        // hasDefault filters out Password (default would defeat the secret),
        // ReadOnly/ReadOnlyInt/Progress (no user input to seed). Everyone
        // else emits `"name":value`; value rendering lives in Control.cpp.
        if (!hasDefault(c.type)) continue;
        sink.appendf("%s\"%s\":", first ? "" : ",", c.name);
        writeControlValue(sink, c);
        first = false;
    }
    probe->teardown();
    delete probe;
}

void HttpServerModule::handleMoveModule(platform::TcpConnection& conn, const char* moduleName, const char* body) {
    auto* mod = findModuleByName(moduleName);
    if (!mod) {
        sendResponse(conn, 404, "application/json", "{\"error\":\"module not found\"}");
        return;
    }
    auto* parent = mod->parent();
    if (!parent) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"top-level modules cannot be reordered\"}");
        return;
    }
    int to = mm::json::parseInt(body, "to");
    if (to < 0 || to >= parent->childCount()) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"to out of range\"}");
        return;
    }
    if (!parent->moveChildTo(mod, static_cast<uint8_t>(to))) {
        // Either already at position N or some other no-op — not an error per se,
        // but report so the UI can avoid a refetch storm on rapid drags.
        sendResponse(conn, 200, "application/json", "{\"ok\":true,\"noop\":true}");
        return;
    }
    // A move changes the parent's child ordering — mark the parent dirty so its
    // file is rewritten with the new order (same as add/delete handlers).
    parent->markDirty();
    FilesystemModule::noteDirty();
    if (scheduler_) scheduler_->buildState();
    sendResponse(conn, 200, "application/json", "{\"ok\":true}");
}

void HttpServerModule::handleReboot(platform::TcpConnection& conn) {
    FilesystemModule::flushPending();
    sendResponse(conn, 200, "application/json", "{\"ok\":true}");
    // Best-effort: close the socket and give LWIP a brief window to push the FIN
    // + payload out over Ethernet before esp_restart() yanks the world. Without the
    // delay the browser sees an aborted connection instead of a clean 200; the UI
    // copes (it auto-reconnects on WS close) but a clean response is friendlier.
    conn.close();
    platform::delayMs(200);
    platform::reboot();  // noreturn
}

void HttpServerModule::handleFirmwareUrl(platform::TcpConnection& conn, const char* body) {
    if constexpr (!platform::hasOta) {
        sendResponse(conn, 501, "application/json",
                     "{\"error\":\"OTA not supported on this platform\"}");
        return;
    }

    // Concurrency guard. esp_https_ota_begin rejects a second concurrent
    // OTA (ESP_FAIL on partition-already-acquired), but both racing tasks
    // would write to g_otaStatus/g_otaBytesRead/g_otaBytesTotal and the UI
    // shows garbled progress. Check g_otaStatus for an in-flight state and
    // reject early with 409. Successful OTAs reboot, so the only path that
    // re-enables a new attempt after an in-flight one is an explicit error.
    if (std::strcmp(g_otaStatus, "starting")    == 0 ||
        std::strcmp(g_otaStatus, "downloading") == 0 ||
        std::strcmp(g_otaStatus, "flashing")    == 0 ||
        std::strcmp(g_otaStatus, "rebooting")   == 0) {
        sendResponse(conn, 409, "application/json",
                     "{\"error\":\"ota already in progress\"}");
        return;
    }

    char url[512] = {};
    mm::json::parseString(body, "url", url, sizeof(url));
    if (url[0] == 0) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"url required\"}");
        return;
    }
    // Cheap URL-shape sanity: only http(s). Stops accidental file:// or
    // protocol-relative things from reaching the platform layer.
    if (std::strncmp(url, "http://", 7) != 0 && std::strncmp(url, "https://", 8) != 0) {
        sendResponse(conn, 400, "application/json",
                     "{\"error\":\"url must start with http:// or https://\"}");
        return;
    }

    // Seed the shared globals so the first WS push after this response shows
    // "starting" instead of whatever the previous OTA left behind (e.g. an
    // "error: …" string from a prior failed attempt).
    std::snprintf(g_otaStatus, sizeof(g_otaStatus), "starting");
    g_otaBytesRead = 0;
    g_otaBytesTotal = 0;

    if (!platform::http_fetch_to_ota(url, g_otaStatus, sizeof(g_otaStatus),
                                     &g_otaBytesRead, &g_otaBytesTotal)) {
        // The platform may have already written an error string; pass it through.
        char err[128];
        std::snprintf(err, sizeof(err),
                      "{\"error\":\"%s\"}", g_otaStatus[0] ? g_otaStatus : "ota start failed");
        sendResponse(conn, 500, "application/json", err);
        return;
    }
    // 202 Accepted — task running; UI polls FirmwareUpdate.update_status.
    sendResponse(conn, 202, "application/json", "{\"ok\":true}");
}

void HttpServerModule::handleWebSocketUpgrade(platform::TcpConnection& conn, const char* req) {
    // Extract Sec-WebSocket-Key
    const char* keyHeader = std::strstr(req, "Sec-WebSocket-Key: ");
    if (!keyHeader) { conn.close(); return; }
    keyHeader += 19;
    char wsKey[32] = {};
    int ki = 0;
    while (*keyHeader && *keyHeader != '\r' && ki < 31) {
        wsKey[ki++] = *keyHeader++;
    }
    wsKey[ki] = 0;

    // RFC 6455: accept = base64(SHA1(client_key + magic_GUID))
    // The GUID is a fixed constant from the spec, proving the server speaks WebSocket.
    char concat[128];
    std::snprintf(concat, sizeof(concat), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", wsKey);
    uint8_t sha1Hash[20];
    sha1(reinterpret_cast<const uint8_t*>(concat), std::strlen(concat), sha1Hash);
    char acceptKey[32];
    base64Encode(std::span<const uint8_t>(sha1Hash), std::span(acceptKey));

    // Send 101 response
    char response[256];
    int respLen = std::snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n",
        acceptKey);
    conn.write(reinterpret_cast<const uint8_t*>(response), respLen);

    // Store connection as WebSocket client
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (!wsClients_[i].valid()) {
            wsClients_[i] = std::move(conn);
            previewSend_.sent[i] = 0;   // fresh slot: clear any stale cursor a prior client left here
            // Abandon any in-flight buffered frame: this new client would otherwise either be skipped
            // (its stale cursor ≥ total looked "done", so it got no frame and the browser showed
            // nothing) or spliced into a half-sent message. Cancelling makes the next frame start
            // clean for every client. The generation bump re-streams the coord table first.
            previewSend_.active = false;
            wsClientGeneration_++;
            return;
        }
    }
    // No slot available — close
    conn.close();
}

void HttpServerModule::pushStateToWebSockets() {
    bool hasClients = false;
    for (auto& ws : wsClients_) {
        if (ws.valid()) { hasClients = true; break; }
    }
    if (!hasClients) return;

    // Buffer-mode sink: the WS frame header needs the total length up front,
    // so the JSON is collected into a growable heap buffer (no size ceiling).
    JsonSink sink;
    buildStateJson(sink);

    for (auto& ws : wsClients_) {
        if (!ws.valid()) continue;
        if (!sendWsTextFrame(ws, sink.data(), static_cast<int>(sink.size()))) {
            ws.close();
        }
    }

    // Also push a WLED-shaped {state, info} frame. The native WLED app connects to this
    // same /ws and reads live state (colour, brightness, on/off) from a DeviceStateInfo
    // message — it has no /json/si GET. Our own UI ignores this frame (its JS keys on
    // `modules`); the WLED app ignores our module frame (its Moshi keys on `state`/`info`).
    // Two small frames, each consumer parses its own — no client needs to know about the
    // other. This is what makes the device's card show the live colour + a working slider.
    pushWledStateToWebSockets();
}

// Build and push the WLED {state, info} object to every WS client. Shares the same body
// writers as /json/si.
void HttpServerModule::pushWledStateToWebSockets() {
    bool hasClients = false;
    for (auto& ws : wsClients_) if (ws.valid()) { hasClients = true; break; }
    if (!hasClients) return;

    const char* name = "projectMM";
    if (MoonModule* sys = findModuleByName("System")) {
        const char* dn = static_cast<SystemModule*>(sys)->deviceName();
        if (dn && dn[0]) name = dn;
    }
    uint8_t mac[6] = {};
    platform::getMacAddress(mac);

    JsonSink sink;
    sink.appendf("{\"state\":");
    writeWledStateBody(sink);
    sink.appendf(",\"info\":");
    writeWledInfoBody(sink, name, mac);
    sink.appendf("}");

    for (auto& ws : wsClients_) {
        if (!ws.valid()) continue;
        if (!sendWsTextFrame(ws, sink.data(), static_cast<int>(sink.size()))) ws.close();
    }
}

// Read one pending WS frame per client and, if it's a WLED state-set ({on}/{bri}), apply
// it to Drivers. The native WLED app's slider/toggle SEND state over /ws (sendState),
// not via HTTP POST, so this is the inbound half of the control path. Client→server
// frames are always MASKED (RFC 6455 §5.3): we unmask in place before parsing. Only the
// small text frame we care about is handled; we ignore continuation/binary/control frames
// (a ping/close is rare on this short-lived control socket and harmless to skip).
void HttpServerModule::pollWledStateFromWebSockets() {
    for (auto& ws : wsClients_) {
        if (!ws.valid()) continue;
        uint8_t f[512];
        int n = ws.read(f, sizeof(f));             // non-blocking (read() returns -1 if nothing)
        if (n < 6) continue;                       // a masked text frame is ≥6 bytes
        // A fast slider drag can land MULTIPLE small {on,bri} frames in one read; walk every
        // complete masked text frame in the chunk so none is dropped (apply each in order →
        // the last value wins, matching the drag). The app's frames are tiny single-segment
        // text frames, so partial-frame reassembly across reads isn't needed; a trailing
        // partial frame is simply left for the next poll.
        size_t off = 0;
        const size_t total = static_cast<size_t>(n);
        while (off + 6 <= total) {
            const uint8_t* fr = f + off;
            const uint8_t opcode = fr[0] & 0x0f;
            const bool masked = fr[1] & 0x80;
            size_t len = fr[1] & 0x7f;
            size_t hdr = 2;
            if (len == 126) {
                if (off + 4 > total) break;
                len = (size_t(fr[2]) << 8) | fr[3]; hdr = 4;
            } else if (len == 127) {
                break;                              // >64 KB control message: not ours, stop
            }
            const size_t frameLen = hdr + 4 + len;  // header + mask key + payload (client = masked)
            if (!masked || off + frameLen > total) break;   // incomplete/unmasked — leave for later
            if (opcode == 0x1 && len < 200) {       // a text frame small enough to be a state-set
                const uint8_t* mask = fr + hdr;
                char body[200];
                for (size_t i = 0; i < len; i++) body[i] = static_cast<char>(fr[hdr + 4 + i] ^ mask[i & 3]);
                body[len] = 0;
                if (mm::json::hasKey(body, "on") || mm::json::hasKey(body, "bri"))
                    applyWledState(body);
            }
            off += frameLen;
        }
    }
}

bool HttpServerModule::sendWsTextFrame(platform::TcpConnection& conn, const char* data, int len) {
    uint8_t header[10];
    int headerLen = 0;

    header[0] = 0x81; // FIN + text opcode
    if (len < 126) {
        header[1] = static_cast<uint8_t>(len);
        headerLen = 2;
    } else if (len < 65536) {
        header[1] = 126;
        header[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
        header[3] = static_cast<uint8_t>(len & 0xFF);
        headerLen = 4;
    } else {
        return false; // too large
    }

    if (!conn.write(header, headerLen)) return false;
    return conn.write(reinterpret_cast<const uint8_t*>(data), len);
}

// Write the whole span via repeated non-blocking writeSome; close the client + return false if it
// can't all go right now. Bounded TOTAL would-block spins (not reset on progress) hard-bound how
// long this synchronous send can occupy the caller's loop; a span that doesn't complete in budget
// closes the client (the browser reconnects). Used by the begin/push/end stream (the coord table
// and downsampled colour frame); the full-res colour frame uses the resumable sendBufferedFrame.
bool HttpServerModule::sendAllOrClose(platform::TcpConnection& ws, const uint8_t* data, size_t len) {
    size_t sent = 0;
    int stalls = 0;
    while (sent < len) {
        int n = ws.writeSome(data + sent, len - sent);
        if (n < 0) { ws.close(); return false; }       // real socket error
        if (n == 0) {                                  // WouldBlock — lwIP send buffer momentarily full
            if (++stalls > kDirectSendSpins) { ws.close(); return false; }
            continue;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Streamed frame: header now, payload pushed in slices, no frame-sized staging buffer — so a
// large frame (PreviewDriver's coordinate table or colour frame) goes out on a memory-tight
// board where a contiguous block won't fit. The producer (forEachCoord) pushes forward-only;
// each slice fans to every client before the next push. A client that can't keep up is closed
// (its WS message ends incomplete → it reconnects), so this never blocks the tick indefinitely.
void HttpServerModule::beginBinaryFrame(size_t totalLen) {
    wsFrameAllSent_ = true;
    uint8_t wsHeader[10];
    int wsHeaderLen;
    wsHeader[0] = 0x82;
    if (totalLen < 126) { wsHeader[1] = static_cast<uint8_t>(totalLen); wsHeaderLen = 2; }
    else if (totalLen < 65536) {
        wsHeader[1] = 126; wsHeader[2] = static_cast<uint8_t>((totalLen >> 8) & 0xFF);
        wsHeader[3] = static_cast<uint8_t>(totalLen & 0xFF); wsHeaderLen = 4;
    } else {
        wsHeader[1] = 127;
        for (int i = 0; i < 8; i++)
            wsHeader[2 + i] = static_cast<uint8_t>((static_cast<uint64_t>(totalLen) >> (56 - 8 * i)) & 0xFF);
        wsHeaderLen = 10;
    }
    for (auto& ws : wsClients_) {
        if (ws.valid() && !sendAllOrClose(ws, wsHeader, static_cast<size_t>(wsHeaderLen)))
            wsFrameAllSent_ = false;
    }
}

void HttpServerModule::pushBinaryFrame(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;
    for (auto& ws : wsClients_) {
        if (ws.valid() && !sendAllOrClose(ws, data, len)) wsFrameAllSent_ = false;
    }
}

bool HttpServerModule::endBinaryFrame() { return wsFrameAllSent_; }

// Resumable full-frame send. One WS message = WS framing header + the caller's app header (both
// copied into previewSend_.hdr) + the caller's `body` (a pointer, NOT copied). Each client's
// cursor walks the logical stream [hdr ++ body], drained a chunk at a time in drainPreviewSend.
bool HttpServerModule::sendBufferedFrame(const uint8_t* header, size_t headerLen,
                                         const uint8_t* body, size_t bodyLen) {
    // Drop-new backpressure: one frame in flight at a time. A caller that asks while a send is active
    // is told "busy" — the in-flight frame is kept and this new one is rejected, which the producer
    // reads as "link is behind" and uses to shed frame rate (it requeues nothing, so the loop runs on).
    if (previewSend_.active) return false;

    const size_t totalLen = headerLen + bodyLen;   // WS payload length = app header + body
    // Build the WS frame header (FIN + binary opcode, unmasked; 7/16/64-bit length form) directly
    // into previewSend_.hdr, followed by the app header — so the cursor streams them as one span.
    uint8_t* h = previewSend_.hdr;
    size_t wsLen;
    h[0] = 0x82;
    if (totalLen < 126) { h[1] = static_cast<uint8_t>(totalLen); wsLen = 2; }
    else if (totalLen < 65536) {
        h[1] = 126; h[2] = static_cast<uint8_t>((totalLen >> 8) & 0xFF);
        h[3] = static_cast<uint8_t>(totalLen & 0xFF); wsLen = 4;
    } else {
        h[1] = 127;
        for (int i = 0; i < 8; i++)
            h[2 + i] = static_cast<uint8_t>((static_cast<uint64_t>(totalLen) >> (56 - 8 * i)) & 0xFF);
        wsLen = 10;
    }
    // The app header follows the WS header in the same buffer. sizeof(hdr)=16 holds the 10-byte WS
    // form + the preview app headers (≤10 bytes); guard so a future larger header can't overrun.
    if (wsLen + headerLen > sizeof(previewSend_.hdr)) return false;
    for (size_t i = 0; i < headerLen; i++) previewSend_.hdr[wsLen + i] = header[i];

    previewSend_.hdrLen = wsLen + headerLen;
    previewSend_.body = body;
    previewSend_.bodyLen = bodyLen;
    for (int i = 0; i < MAX_WS_CLIENTS; i++) previewSend_.sent[i] = 0;
    previewSend_.active = true;
    // Deliberately do NOT drain here. sendBufferedFrame is called from PreviewDriver's loop() on the
    // RENDER thread; a socket writeSome is variable-cost (0..~ms) and would land that cost — and its
    // jitter — directly on the render tick, hitching the LEDs. So we only queue the frame (copy the
    // header, point at the body) and let drainPreviewSend() push bytes purely on loop20ms, off the
    // render hot path. The frame starts draining within one transport poll (≤20 ms).
    return true;
}

// Per-client cursor over the logical [hdr ++ body] stream: write whatever the socket takes now (up
// to one memory-adaptive chunk), advance the cursor, leave the rest for the next tick. A real
// socket error closes that client (its WS message ends incomplete → the browser discards it). The
// send completes when every live client has the whole frame, or when no client is left.
void HttpServerModule::drainPreviewSend() {
    if (!previewSend_.active) return;
    const size_t total = previewSend_.hdrLen + previewSend_.bodyLen;
    const size_t chunk = previewChunkBytes();
    bool anyLiveClient = false;
    bool allDone = true;
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        auto& ws = wsClients_[i];
        if (!ws.valid()) continue;
        anyLiveClient = true;
        size_t& cur = previewSend_.sent[i];
        size_t budget = chunk;   // bound bytes pushed to THIS client this tick → bounded tick cost
        while (cur < total && budget > 0) {
            // Source the next byte run from hdr (cursor < hdrLen) or body (cursor >= hdrLen).
            const uint8_t* src;
            size_t span;
            if (cur < previewSend_.hdrLen) { src = previewSend_.hdr + cur; span = previewSend_.hdrLen - cur; }
            else { src = previewSend_.body + (cur - previewSend_.hdrLen); span = total - cur; }
            if (span > budget) span = budget;
            int n = ws.writeSome(src, span);
            if (n < 0) { ws.close(); break; }    // real error — drop this client
            if (n == 0) break;                   // WouldBlock — leave the rest for next tick (no spin)
            cur += static_cast<size_t>(n);
            budget -= static_cast<size_t>(n);
        }
        if (ws.valid() && cur < total) allDone = false;
    }
    // Done when every live client finished, or no client remains to send to.
    if (!anyLiveClient || allDone) previewSend_.active = false;
}

// Per-tick per-client chunk cap, derived from free contiguous memory: a tight board takes small
// bites (so one drain can't dominate the tick), a roomy board drains a big frame in a tick or two.
// Bounded both ways — never below a floor (forward progress) nor above a ceiling (tick occupancy).
size_t HttpServerModule::previewChunkBytes() const {
    constexpr size_t kFloor = 2048;     // always make real progress, even on a fragmented board
    constexpr size_t kCeil  = 65536;    // cap tick occupancy regardless of how much RAM is free
    const size_t block = platform::maxAllocBlock();
    size_t chunk = block / 8;           // a fraction of the largest contiguous block
    if (chunk < kFloor) chunk = kFloor;
    if (chunk > kCeil)  chunk = kCeil;
    return chunk;
}

} // namespace mm
