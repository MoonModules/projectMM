// HttpServerModule implementation. Public surface and class layout live in
// HttpServerModule.h. Per the project policy in CLAUDE.md, core service modules
// that bridge to the platform (HTTP server, WebSocket framing, JSON state push)
// split into .h + .cpp so implementation edits don't cascade-recompile every TU
// that includes the header.

#include "core/HttpServerModule.h"

#include "core/Scheduler.h"
#include "core/PreviewFrame.h"
#include "core/ModuleFactory.h"
#include "core/JsonUtil.h"
#include "core/JsonSink.h"
#include "core/Sha1.h"
#include "core/Base64.h"
#include "core/FilesystemModule.h"
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
    for (auto& ws : wsClients_) ws.close();
    server_.close();
}

void HttpServerModule::loop20ms() {
    // Accept one HTTP connection per tick
    auto conn = server_.accept();
    if (conn.valid()) {
        handleConnection(conn);
        return; // don't broadcast in same tick as accept (WebSocket needs time to process 101)
    }

    // Broadcast preview frame if ready
    if (previewFrame_ && previewFrame_->ready) {
        broadcastPreviewFrame();
        previewFrame_->ready = false;
    }
}

void HttpServerModule::loop1s() {
    pushStateToWebSockets();
}

void HttpServerModule::handleConnection(platform::TcpConnection& conn) {
    uint8_t buf[2048];
    int totalRead = 0;

    // Read request (blocking with timeout on desktop, retries on ESP32)
    for (int attempt = 0; attempt < 20 && totalRead < static_cast<int>(sizeof(buf) - 1); attempt++) {
        int n = conn.read(buf + totalRead, sizeof(buf) - 1 - totalRead);
        if (n > 0) {
            totalRead += n;
            buf[totalRead] = 0;
            if (std::strstr(reinterpret_cast<char*>(buf), "\r\n\r\n")) break;
        } else if (n == 0) {
            return; // peer closed
        } else {
            break; // timeout or error
        }
    }

    if (totalRead == 0) { conn.close(); return; }
    buf[totalRead] = 0;
    auto* req = reinterpret_cast<char*>(buf);

    // If we have headers but body might still be arriving, read more
    auto* headerEnd = std::strstr(req, "\r\n\r\n");
    if (headerEnd) {
        auto* clh = std::strstr(req, "Content-Length:");
        if (clh) {
            int contentLen = std::atoi(clh + 15);
            int headerSize = static_cast<int>(headerEnd + 4 - req);
            int bodyNeeded = headerSize + contentLen;
            while (totalRead < bodyNeeded && totalRead < static_cast<int>(sizeof(buf) - 1)) {
                int n = conn.read(buf + totalRead, sizeof(buf) - 1 - totalRead);
                if (n > 0) totalRead += n;
                else break;
            }
            buf[totalRead] = 0;
        }
    }

    // Parse method and path
    char method[8] = {};
    char path[128] = {};
    std::sscanf(req, "%7s %127s", method, path);

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
        else if (std::strcmp(path, "/style.css") == 0) serveFile(conn, "style.css", "text/css");
        else if (std::strcmp(path, "/moonlight-logo.png") == 0) serveFile(conn, "moonlight-logo.png", "image/png");
        else if (std::strcmp(path, "/api/state") == 0) serveState(conn);
        else if (std::strcmp(path, "/api/system") == 0) serveSystem(conn);
        else if (std::strcmp(path, "/api/types") == 0) serveTypes(conn);
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
        } else if (std::strcmp(path, "/api/reboot") == 0) {
            handleReboot(conn);
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
    } else {
        sendResponse(conn, 405, "text/plain", "Method not allowed");
    }

    conn.close();
}

