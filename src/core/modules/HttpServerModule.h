#pragma once

#include "core/AppState.h"
#include "core/MoonModule.h"
#include "light/Layer.h"
#include "platform/TcpServer.h"
#include <cstdio>
#include <cstring>

namespace mm {

class HttpServerModule : public MoonModule {
public:
    const char* name() const override { return "HTTP Server"; }

    void setAppState(AppState* state) { appState_ = state; }
    void setUiPath(const char* path) {
        std::strncpy(uiPath_, path, sizeof(uiPath_) - 1);
        uiPath_[sizeof(uiPath_) - 1] = '\0';
    }

    void addControls() override {
        portIdx_ = addControl("port", uint16_t(8080), uint16_t(1), uint16_t(65535));
    }

    void setup() override {
        uint16_t port = control(portIdx_)->u16.value;
        server_ = platform::tcpListen(port);
        if (server_ != platform::INVALID_TCP_HANDLE) {
            std::printf("[http] listening on port %u\n", port);
        }
    }

    void teardown() override {
        platform::tcpCloseServer(server_);
        server_ = platform::INVALID_TCP_HANDLE;
    }

    void loop() override {
        if (server_ == platform::INVALID_TCP_HANDLE) return;

        // Accept one connection per loop iteration (non-blocking)
        auto client = platform::tcpAccept(server_);
        if (client == platform::INVALID_TCP_HANDLE) return;

        // Read request: headers + body
        char buf[2048] = {};
        int total = 0;
        // Read until we have headers
        for (int attempt = 0; attempt < 50 && total < static_cast<int>(sizeof(buf) - 1); ++attempt) {
            int n = platform::tcpRead(client, buf + total,
                                       sizeof(buf) - 1 - total);
            if (n > 0) total += n;
            buf[total] = '\0';
            if (std::strstr(buf, "\r\n\r\n")) break;
            if (n <= 0 && total > 0) break;
        }
        if (total <= 0) {
            platform::tcpClose(client);
            return;
        }
        // If POST, ensure we read the body too
        const char* headerEnd = std::strstr(buf, "\r\n\r\n");
        if (headerEnd) {
            const char* clHeader = std::strstr(buf, "Content-Length:");
            if (!clHeader) clHeader = std::strstr(buf, "content-length:");
            if (clHeader) {
                int contentLen = 0;
                std::sscanf(clHeader + 15, "%d", &contentLen);
                int headerSize = static_cast<int>(headerEnd + 4 - buf);
                int bodyNeeded = headerSize + contentLen;
                for (int attempt = 0; attempt < 50 && total < bodyNeeded &&
                     total < static_cast<int>(sizeof(buf) - 1); ++attempt) {
                    int n = platform::tcpRead(client, buf + total,
                                               sizeof(buf) - 1 - total);
                    if (n > 0) total += n;
                    if (n <= 0) break;
                }
            }
        }
        buf[total] = '\0';

        // Parse method and path
        char method[8] = {};
        char path[256] = {};
        std::sscanf(buf, "%7s %255s", method, path);

        if (std::strcmp(method, "GET") == 0) {
            handleGet(client, path);
        } else if (std::strcmp(method, "POST") == 0) {
            // Find body (after \r\n\r\n)
            const char* body = std::strstr(buf, "\r\n\r\n");
            if (body) body += 4; else body = "";
            handlePost(client, path, body);
        } else {
            sendResponse(client, 405, "text/plain", "Method Not Allowed");
        }

        platform::tcpClose(client);
    }

private:
    platform::TcpServerHandle server_ = platform::INVALID_TCP_HANDLE;
    AppState* appState_ = nullptr;
    char uiPath_[128] = {};
    uint8_t portIdx_ = 0;

    void handleGet(platform::TcpClientHandle client, const char* path) {
        if (std::strcmp(path, "/") == 0 || std::strcmp(path, "/index.html") == 0) {
            serveFile(client, "index.html", "text/html");
        } else if (std::strcmp(path, "/app.js") == 0) {
            serveFile(client, "app.js", "application/javascript");
        } else if (std::strcmp(path, "/style.css") == 0) {
            serveFile(client, "style.css", "text/css");
        } else if (std::strcmp(path, "/api/state") == 0) {
            serveState(client);
        } else {
            sendResponse(client, 404, "text/plain", "Not Found");
        }
    }

