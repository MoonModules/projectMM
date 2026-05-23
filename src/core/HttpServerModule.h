#pragma once

#include "core/MoonModule.h"
#include "core/Scheduler.h"
#include "core/PreviewFrame.h"
#include "core/ModuleFactory.h"
#include "core/JsonUtil.h"
#include "core/FilesystemModule.h"
#include "platform/platform.h"

#include "ui/ui_embedded.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace mm {

// Writes JSON with no fixed-buffer size ceiling. Two modes:
//  - socket mode: a small staging buffer flushes to a TcpConnection as it fills,
//    so the whole response never lives in RAM at once (used by GET /api/state).
//  - buffer mode: bytes collect in a heap buffer that doubles on demand, for
//    callers that need the assembled JSON + its length (the WebSocket push,
//    whose frame header carries the length up front).
// Either way, a module tree of any size serializes correctly.
class JsonSink {
public:
    // Socket mode.
    explicit JsonSink(platform::TcpConnection& conn) : conn_(&conn) {}

    // Buffer mode — collects into a growable heap buffer.
    JsonSink() = default;

    ~JsonSink() { if (heap_) platform::free(heap_); }

    JsonSink(const JsonSink&) = delete;
    JsonSink& operator=(const JsonSink&) = delete;

    void append(const char* s) {
        if (!s) return;
        while (*s) {
            if (conn_) {
                if (pos_ == STAGE_SIZE) flushStage();
                stage_[pos_++] = *s++;
            } else {
                if (!ensureHeap(heapLen_ + 1)) return;  // out of memory: drop
                heap_[heapLen_++] = *s++;
            }
        }
    }

    // Append a printf-formatted fragment. The common case (one control, one
    // module header) fits the FRAG_MAX stack buffer; a fragment that would
    // exceed it — e.g. an unusually long text-control value — is re-formatted
    // into a heap buffer so the output is never silently truncated.
    void appendf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char frag[FRAG_MAX];
        va_list ap;
        va_start(ap, fmt);
        va_list ap2;
        va_copy(ap2, ap);
        int n = std::vsnprintf(frag, sizeof(frag), fmt, ap);
        va_end(ap);
        if (n < 0) { va_end(ap2); return; }
        if (static_cast<size_t>(n) < sizeof(frag)) {
            va_end(ap2);
            append(frag);
            return;
        }
        // Fragment longer than the stack buffer — format into an exact heap buffer.
        char* big = static_cast<char*>(platform::alloc(static_cast<size_t>(n) + 1));
        if (big) {
            std::vsnprintf(big, static_cast<size_t>(n) + 1, fmt, ap2);
            append(big);
            platform::free(big);
        }
        va_end(ap2);
    }

    // Socket mode: flush staged bytes to the socket. Call once at the end.
    void flush() { flushStage(); }

    // Buffer mode: the collected JSON and its length (null-terminated).
    const char* data() const { return heap_ ? heap_ : ""; }
    size_t size() const { return heapLen_; }

private:
    static constexpr size_t STAGE_SIZE = 1024;
    static constexpr size_t FRAG_MAX = 256;

    void flushStage() {
        if (conn_ && pos_ > 0) {
            conn_->write(reinterpret_cast<const uint8_t*>(stage_), pos_);
            pos_ = 0;
        }
    }

    // Grow the heap buffer to hold at least `need` bytes plus a null terminator.
    bool ensureHeap(size_t need) {
        if (need + 1 <= heapCap_) return true;
        size_t newCap = heapCap_ == 0 ? 2048 : heapCap_ * 2;
        while (newCap < need + 1) newCap *= 2;
        char* grown = static_cast<char*>(platform::alloc(newCap));
        if (!grown) return false;
        if (heap_) { std::memcpy(grown, heap_, heapLen_); platform::free(heap_); }
        heap_ = grown;
        heapCap_ = newCap;
        heap_[heapLen_] = 0;
        return true;
    }

    platform::TcpConnection* conn_ = nullptr;  // socket mode when non-null
    char stage_[STAGE_SIZE];
    size_t pos_ = 0;

    char* heap_ = nullptr;                     // buffer mode
    size_t heapLen_ = 0;
    size_t heapCap_ = 0;
};

class HttpServerModule : public MoonModule {
public:
    uint16_t port = 8080;

    void setScheduler(Scheduler* s) { scheduler_ = s; }
    void setUiPath(const char* path) { uiPath_ = path; }
    void setPreviewFrame(PreviewFrame* f) { previewFrame_ = f; }

    // Keep running even when "disabled" via the UI — otherwise the user has no way
    // to re-enable themselves through the same UI. The `enabled` checkbox on this
    // card has no effect; that's intentional.
    bool respectsEnabled() const override { return false; }

    void onBuildControls() override {
        controls_.addUint16("port", port);
    }