void HttpServerModule::sendResponse(platform::TcpConnection& conn, int status, const char* contentType, const char* body) {
    const char* statusText = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" : "Error";
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

    // Fall back to embedded data (ESP32 or when disk files not found)
    const uint8_t* data = nullptr;
    size_t dataLen = 0;
    if (std::strcmp(filename, "index.html") == 0) { data = ui::indexHtml; dataLen = ui::indexHtmlLen; }
    else if (std::strcmp(filename, "app.js") == 0) { data = ui::appJs; dataLen = ui::appJsLen; }
    else if (std::strcmp(filename, "style.css") == 0) { data = ui::styleCss; dataLen = ui::styleCssLen; }
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
        "Connection: close\r\n"
        "Cache-Control: no-cache\r\n"
        "\r\n",
        contentType, dataLen);
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
        // Per-type body emitted WITHOUT the closing }. We append "hidden" then } afterwards.
        switch (c.type) {
            case ControlType::Uint8:
                sink.appendf(
                    "{\"name\":\"%s\",\"type\":\"uint8\",\"value\":%u,\"min\":%u,\"max\":%u",
                    c.name, *static_cast<uint8_t*>(c.ptr), c.min, c.max);
                break;
            case ControlType::Uint16:
                sink.appendf(
                    "{\"name\":\"%s\",\"type\":\"uint16\",\"value\":%u",
                    c.name, *static_cast<uint16_t*>(c.ptr));
                break;
            case ControlType::Int16:
                sink.appendf(
                    "{\"name\":\"%s\",\"type\":\"int16\",\"value\":%d",
                    c.name, *static_cast<int16_t*>(c.ptr));
                break;
            case ControlType::Bool:
                sink.appendf(
                    "{\"name\":\"%s\",\"type\":\"bool\",\"value\":%s",
                    c.name, *static_cast<bool*>(c.ptr) ? "true" : "false");
                break;
            case ControlType::Text: {
                char escaped[128];
                jsonEscape(static_cast<char*>(c.ptr), escaped, sizeof(escaped));
                sink.appendf(
                    "{\"name\":\"%s\",\"type\":\"text\",\"value\":\"%s\"",
                    c.name, escaped);
                break;
            }
            case ControlType::Password: {
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
                sink.appendf(
                    "{\"name\":\"%s\",\"type\":\"password\",\"value\":\"%s\"",
                    c.name, encoded);
                break;
            }
            case ControlType::ReadOnly: {
                char escaped[128];
                jsonEscape(static_cast<char*>(c.ptr), escaped, sizeof(escaped));
                sink.appendf(
                    "{\"name\":\"%s\",\"type\":\"display\",\"value\":\"%s\"",
                    c.name, escaped);
                break;
            }
            case ControlType::Select: {
                sink.appendf(
                    "{\"name\":\"%s\",\"type\":\"select\",\"value\":%u,\"options\":[",
                    c.name, *static_cast<uint8_t*>(c.ptr));
                auto* options = reinterpret_cast<const char* const*>(c.aux);
                for (uint8_t o = 0; o < c.max; o++) {
                    sink.appendf("%s\"%s\"", o > 0 ? "," : "", options[o]);
                }
                sink.append("]");
                break;
            }
            case ControlType::Progress:
                sink.appendf(
                    "{\"name\":\"%s\",\"type\":\"progress\",\"value\":%lu,\"total\":%lu",
                    c.name, static_cast<unsigned long>(*static_cast<uint32_t*>(c.ptr)),
                    static_cast<unsigned long>(c.aux));
                break;
        }
        // Emit "hidden":true only when set (common case is false; omit to save bytes).
        // Then close the per-control object.
        sink.append(c.hidden ? ",\"hidden\":true}" : "}");
    }
}