    void handlePost(platform::TcpClientHandle client, const char* path, const char* body) {
        if (std::strncmp(path, "/api/control/", 13) == 0) {
            handleSetControl(client, path + 13, body);
        } else if (std::strncmp(path, "/api/effect/", 12) == 0) {
            handleSwitchEffect(client, path + 12);
        } else if (std::strncmp(path, "/api/modifier/add/", 18) == 0) {
            handleAddModifier(client, path + 18);
        } else if (std::strncmp(path, "/api/modifier/remove/", 21) == 0) {
            handleRemoveModifier(client, path + 21);
        } else {
            sendResponse(client, 404, "text/plain", "Not Found");
        }
    }

    void serveFile(platform::TcpClientHandle client, const char* filename, const char* contentType) {
        char filepath[256];
        std::snprintf(filepath, sizeof(filepath), "%s/%s", uiPath_, filename);

        FILE* f = std::fopen(filepath, "rb");
        if (!f) {
            sendResponse(client, 404, "text/plain", "File Not Found");
            return;
        }

        std::fseek(f, 0, SEEK_END);
        long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);

        char header[256];
        int hlen = std::snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %ld\r\n"
            "Cache-Control: no-cache, no-store, must-revalidate\r\n"
            "Connection: close\r\n"
            "\r\n", contentType, size);
        platform::tcpWrite(client, header, hlen);

