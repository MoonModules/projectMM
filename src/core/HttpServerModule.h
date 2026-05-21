#pragma once

#include "core/MoonModule.h"
#include "core/Scheduler.h"
#include "core/PreviewFrame.h"
#include "core/ModuleFactory.h"
#include "core/JsonUtil.h"
#include "core/FilesystemModule.h"
#include "platform/platform.h"

#include "ui/ui_embedded.h"

#include <cstdio>
#include <cstring>
#include <cstdint>

namespace mm {

class HttpServerModule : public MoonModule {
public:
    uint16_t port = 8080;

    void setScheduler(Scheduler* s) { scheduler_ = s; }
    void setUiPath(const char* path) { uiPath_ = path; }
    void setPreviewFrame(PreviewFrame* f) { previewFrame_ = f; }

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

    // Shared JSON buffer for API responses (one request at a time, so safe to share)
    static constexpr size_t JSON_BUF_SIZE = 4096;
    static inline char jsonBuf_[JSON_BUF_SIZE];

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
            else if (std::strcmp(path, "/api/state") == 0) serveState(conn);
            else if (std::strcmp(path, "/api/system") == 0) serveSystem(conn);
            else sendResponse(conn, 404, "text/plain", "Not found");
        } else if (std::strcmp(method, "POST") == 0) {
            if (std::strcmp(path, "/api/control") == 0 && body) {
                handleSetControl(conn, body);
            } else if (std::strcmp(path, "/api/modules") == 0 && body) {
                handleAddModule(conn, body);
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

    void serveState(platform::TcpConnection& conn) {
        int len = buildStateJson(jsonBuf_, JSON_BUF_SIZE);
        sendResponse(conn, 200, "application/json", jsonBuf_);
        (void)len;
    }

    int buildStateJson(char* buf, size_t bufSize) {
        int pos = 0;
        auto append = [&](const char* s) {
            if (static_cast<size_t>(pos) >= bufSize) return;
            int n = std::snprintf(buf + pos, bufSize - pos, "%s", s);
            if (n > 0 && static_cast<size_t>(pos + n) < bufSize) pos += n;
        };

        append("{\"modules\":[");

        if (scheduler_) {
            for (uint8_t m = 0; m < scheduler_->moduleCount(); m++) {
                auto* mod = scheduler_->module(m);
                if (!mod || mod == this) continue; // skip self
                if (m > 0 && buf[pos - 1] != '[') append(",");
                writeModuleJson(buf, bufSize, pos, mod);
            }
        }

        append("]}");
        buf[pos] = 0;
        return pos;
    }

    void writeModuleJson(char* buf, size_t bufSize, int& pos, MoonModule* mod) {
        auto append = [&](const char* s) {
            if (static_cast<size_t>(pos) >= bufSize) return;
            int n = std::snprintf(buf + pos, bufSize - pos, "%s", s);
            if (n > 0 && static_cast<size_t>(pos + n) < bufSize) pos += n;
        };

        if (static_cast<size_t>(pos) >= bufSize) return;
        int n = std::snprintf(buf + pos, bufSize - pos,
            "{\"name\":\"%s\",\"enabled\":%s,\"controls\":[",
            mod->name() ? mod->name() : "",
            mod->enabled() ? "true" : "false");
        if (n > 0 && static_cast<size_t>(pos + n) < bufSize) pos += n;
        writeControls(buf, bufSize, pos, mod);
        append("]");

        // Children
        uint8_t cc = mod->childCount();
        if (cc > 0) {
            append(",\"children\":[");
            for (uint8_t i = 0; i < cc; i++) {
                if (i > 0) append(",");
                writeModuleJson(buf, bufSize, pos, mod->child(i));
            }
            append("]");
        }

        append("}");
    }

    void writeControls(char* buf, size_t bufSize, int& pos, MoonModule* mod) {
        auto& ctrls = mod->controls();
        for (uint8_t i = 0; i < ctrls.count(); i++) {
            if (i > 0) {
                int n = std::snprintf(buf + pos, bufSize - pos, ",");
                if (n > 0 && static_cast<size_t>(pos + n) < bufSize) pos += n;
            }
            auto& c = ctrls[i];
            // Per-type body emitted WITHOUT the closing }. We append "hidden" then } afterwards.
            int n = 0;
            switch (c.type) {
                case ControlType::Uint8:
                    n = std::snprintf(buf + pos, bufSize - pos,
                        "{\"name\":\"%s\",\"type\":\"uint8\",\"value\":%u,\"min\":%u,\"max\":%u",
                        c.name, *static_cast<uint8_t*>(c.ptr), c.min, c.max);
                    break;
                case ControlType::Uint16:
                    n = std::snprintf(buf + pos, bufSize - pos,
                        "{\"name\":\"%s\",\"type\":\"uint16\",\"value\":%u",
                        c.name, *static_cast<uint16_t*>(c.ptr));
                    break;
                case ControlType::Bool:
                    n = std::snprintf(buf + pos, bufSize - pos,
                        "{\"name\":\"%s\",\"type\":\"bool\",\"value\":%s",
                        c.name, *static_cast<bool*>(c.ptr) ? "true" : "false");
                    break;
                case ControlType::Text:
                    n = std::snprintf(buf + pos, bufSize - pos,
                        "{\"name\":\"%s\",\"type\":\"text\",\"value\":\"%s\"",
                        c.name, static_cast<char*>(c.ptr));
                    break;
                case ControlType::ReadOnly:
                    n = std::snprintf(buf + pos, bufSize - pos,
                        "{\"name\":\"%s\",\"type\":\"display\",\"value\":\"%s\"",
                        c.name, static_cast<char*>(c.ptr));
                    break;
                case ControlType::Select: {
                    n = std::snprintf(buf + pos, bufSize - pos,
                        "{\"name\":\"%s\",\"type\":\"select\",\"value\":%u,\"options\":[",
                        c.name, *static_cast<uint8_t*>(c.ptr));
                    if (n > 0 && static_cast<size_t>(pos + n) < bufSize) pos += n;
                    auto* options = reinterpret_cast<const char* const*>(c.aux);
                    for (uint8_t o = 0; o < c.max; o++) {
                        n = std::snprintf(buf + pos, bufSize - pos,
                            "%s\"%s\"", o > 0 ? "," : "", options[o]);
                        if (n > 0 && static_cast<size_t>(pos + n) < bufSize) pos += n;
                    }
                    n = std::snprintf(buf + pos, bufSize - pos, "]");
                    break;
                }
                case ControlType::Progress:
                    n = std::snprintf(buf + pos, bufSize - pos,
                        "{\"name\":\"%s\",\"type\":\"progress\",\"value\":%lu,\"total\":%lu",
                        c.name, static_cast<unsigned long>(*static_cast<uint32_t*>(c.ptr)),
                        static_cast<unsigned long>(c.aux));
                    break;
            }
            if (n > 0 && static_cast<size_t>(pos + n) < bufSize) pos += n;
            // Emit "hidden":true only when set (common case is false; omit to save bytes).
            // Then close the per-control object.
            if (c.hidden) {
                n = std::snprintf(buf + pos, bufSize - pos, ",\"hidden\":true}");
            } else {
                n = std::snprintf(buf + pos, bufSize - pos, "}");
            }
            if (n > 0 && static_cast<size_t>(pos + n) < bufSize) pos += n;
        }
    }

    // -----------------------------------------------------------------------
    // Control setter
    // -----------------------------------------------------------------------

    void handleSetControl(platform::TcpConnection& conn, const char* body) {
        // Parse: {"module":"Noise","control":"scale","value":8}
        char moduleName[32] = {};
        char controlName[32] = {};
        parseJsonString(body, "module", moduleName, sizeof(moduleName));
        parseJsonString(body, "control", controlName, sizeof(controlName));

        // Find the module by name
        MoonModule* target = findModuleByName(moduleName);
        if (!target) {
            sendResponse(conn, 404, "application/json", "{\"error\":\"module not found\"}");
            return;
        }

        // Handle module-level "enabled" property
        if (std::strcmp(controlName, "enabled") == 0) {
            target->setEnabled(parseJsonBool(body, "value"));
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
                    int v = parseJsonInt(body, "value");
                    *static_cast<uint8_t*>(c.ptr) = static_cast<uint8_t>(v);
                    break;
                }
                case ControlType::Uint16: {
                    int v = parseJsonInt(body, "value");
                    *static_cast<uint16_t*>(c.ptr) = static_cast<uint16_t>(v);
                    break;
                }
                case ControlType::Bool: {
                    bool v = parseJsonBool(body, "value");
                    *static_cast<bool*>(c.ptr) = v;
                    break;
                }
                case ControlType::Text: {
                    char v[64] = {};
                    parseJsonString(body, "value", v, sizeof(v));
                    uint8_t maxLen = c.max > 0 ? c.max - 1 : 15;
                    std::strncpy(static_cast<char*>(c.ptr), v, maxLen);
                    static_cast<char*>(c.ptr)[maxLen] = '\0';
                    break;
                }
                case ControlType::Select: {
                    int v = parseJsonInt(body, "value");
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

    void serveSystem(platform::TcpConnection& conn) {
        int pos = std::snprintf(jsonBuf_, JSON_BUF_SIZE,
            "{\"fps\":%u,\"tickTimeUs\":%u,\"freeHeap\":%u,\"freeInternal\":%u,\"maxBlock\":%u,\"uptime\":%u,\"modules\":[",
            static_cast<unsigned>(scheduler_ ? scheduler_->fps() : 0),
            static_cast<unsigned>(scheduler_ ? scheduler_->tickTimeUs() : 0),
            static_cast<unsigned>(platform::freeHeap()),
            static_cast<unsigned>(platform::freeInternalHeap()),
            static_cast<unsigned>(platform::maxAllocBlock()),
            static_cast<unsigned>(scheduler_ ? scheduler_->elapsed() / 1000 : 0));

        // Per-module timing (walk tree recursively)
        if (scheduler_ && pos > 0 && static_cast<size_t>(pos) < JSON_BUF_SIZE) {
            bool first = true;
            for (uint8_t i = 0; i < scheduler_->moduleCount(); i++) {
                writeModuleMetricsJson(jsonBuf_, JSON_BUF_SIZE, pos, scheduler_->module(i), first);
            }
        }

        int n = std::snprintf(jsonBuf_ + pos, JSON_BUF_SIZE - pos, "]}");
        if (n > 0 && static_cast<size_t>(pos + n) < JSON_BUF_SIZE) pos += n;

        sendResponse(conn, 200, "application/json", jsonBuf_);
    }

    void writeModuleMetricsJson(char* buf, size_t bufSize, int& pos, MoonModule* mod, bool& first) {
        if (!mod || static_cast<size_t>(pos) >= bufSize) return;
        int n = std::snprintf(buf + pos, bufSize - pos,
            "%s{\"name\":\"%s\",\"us\":%u,\"classSize\":%u,\"heap\":%u}",
            first ? "" : ",",
            mod->name() ? mod->name() : "?",
            static_cast<unsigned>(mod->loopTimeUs()),
            static_cast<unsigned>(mod->classSize()),
            static_cast<unsigned>(mod->dynamicBytes()));
        if (n > 0 && static_cast<size_t>(pos + n) < bufSize) pos += n;
        first = false;
        for (uint8_t i = 0; i < mod->childCount(); i++) {
            writeModuleMetricsJson(buf, bufSize, pos, mod->child(i), first);
        }
    }

    // -----------------------------------------------------------------------
    // Module CRUD
    // -----------------------------------------------------------------------

    void handleAddModule(platform::TcpConnection& conn, const char* body) {
        char typeName[32] = {};
        char id[32] = {};
        char parentId[32] = {};
        parseJsonString(body, "type", typeName, sizeof(typeName));
        parseJsonString(body, "id", id, sizeof(id));
        parseJsonString(body, "parent_id", parentId, sizeof(parentId));

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

        // Lifecycle: setup the new module
        mod->setup();
        mod->onBuildControls();
        mod->onAllocateMemory();

        if (scheduler_) scheduler_->rebuild();

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

        sendResponse(conn, 200, "application/json", "{\"ok\":true}");
    }

    // JSON parsing delegates to core/JsonUtil.h. Kept as thin wrappers so existing call
    // sites read unchanged.
    static void parseJsonString(const char* json, const char* key, char* out, size_t maxLen) {
        mm::json::parseString(json, key, out, maxLen);
    }
    static int parseJsonInt(const char* json, const char* key) {
        return mm::json::parseInt(json, key);
    }
    static bool parseJsonBool(const char* json, const char* key) {
        return mm::json::parseBool(json, key);
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

        char json[4096];
        int len = buildStateJson(json, sizeof(json));

        for (auto& ws : wsClients_) {
            if (!ws.valid()) continue;
            if (!sendWsTextFrame(ws, json, len)) {
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

    static bool sendWsBinaryFrame(platform::TcpConnection& conn, const uint8_t* data, size_t len) {
        uint8_t header[10];
        int headerLen = 0;

        header[0] = 0x82; // FIN + binary opcode
        if (len < 126) {
            header[1] = static_cast<uint8_t>(len);
            headerLen = 2;
        } else if (len < 65536) {
            header[1] = 126;
            header[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
            header[3] = static_cast<uint8_t>(len & 0xFF);
            headerLen = 4;
        } else {
            return false;
        }

        if (!conn.write(header, headerLen)) return false;
        return conn.write(data, len);
    }

    void broadcastPreviewFrame() {
        if (!previewFrame_ || !previewFrame_->data || previewFrame_->dataLen == 0) return;

        // Build 7-byte preview header on stack: [0x02][w16][h16][d16]
        uint8_t previewHeader[7];
        previewHeader[0] = 0x02;
        previewHeader[1] = static_cast<uint8_t>(previewFrame_->width & 0xFF);
        previewHeader[2] = static_cast<uint8_t>(previewFrame_->width >> 8);
        previewHeader[3] = static_cast<uint8_t>(previewFrame_->height & 0xFF);
        previewHeader[4] = static_cast<uint8_t>(previewFrame_->height >> 8);
        previewHeader[5] = static_cast<uint8_t>(previewFrame_->depth & 0xFF);
        previewHeader[6] = static_cast<uint8_t>(previewFrame_->depth >> 8);

        size_t totalLen = 7 + previewFrame_->dataLen;

        for (auto& ws : wsClients_) {
            if (!ws.valid()) continue;
            // Send WebSocket frame header + preview header + buffer data (3 writes, zero copy)
            if (!sendWsBinaryFrameMulti(ws, previewHeader, 7, previewFrame_->data, previewFrame_->dataLen, totalLen)) {
                ws.close();
            }
        }
    }

    static bool sendWsBinaryFrameMulti(platform::TcpConnection& conn,
                                        const uint8_t* data1, size_t len1,
                                        const uint8_t* data2, size_t len2,
                                        size_t totalLen) {
        uint8_t header[10];
        int headerLen = 0;

        header[0] = 0x82; // FIN + binary opcode
        if (totalLen < 126) {
            header[1] = static_cast<uint8_t>(totalLen);
            headerLen = 2;
        } else if (totalLen < 65536) {
            header[1] = 126;
            header[2] = static_cast<uint8_t>((totalLen >> 8) & 0xFF);
            header[3] = static_cast<uint8_t>(totalLen & 0xFF);
            headerLen = 4;
        } else {
            return false;
        }

        if (!conn.write(header, headerLen)) return false;
        if (!conn.write(data1, len1)) return false;
        return conn.write(data2, len2);
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