void HttpServerModule::handleSetControl(platform::TcpConnection& conn, const char* body) {
    // Parse: {"module":"Noise","control":"scale","value":8}
    char moduleName[32] = {};
    char controlName[32] = {};
    mm::json::parseString(body, "module", moduleName, sizeof(moduleName));
    mm::json::parseString(body, "control", controlName, sizeof(controlName));

    // Find the module by name
    MoonModule* target = findModuleByName(moduleName);
    if (!target) {
        sendResponse(conn, 404, "application/json", "{\"error\":\"module not found\"}");
        return;
    }

    // Handle module-level "enabled" property
    if (std::strcmp(controlName, "enabled") == 0) {
        target->setEnabled(mm::json::parseBool(body, "value"));
        target->markDirty();
        FilesystemModule::noteDirty();
        if (scheduler_) scheduler_->rebuild();
        sendResponse(conn, 200, "application/json", "{\"ok\":true}");
        return;
    }

    // Find the control by name and set value
    auto& ctrls = target->controls();
    for (uint8_t i = 0; i < ctrls.count(); i++) {
        auto& c = ctrls[i];
        if (std::strcmp(c.name, controlName) != 0) continue;

        switch (c.type) {
            case ControlType::Uint8: {
                int v = mm::json::parseInt(body, "value");
                // Out-of-range from a hostile / buggy client: reject with
                // 400 rather than wrap on the static_cast. Matches the
                // Select branch below.
                if (v < c.min || v > c.max) {
                    sendResponse(conn, 400, "application/json", "{\"error\":\"value out of range\"}");
                    return;
                }
                *static_cast<uint8_t*>(c.ptr) = static_cast<uint8_t>(v);
                break;
            }
            case ControlType::Uint16: {
                int v = mm::json::parseInt(body, "value");
                // No c.min/c.max check: those fields are uint8_t and can't
                // bound a uint16 range (would 400-reject every value > 255).
                // Clamp to the natural type range to prevent static_cast wrap.
                if (v < 0) v = 0;
                if (v > UINT16_MAX) v = UINT16_MAX;
                *static_cast<uint16_t*>(c.ptr) = static_cast<uint16_t>(v);
                break;
            }
            case ControlType::Int16: {
                int v = mm::json::parseInt(body, "value");
                // Clamp to the natural type range. Same reason as Uint16: c.min
                // and c.max are uint8_t and can't bound int16, so applying them
                // would 400-reject every value outside 0..255.
                if (v < INT16_MIN) v = INT16_MIN;
                if (v > INT16_MAX) v = INT16_MAX;
                *static_cast<int16_t*>(c.ptr) = static_cast<int16_t>(v);
                break;
            }
            case ControlType::Bool: {
                bool v = mm::json::parseBool(body, "value");
                *static_cast<bool*>(c.ptr) = v;
                break;
            }
            case ControlType::Text:
            case ControlType::Password: {
                // Password writes set the real value just like Text; only
                // serialization (writeControls) hides it.
                char v[64] = {};
                mm::json::parseString(body, "value", v, sizeof(v));
                uint8_t maxLen = c.max > 0 ? c.max - 1 : 15;
                std::strncpy(static_cast<char*>(c.ptr), v, maxLen);
                static_cast<char*>(c.ptr)[maxLen] = '\0';
                break;
            }
            case ControlType::Select: {
                int v = mm::json::parseInt(body, "value");
                if (v < 0 || v >= c.max) {
                    sendResponse(conn, 400, "application/json", "{\"error\":\"value out of range\"}");
                    return;
                }
                *static_cast<uint8_t*>(c.ptr) = static_cast<uint8_t>(v);
                break;
            }
            case ControlType::ReadOnly:
            case ControlType::Progress:
                break; // read-only, skip
        }
        // Rebuild controls only for Select (dynamic onBuildControls), rebuild pipeline for all
        if (c.type == ControlType::Select) {
            target->rebuildControls();
        }
        target->markDirty();
        FilesystemModule::noteDirty();
        if (scheduler_) scheduler_->rebuild();

        sendResponse(conn, 200, "application/json", "{\"ok\":true}");
        return;
    }
    sendResponse(conn, 404, "application/json", "{\"error\":\"control not found\"}");
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
    sink.appendf(
        "{\"fps\":%u,\"tickTimeUs\":%u,\"freeHeap\":%u,\"freeInternal\":%u,\"maxBlock\":%u,\"uptime\":%u,\"modules\":[",
        static_cast<unsigned>(scheduler_ ? scheduler_->fps() : 0),
        static_cast<unsigned>(scheduler_ ? scheduler_->tickTimeUs() : 0),
        static_cast<unsigned>(platform::freeHeap()),
        static_cast<unsigned>(platform::freeInternalHeap()),
        static_cast<unsigned>(platform::maxAllocBlock()),
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

void HttpServerModule::handleAddModule(platform::TcpConnection& conn, const char* body) {
    char typeName[32] = {};
    char id[32] = {};
    char parentId[32] = {};
    mm::json::parseString(body, "type", typeName, sizeof(typeName));
    mm::json::parseString(body, "id", id, sizeof(id));
    mm::json::parseString(body, "parent_id", parentId, sizeof(parentId));

    if (typeName[0] == 0) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"missing type\"}");
        return;
    }

    // Top-level modules (Layouts/Layers/Drivers/Filesystem/System/Network/HttpServer)
    // are policy-fixed and wired in main.cpp at boot. The HTTP surface only
    // allows adding *child* modules to an existing parent — anything else
    // would be an orphan (not added to any tree, not registered with the
    // scheduler, never ticked, leaked). Reject early and symmetrically with
    // handleDeleteModule / handleReplaceModule (both also 400 on top-level).
    // Scenario tests adding top-level modules go through scenario_runner.cpp's
    // in-process path, not this HTTP handler.
    if (parentId[0] == 0) {
        sendResponse(conn, 400, "application/json",
                     "{\"error\":\"parent_id required (top-level modules are policy-fixed in main.cpp)\"}");
        return;
    }

    // Check if module with this name already exists
    if (id[0] != 0 && findModuleByName(id)) {
        sendResponse(conn, 200, "application/json", "{\"ok\":true,\"note\":\"already exists\"}");
        return;
    }

    // Resolve the parent before allocating — failure here means we never
    // construct an orphan module.
    auto* parent = findModuleByName(parentId);
    if (!parent) {
        sendResponse(conn, 404, "application/json", "{\"error\":\"parent not found\"}");
        return;
    }

    // Create module via factory
    auto* mod = ModuleFactory::create(typeName);
    if (!mod) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"unknown type\"}");
        return;
    }
    if (id[0] != 0) mod->setName(id);

    if (!parent->addChild(mod)) {
        delete mod;
        sendResponse(conn, 400, "application/json", "{\"error\":\"parent rejected child\"}");
        return;
    }

    // Disambiguate the name if something else in the tree already carries
    // it. Factory display names like "Layer" collide when a second Layer is
    // added (factory has no per-instance state), and findModuleByName does
    // a first-match DFS so the second one becomes unreachable. The
    // Scheduler also runs the same pass over the whole tree after
    // persistence load (see Scheduler::setup phase 2a). Single source of
    // truth — both paths go through Scheduler::ensureUniqueName.
    if (scheduler_) scheduler_->ensureUniqueName(mod);

    // Lifecycle: same phase order as Scheduler::setup() — onBuildControls() first so
    // control buffers are bound, then setup() (which may read those bound members),
    // then onAllocateMemory(). Getting this order wrong means a module's setup() sees
    // uninitialized control state.
    mod->onBuildControls();
    mod->setup();
    mod->onAllocateMemory();

    if (scheduler_) scheduler_->rebuild();

    // Persist the new tree shape — marking the parent dirty causes saveSubtree
    // to write the parent's file with the new child slot included. The save is
    // debounced (2s after the last dirty mark) so an immediate reboot won't catch
    // the write; callers wanting a synchronous save can call FilesystemModule::flush().
    // parent is guaranteed non-null by the top-of-function checks.
    parent->markDirty();
    FilesystemModule::noteDirty();

    sendResponse(conn, 200, "application/json", "{\"ok\":true}");
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

    // Remove from parent
    parent->removeChild(mod);

    // Tear down + recursively free the whole subtree. A bare `delete mod`
    // here would only free mod's children_ pointer array (MoonModule's
    // destructor calls `delete[] children_`); each child module the array
    // pointed to would leak. Use the same pair handleReplaceModule does.
    mod->teardown();
    Scheduler::deleteTree(mod);

    if (scheduler_) scheduler_->rebuild();

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

    // Swap in place; replaceChildAt returns the old module, which we own.
    MoonModule* old = parent->replaceChildAt(index, fresh);

    // Lifecycle on the fresh module — same phase order as the add path.
    fresh->onBuildControls();
    fresh->setup();
    fresh->onAllocateMemory();

    // Tear down the old subtree (teardown + recursive delete) — same pair
    // FilesystemModule::applyNode uses; a bare delete would leak its children.
    if (old) {
        old->teardown();
        Scheduler::deleteTree(old);
    }

    // Re-run onAllocateMemory across the tree so Layer LUT / Drivers buffer
    // wiring re-forms — a replaced effect/driver re-wires like a freshly added one.
    if (scheduler_) scheduler_->rebuild();

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
        // displayNameFor returns a pointer into a static buffer shared
        // across calls, so copy it to the stack before another factory
        // call (or the next loop iteration) overwrites it.
        char displayName[16];
        std::strncpy(displayName, ModuleFactory::displayNameFor(name, role), sizeof(displayName) - 1);
        displayName[sizeof(displayName) - 1] = 0;
        sink.appendf("%s{\"name\":\"%s\",\"displayName\":\"%s\",\"role\":\"%s\","
                     "\"docPath\":\"%s\",\"tags\":\"%s\",\"dim\":%u,\"defaults\":{",
                     first ? "" : ",", name, displayName, roleStr,
                     docPath ? docPath : "", tags ? tags : "",
                     static_cast<unsigned>(dim));
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
        switch (c.type) {
            case ControlType::Uint8:
                sink.appendf("%s\"%s\":%u", first ? "" : ",", c.name,
                             *static_cast<uint8_t*>(c.ptr));
                break;
            case ControlType::Uint16:
                sink.appendf("%s\"%s\":%u", first ? "" : ",", c.name,
                             *static_cast<uint16_t*>(c.ptr));
                break;
            case ControlType::Int16:
                sink.appendf("%s\"%s\":%d", first ? "" : ",", c.name,
                             *static_cast<int16_t*>(c.ptr));
                break;
            case ControlType::Bool:
                sink.appendf("%s\"%s\":%s", first ? "" : ",", c.name,
                             *static_cast<bool*>(c.ptr) ? "true" : "false");
                break;
            case ControlType::Text: {
                char escaped[128];
                jsonEscape(static_cast<char*>(c.ptr), escaped, sizeof(escaped));
                sink.appendf("%s\"%s\":\"%s\"", first ? "" : ",", c.name, escaped);
                break;
            }
            case ControlType::Select:
                sink.appendf("%s\"%s\":%u", first ? "" : ",", c.name,
                             *static_cast<uint8_t*>(c.ptr));
                break;
            default:
                continue;  // ReadOnly/Progress: no default; Password: never serialized
        }
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
    if (scheduler_) scheduler_->rebuild();
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
    for (auto& ws : wsClients_) {
        if (!ws.valid()) {
            ws = std::move(conn);
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

void HttpServerModule::broadcastPreviewFrame() {
    if (!previewFrame_ || !previewFrame_->data || previewFrame_->dataLen == 0) return;

    // Build 13-byte preview header on stack:
    //   [0x02][dw16][dh16][dd16][ow16][oh16][od16]
    // dw/dh/dd = dimensions of the (downsampled) data in the payload;
    // ow/oh/od = original physical grid dimensions, for optional UI upscale.
    // All uint16 little-endian.
    uint8_t previewHeader[13];
    previewHeader[0] = 0x02;
    auto put16 = [&](int at, lengthType v) {
        previewHeader[at]     = static_cast<uint8_t>(v & 0xFF);
        previewHeader[at + 1] = static_cast<uint8_t>(v >> 8);
    };
    put16(1, previewFrame_->width);
    put16(3, previewFrame_->height);
    put16(5, previewFrame_->depth);
    put16(7, previewFrame_->origWidth);
    put16(9, previewFrame_->origHeight);
    put16(11, previewFrame_->origDepth);

    size_t totalLen = 13 + previewFrame_->dataLen;

    // WebSocket frame header for a binary message of totalLen bytes.
    uint8_t wsHeader[4];
    int wsHeaderLen = 0;
    wsHeader[0] = 0x82; // FIN + binary opcode
    if (totalLen < 126) {
        wsHeader[1] = static_cast<uint8_t>(totalLen);
        wsHeaderLen = 2;
    } else if (totalLen < 65536) {
        wsHeader[1] = 126;
        wsHeader[2] = static_cast<uint8_t>((totalLen >> 8) & 0xFF);
        wsHeader[3] = static_cast<uint8_t>(totalLen & 0xFF);
        wsHeaderLen = 4;
    } else {
        return; // frame too large for the 16-bit length form
    }

    // One scatter-gather chunk list: WS header + preview header + payload.
    // The payload chunk points at PreviewDriver's own downsample buffer —
    // PreviewDriver::loop() writes the strided RGB copy there and sets
    // previewFrame_->data to it. No copy here; the buffer is driver-owned
    // (not a Drivers slice — the downsample step owns its own storage).
    const platform::WriteChunk chunks[] = {
        { wsHeader,      static_cast<size_t>(wsHeaderLen) },
        { previewHeader, sizeof(previewHeader) },
        { previewFrame_->data, previewFrame_->dataLen },
    };
    constexpr int chunkCount = sizeof(chunks) / sizeof(chunks[0]);

    for (auto& ws : wsClients_) {
        if (!ws.valid()) continue;
        switch (ws.writeChunks(chunks, chunkCount)) {
            case platform::WriteResult::Complete:
            case platform::WriteResult::WouldBlock:
                // WouldBlock: browser is backpressured — skip this frame,
                // keep the connection open (the next frame may fit).
                break;
            case platform::WriteResult::Partial:
            case platform::WriteResult::Error:
                // Partial: a truncated WS message went out — the stream is
                // corrupt, the connection must be dropped. Error: dead socket.
                ws.close();
                break;
        }
    }
}

} // namespace mm