        char chunk[1024];
        while (size > 0) {
            size_t r = std::fread(chunk, 1, sizeof(chunk), f);
            if (r == 0) break;
            platform::tcpWrite(client, chunk, r);
            size -= static_cast<long>(r);
        }
        std::fclose(f);
    }

    // Serialize an array of MoonModules to JSON
    int serializeModules(char* json, size_t maxLen, int pos,
                         const char* key, MoonModule** mods, size_t count) {
        pos += std::snprintf(json + pos, maxLen - pos, "\"%s\":[", key);
        for (size_t i = 0; i < count; ++i) {
            if (i > 0) pos += std::snprintf(json + pos, maxLen - pos, ",");
            auto* mod = mods[i];
            pos += std::snprintf(json + pos, maxLen - pos,
                "{\"index\":%zu,\"name\":\"%s\",\"controls\":[", i, mod->name());
            for (uint8_t c = 0; c < mod->controlCount(); ++c) {
                if (c > 0) pos += std::snprintf(json + pos, maxLen - pos, ",");
                auto* ctrl = mod->control(c);
                pos += std::snprintf(json + pos, maxLen - pos,
                    "{\"name\":\"%s\",\"type\":%u", ctrl->name, static_cast<unsigned>(ctrl->type));
                if (ctrl->type == ControlType::Uint16) {
                    pos += std::snprintf(json + pos, maxLen - pos,
                        ",\"value\":%u,\"min\":%u,\"max\":%u",
                        ctrl->u16.value, ctrl->u16.min, ctrl->u16.max);
                } else if (ctrl->type == ControlType::Bool) {
                    pos += std::snprintf(json + pos, maxLen - pos,
                        ",\"value\":%s", ctrl->b.value ? "true" : "false");
                } else if (ctrl->type == ControlType::Text) {
                    pos += std::snprintf(json + pos, maxLen - pos,
                        ",\"value\":\"%s\"", ctrl->text.value);
                }
                pos += std::snprintf(json + pos, maxLen - pos, "}");
            }
            pos += std::snprintf(json + pos, maxLen - pos, "]}");
        }
        pos += std::snprintf(json + pos, maxLen - pos, "]");
        return pos;
    }

    void serveState(platform::TcpClientHandle client) {
        char json[4096] = {};
        int pos = 0;
        pos += std::snprintf(json + pos, sizeof(json) - pos, "{");

        if (appState_) {
            // Layers with their active effects and modifiers
            pos += std::snprintf(json + pos, sizeof(json) - pos, "\"layers\":[");
            if (appState_->layers) {
                for (size_t i = 0; i < appState_->layerCount; ++i) {
                    if (i > 0) pos += std::snprintf(json + pos, sizeof(json) - pos, ",");
                    auto& lr = appState_->layers[i];
                    pos += std::snprintf(json + pos, sizeof(json) - pos,
                        "{\"index\":%zu,\"bufferSize\":%zu,\"effects\":[",
                        i, lr.buffer().count());
                    // List active effect names
                    for (uint8_t e = 0; e < lr.effectCount(); ++e) {
                        if (e > 0) pos += std::snprintf(json + pos, sizeof(json) - pos, ",");
                        pos += std::snprintf(json + pos, sizeof(json) - pos,
                            "\"%s\"", lr.effect(e)->name());
                    }
                    pos += std::snprintf(json + pos, sizeof(json) - pos, "],\"modifiers\":[");
                    for (uint8_t m = 0; m < lr.modifierCount(); ++m) {
                        if (m > 0) pos += std::snprintf(json + pos, sizeof(json) - pos, ",");
                        pos += std::snprintf(json + pos, sizeof(json) - pos,
                            "\"%s\"", lr.modifier(m)->name());
                    }
                    pos += std::snprintf(json + pos, sizeof(json) - pos, "]}");
                }
            }
            pos += std::snprintf(json + pos, sizeof(json) - pos, "],");

            // Available effects
            pos = serializeModules(json, sizeof(json), pos,
                "effects", appState_->availableEffects, appState_->effectCount);
            pos += std::snprintf(json + pos, sizeof(json) - pos, ",");

            // Available modifiers
            pos = serializeModules(json, sizeof(json), pos,
                "modifiers", appState_->availableModifiers, appState_->modifierCount);
            pos += std::snprintf(json + pos, sizeof(json) - pos, ",");

            // Layouts
            pos = serializeModules(json, sizeof(json), pos,
                "layouts", appState_->availableLayouts, appState_->layoutCount);
            pos += std::snprintf(json + pos, sizeof(json) - pos, ",");

            // Drivers
            pos = serializeModules(json, sizeof(json), pos,
                "drivers", appState_->availableDrivers, appState_->driverCount);
        }

        pos += std::snprintf(json + pos, sizeof(json) - pos, "}");
        sendResponse(client, 200, "application/json", json);
    }

    // Find a module by type/index: "effect/0", "layout/1", "driver/0"
    MoonModule* findModule(const char* typeAndIndex) {
        if (!appState_) return nullptr;
        char type[16] = {};
        unsigned idx = 0;
        if (std::sscanf(typeAndIndex, "%15[^/]/%u", type, &idx) != 2) return nullptr;

        if (std::strcmp(type, "effect") == 0 && idx < appState_->effectCount)
            return appState_->availableEffects[idx];
        if (std::strcmp(type, "modifier") == 0 && idx < appState_->modifierCount)
            return appState_->availableModifiers[idx];
        if (std::strcmp(type, "layout") == 0 && idx < appState_->layoutCount)
            return appState_->availableLayouts[idx];
        if (std::strcmp(type, "driver") == 0 && idx < appState_->driverCount)
            return appState_->availableDrivers[idx];
        return nullptr;
    }

    void handleSetControl(platform::TcpClientHandle client, const char* pathRest, const char* body) {
        // Path: {type}/{moduleIndex}/{controlIndex}
        // e.g. effect/0/0, layout/0/1, driver/0/0
        char typeAndModule[32] = {};
        unsigned controlIdx = 0;
        // Extract "type/moduleIdx" and controlIdx
        const char* lastSlash = std::strrchr(pathRest, '/');
        if (!lastSlash) {
            sendResponse(client, 400, "text/plain", "Bad path");
            return;
        }
        size_t prefixLen = static_cast<size_t>(lastSlash - pathRest);
        if (prefixLen >= sizeof(typeAndModule)) prefixLen = sizeof(typeAndModule) - 1;
        std::memcpy(typeAndModule, pathRest, prefixLen);
        typeAndModule[prefixLen] = '\0';
        std::sscanf(lastSlash + 1, "%u", &controlIdx);

        auto* mod = findModule(typeAndModule);
        if (!mod) {
            sendResponse(client, 404, "text/plain", "Module not found");
            return;
        }

        // Parse value from body (simple: look for "value":)
        const char* valStr = std::strstr(body, "\"value\":");
        if (!valStr) {
            sendResponse(client, 400, "text/plain", "Missing value");
            return;
        }
        valStr += 8; // skip "value":

        auto* ctrl = mod->control(static_cast<uint8_t>(controlIdx));
        if (!ctrl) {
            sendResponse(client, 404, "text/plain", "Control not found");
            return;
        }

        if (ctrl->type == ControlType::Uint16) {
            unsigned v = 0;
            std::sscanf(valStr, "%u", &v);
            mod->setControl(static_cast<uint8_t>(controlIdx), static_cast<uint16_t>(v));
        } else if (ctrl->type == ControlType::Bool) {
            bool v = (std::strstr(valStr, "true") != nullptr);
            mod->setControl(static_cast<uint8_t>(controlIdx), v);
        } else if (ctrl->type == ControlType::Text) {
            // Extract string between quotes
            const char* start = std::strchr(valStr, '"');
            if (start) {
                start++;
                const char* end = std::strchr(start, '"');
                if (end) {
                    char tmp[64] = {};
                    size_t len = static_cast<size_t>(end - start);
                    if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
                    std::memcpy(tmp, start, len);
                    mod->setControl(static_cast<uint8_t>(controlIdx), tmp);
                }
            }
        }

        sendResponse(client, 200, "application/json", "{\"ok\":true}");
    }

    void handleSwitchEffect(platform::TcpClientHandle client, const char* pathRest) {
        // Path: {layerIndex}/{effectIndex}
        unsigned layerIdx = 0, effectIdx = 0;
        if (std::sscanf(pathRest, "%u/%u", &layerIdx, &effectIdx) != 2) {
            sendResponse(client, 400, "text/plain", "Bad path");
            return;
        }

        if (!appState_ || layerIdx >= appState_->layerCount ||
            effectIdx >= appState_->effectCount) {
            sendResponse(client, 404, "text/plain", "Not found");
            return;
        }

        appState_->layers[layerIdx].setEffect(appState_->availableEffects[effectIdx]);
        sendResponse(client, 200, "application/json", "{\"ok\":true}");
    }

    void handleAddModifier(platform::TcpClientHandle client, const char* pathRest) {
        // Path: {layerIndex}/{modifierIndex}
        unsigned layerIdx = 0, modIdx = 0;
        if (std::sscanf(pathRest, "%u/%u", &layerIdx, &modIdx) != 2) {
            sendResponse(client, 400, "text/plain", "Bad path");
            return;
        }
        if (!appState_ || layerIdx >= appState_->layerCount ||
            modIdx >= appState_->modifierCount) {
            sendResponse(client, 404, "text/plain", "Not found");
            return;
        }
        appState_->layers[layerIdx].addModifier(appState_->availableModifiers[modIdx]);
        appState_->layers[layerIdx].rebuildLUT();
        sendResponse(client, 200, "application/json", "{\"ok\":true}");
    }

    void handleRemoveModifier(platform::TcpClientHandle client, const char* pathRest) {
        // Path: {layerIndex} — clears all modifiers
        unsigned layerIdx = 0;
        if (std::sscanf(pathRest, "%u", &layerIdx) != 1) {
            sendResponse(client, 400, "text/plain", "Bad path");
            return;
        }
        if (!appState_ || layerIdx >= appState_->layerCount) {
            sendResponse(client, 404, "text/plain", "Not found");
            return;
        }
        appState_->layers[layerIdx].clearModifiers();
        appState_->layers[layerIdx].rebuildLUT();
        sendResponse(client, 200, "application/json", "{\"ok\":true}");
    }

    void sendResponse(platform::TcpClientHandle client, int status,
                      const char* contentType, const char* body) {
        const char* statusText = (status == 200) ? "OK" :
                                 (status == 404) ? "Not Found" :
                                 (status == 400) ? "Bad Request" :
                                 (status == 405) ? "Method Not Allowed" : "Error";
        size_t bodyLen = std::strlen(body);
        char header[256];
        int hlen = std::snprintf(header, sizeof(header),
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Cache-Control: no-cache, no-store, must-revalidate\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n"
            "\r\n", status, statusText, contentType, bodyLen);
        platform::tcpWrite(client, header, hlen);
        platform::tcpWrite(client, body, bodyLen);
    }
};

} // namespace mm