    void setup() override {
        if (!server_.open(port)) {
            std::printf("HTTP server failed to open port %u\n", port);
        }
    }

    void teardown() override {
        for (auto& ws : wsClients_) ws.close();
        server_.close();
    }

    void loop20ms() override {
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

    void loop1s() override {
        pushStateToWebSockets();
    }

private:
    platform::TcpServer server_;
    Scheduler* scheduler_ = nullptr;
    PreviewFrame* previewFrame_ = nullptr;
    const char* uiPath_ = "src/ui";

    static constexpr int MAX_WS_CLIENTS = 4;
    platform::TcpConnection wsClients_[MAX_WS_CLIENTS];

    // All JSON API responses (/api/state, /api/types, /api/system) and the WS
    // state push stream through a JsonSink — no shared fixed-size buffer.

    // XOR key for Password-control obfuscation in /api/state. NOT a secret — the
    // same value lives in src/ui/app.js (PW_XOR_KEY). This only stops the
    // password being plainly readable in a raw API response; it is trivially
    // reversible by design (see the ControlType::Password serialization).
    static constexpr uint8_t PASSWORD_XOR_KEY = 0x5A;

    // -----------------------------------------------------------------------
    // HTTP handling
    // -----------------------------------------------------------------------

    void handleConnection(platform::TcpConnection& conn) {
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

    void sendResponse(platform::TcpConnection& conn, int status, const char* contentType, const char* body) {
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

    void serveFile(platform::TcpConnection& conn, const char* filename, const char* contentType) {
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

    // -----------------------------------------------------------------------
    // JSON state
    // -----------------------------------------------------------------------

    // /api/state is streamed through a JsonSink — the module tree can be any
    // size with no fixed-buffer ceiling. The HTTP response omits Content-Length;
    // `Connection: close` tells the client the body ends at EOF.
    void serveState(platform::TcpConnection& conn) {
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

    void buildStateJson(JsonSink& sink) {
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

    void writeModuleJson(JsonSink& sink, MoonModule* mod) {
        // Per-module header: name, role, enabled, loopTimeUs (fps/ms display),
        // classSize (static C++ object bytes) + dynamicBytes (heap), controls
        const char* roleStr = roleName(mod->role());
        const char* type = mod->typeName();
        if (!type) type = "";
        sink.appendf(
            "{\"name\":\"%s\",\"type\":\"%s\",\"role\":\"%s\",\"enabled\":%s,"
            "\"loopTimeUs\":%u,\"classSize\":%u,\"dynamicBytes\":%u,\"controls\":[",
            mod->name() ? mod->name() : "",
            type,
            roleStr,
            mod->enabled() ? "true" : "false",
            static_cast<unsigned>(mod->loopTimeUs()),
            static_cast<unsigned>(mod->classSize()),
            static_cast<unsigned>(mod->dynamicBytes()));
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

    void writeControls(JsonSink& sink, MoonModule* mod) {
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
                    base64Encode(scrambled, pwLen, encoded, sizeof(encoded));
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

    // -----------------------------------------------------------------------
    // Control setter
    // -----------------------------------------------------------------------

    void handleSetControl(platform::TcpConnection& conn, const char* body) {
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
                    *static_cast<uint8_t*>(c.ptr) = static_cast<uint8_t>(v);
                    break;
                }
                case ControlType::Uint16: {
                    int v = mm::json::parseInt(body, "value");
                    *static_cast<uint16_t*>(c.ptr) = static_cast<uint16_t>(v);
                    break;
                }
                case ControlType::Int16: {
                    int v = mm::json::parseInt(body, "value");
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

    MoonModule* findModuleByName(const char* name) {
        if (!name || name[0] == 0 || !scheduler_) return nullptr;

        for (uint8_t m = 0; m < scheduler_->moduleCount(); m++) {
            auto* mod = scheduler_->module(m);
            if (!mod) continue;
            auto* found = findInTree(mod, name);
            if (found) return found;
        }
        return nullptr;
    }

    static MoonModule* findInTree(MoonModule* mod, const char* name) {
        if (mod->name() && std::strcmp(mod->name(), name) == 0) return mod;
        for (uint8_t i = 0; i < mod->childCount(); i++) {
            auto* found = findInTree(mod->child(i), name);
            if (found) return found;
        }
        return nullptr;
    }

    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // System metrics
    // -----------------------------------------------------------------------

    // /api/system is streamed through a JsonSink — no fixed-buffer ceiling, same
    // as /api/state and /api/types.
    void serveSystem(platform::TcpConnection& conn) {
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

    void writeModuleMetricsJson(JsonSink& sink, MoonModule* mod, bool& first) {
        if (!mod) return;
        sink.appendf(
            "%s{\"name\":\"%s\",\"us\":%u,\"classSize\":%u,\"heap\":%u}",
            first ? "" : ",",
            mod->name() ? mod->name() : "?",
            static_cast<unsigned>(mod->loopTimeUs()),
            static_cast<unsigned>(mod->classSize()),
            static_cast<unsigned>(mod->dynamicBytes()));
        first = false;
        for (uint8_t i = 0; i < mod->childCount(); i++) {
            writeModuleMetricsJson(sink, mod->child(i), first);
        }
    }

    // -----------------------------------------------------------------------
    // Module CRUD
    // -----------------------------------------------------------------------

    void handleAddModule(platform::TcpConnection& conn, const char* body) {
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

        // Check if module with this name already exists
        if (id[0] != 0 && findModuleByName(id)) {
            sendResponse(conn, 200, "application/json", "{\"ok\":true,\"note\":\"already exists\"}");
            return;
        }

        // Create module via factory
        auto* mod = ModuleFactory::create(typeName);
        if (!mod) {
            sendResponse(conn, 400, "application/json", "{\"error\":\"unknown type\"}");
            return;
        }
        if (id[0] != 0) mod->setName(id);

        // Find parent and add as child
        if (parentId[0] != 0) {
            auto* parent = findModuleByName(parentId);
            if (!parent) {
                delete mod;
                sendResponse(conn, 404, "application/json", "{\"error\":\"parent not found\"}");
                return;
            }
            if (!parent->addChild(mod)) {
                delete mod;
                sendResponse(conn, 400, "application/json", "{\"error\":\"parent rejected child\"}");
                return;
            }
        }

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
        if (parentId[0] != 0) {
            if (auto* parent = findModuleByName(parentId)) parent->markDirty();
        } else {
            mod->markDirty();
        }
        FilesystemModule::noteDirty();

        sendResponse(conn, 200, "application/json", "{\"ok\":true}");
    }

    void handleDeleteModule(platform::TcpConnection& conn, const char* moduleName) {
        auto* mod = findModuleByName(moduleName);
        if (!mod) {
            sendResponse(conn, 404, "application/json", "{\"error\":\"module not found\"}");
            return;
        }

        // Remove from parent
        auto* parent = mod->parent();
        if (parent) {
            parent->removeChild(mod);
        }

        // Teardown and delete
        mod->teardown();
        delete mod;

        if (scheduler_) scheduler_->rebuild();

        // Persist the new tree shape — marking the parent dirty rewrites its file
        // without the deleted child slot. Root deletes are skipped (no parent to mark);
        // the top-level shape is fixed in main.cpp anyway.
        if (parent) {
            parent->markDirty();
            FilesystemModule::noteDirty();
        }

        sendResponse(conn, 200, "application/json", "{\"ok\":true}");
    }

    // POST /api/modules/<name>/replace with body {"type":"<TypeName>"}.
    // Swaps a child's type at the same position: siblings, order, and the parent's
    // selection are preserved. The replacement starts with its own factory-default
    // control values — a clean swap, no carry-over. The engine already does this
    // internally (FilesystemModule::applyNode on a type mismatch); this exposes it
    // as an explicit user operation.
    void handleReplaceModule(platform::TcpConnection& conn, const char* moduleName, const char* body) {
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

    // GET /api/types → {"types":[{"name":"NoiseEffect","role":"effect",
    //                  "docPath":"light/effects/NoiseEffect.md","defaults":{...}}, ...]}.
    // The defaults map is captured by factory-creating a fresh probe instance per type,
    // running its onBuildControls(), and reading each bound variable's value-at-rest.
    // The probe is destroyed before the next iteration. UI uses these to render the
    // ↺ reset-to-default button (active when the live value differs) and the help
    // link (docPath, relative to docs/moonmodules/ — "" means no help link).
    // /api/types is streamed through a JsonSink — same as /api/state, no fixed-buffer
    // ceiling. The HTTP response omits Content-Length; Connection: close ends the body.
    void serveTypes(platform::TcpConnection& conn) {
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

    // Emit `"controlName":value, ...` pairs into the sink for a probe of typeName.
    // The probe is created from the factory, onBuildControls is run, and the bound
    // variables are read at their initial (just-constructed) state. The probe is deleted
    // before return. Only persistable scalar types are emitted (Uint8/Uint16/Bool/Text).
    // Text values are JSON-escaped minimally — typeName-derived control names are
    // alphanumeric so the keys are safe.
    void writeTypeDefaults(JsonSink& sink, const char* typeName) {
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

    // POST /api/modules/<name>/move with body {"to":N}. Moves the named module to
    // absolute index N within its parent's children. Triggers a full pipeline rebuild
    // because modifier/layout reorders change the LUT.
    void handleMoveModule(platform::TcpConnection& conn, const char* moduleName, const char* body) {
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

    // POST /api/reboot — restart the device. Response is sent before the restart
    // happens, but on ESP32 the device may reset before the TCP socket finishes;
    // browsers handle this via their existing WS-disconnect → reconnect logic.
    //
    // Flush pending FS writes first — otherwise a quick add-then-reboot loses the
    // pending change to the 2s save debounce.
    void handleReboot(platform::TcpConnection& conn) {
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

    // -----------------------------------------------------------------------
    // WebSocket
    // -----------------------------------------------------------------------

    void handleWebSocketUpgrade(platform::TcpConnection& conn, const char* req) {
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
        base64Encode(sha1Hash, 20, acceptKey, sizeof(acceptKey));

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

    void pushStateToWebSockets() {
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

    static bool sendWsTextFrame(platform::TcpConnection& conn, const char* data, int len) {
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


    // Broadcast the 3D preview frame to every connected browser. The send is
    // non-blocking and all-or-nothing: a backpressured browser causes the frame
    // to be skipped (WouldBlock), never a render-task stall. The preview is
    // already rate-limited by PreviewDriver's fps control.
    void broadcastPreviewFrame() {
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

    // -----------------------------------------------------------------------
    // SHA-1 (RFC 3174) — minimal implementation for WebSocket handshake
    // -----------------------------------------------------------------------

    static void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
        uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
                 h3 = 0x10325476, h4 = 0xC3D2E1F0;

        // Pad message
        size_t padLen = ((len + 8) / 64 + 1) * 64;
        uint8_t padded[512] = {};
        if (padLen > sizeof(padded)) return; // input too large for our buffer
        std::memcpy(padded, data, len);
        padded[len] = 0x80;
        uint64_t bitLen = static_cast<uint64_t>(len) * 8;
        for (int i = 0; i < 8; i++) {
            padded[padLen - 1 - i] = static_cast<uint8_t>(bitLen >> (i * 8));
        }

        for (size_t offset = 0; offset < padLen; offset += 64) {
            uint32_t w[80];
            for (int i = 0; i < 16; i++) {
                w[i] = (static_cast<uint32_t>(padded[offset + i * 4]) << 24) |
                       (static_cast<uint32_t>(padded[offset + i * 4 + 1]) << 16) |
                       (static_cast<uint32_t>(padded[offset + i * 4 + 2]) << 8) |
                        static_cast<uint32_t>(padded[offset + i * 4 + 3]);
            }
            for (int i = 16; i < 80; i++) {
                uint32_t v = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
                w[i] = (v << 1) | (v >> 31);
            }

            uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
            for (int i = 0; i < 80; i++) {
                uint32_t f, k;
                if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
                else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
                else { f = b ^ c ^ d; k = 0xCA62C1D6; }
                uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
                e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = temp;
            }
            h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
        }

        auto store32 = [](uint8_t* p, uint32_t v) {
            p[0] = static_cast<uint8_t>(v >> 24); p[1] = static_cast<uint8_t>(v >> 16);
            p[2] = static_cast<uint8_t>(v >> 8);  p[3] = static_cast<uint8_t>(v);
        };
        store32(out, h0); store32(out + 4, h1); store32(out + 8, h2);
        store32(out + 12, h3); store32(out + 16, h4);
    }

    // -----------------------------------------------------------------------
    // Escape a string for embedding inside a JSON string literal: " → \" and
    // \ → \\. Writes into `out` (no surrounding quotes). Truncates rather than
    // overflowing if the escaped form exceeds outMax.
    static void jsonEscape(const char* in, char* out, size_t outMax) {
        size_t oi = 0;
        for (; *in && oi + 2 < outMax; in++) {
            if (*in == '"' || *in == '\\') out[oi++] = '\\';
            out[oi++] = *in;
        }
        out[oi] = 0;
    }

    // Base64 encode
    // -----------------------------------------------------------------------

    static void base64Encode(const uint8_t* in, size_t inLen, char* out, size_t outMax) {
        static constexpr char table[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        size_t oi = 0;
        for (size_t i = 0; i < inLen && oi + 4 < outMax; i += 3) {
            uint32_t n = static_cast<uint32_t>(in[i]) << 16;
            if (i + 1 < inLen) n |= static_cast<uint32_t>(in[i + 1]) << 8;
            if (i + 2 < inLen) n |= static_cast<uint32_t>(in[i + 2]);
            out[oi++] = table[(n >> 18) & 0x3F];
            out[oi++] = table[(n >> 12) & 0x3F];
            out[oi++] = (i + 1 < inLen) ? table[(n >> 6) & 0x3F] : '=';
            out[oi++] = (i + 2 < inLen) ? table[n & 0x3F] : '=';
        }
        out[oi] = 0;
    }
};

} // namespace mm
