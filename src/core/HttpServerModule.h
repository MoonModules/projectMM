#pragma once

#include "core/MoonModule.h"
#include "platform/platform.h"

#include <cstdint>

namespace mm {

// Forward declarations — bodies in HttpServerModule.cpp include the real headers.
class JsonSink;
class Scheduler;
struct PreviewFrame;

// HttpServerModule serves the UI's REST API, the static UI assets, and the
// WebSocket channel that pushes state JSON + binary preview frames. Implementation
// lives in HttpServerModule.cpp — this header is the interface only.
//
// The five `JsonSink&` helpers below are private members rather than free
// functions because they all read `this->wsClients_`, `this->scheduler_`, or
// other module state, or call other HttpServerModule members.
//
// Three pieces of this module's helpers used to live inline here and have
// been extracted into their own headers:
//   - JsonSink class + jsonEscape() → core/JsonSink.h
//   - sha1() (RFC 3174, WS handshake) → core/Sha1.h
//   - base64Encode() (WS handshake + Password obfuscation) → core/Base64.h
// They live in `namespace mm` so the call sites in HttpServerModule.cpp are
// unchanged.
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

    void onBuildControls() override;
    void setup() override;
    void teardown() override;
    void loop20ms() override;
    void loop1s() override;

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
    void handleConnection(platform::TcpConnection& conn);
    void sendResponse(platform::TcpConnection& conn, int status, const char* contentType, const char* body);
    void sendPreflightResponse(platform::TcpConnection& conn);
    void serveFile(platform::TcpConnection& conn, const char* filename, const char* contentType);

    // -----------------------------------------------------------------------
    // JSON state
    // -----------------------------------------------------------------------
    void serveState(platform::TcpConnection& conn);
    void buildStateJson(JsonSink& sink);
    void writeModuleJson(JsonSink& sink, MoonModule* mod);
    void writeControls(JsonSink& sink, MoonModule* mod);
    // Emit `,"status":"…","severity":"…"` for a module that has a status set;
    // no-op when status is null. Shared by writeModuleJson (/api/state) and
    // writeModuleMetricsJson (/api/system) so the two endpoints stay in sync.
    static void writeStatus(JsonSink& sink, MoonModule* mod);

    // -----------------------------------------------------------------------
    // Control setter
    // -----------------------------------------------------------------------
    void handleSetControl(platform::TcpConnection& conn, const char* body);

    // Find a module anywhere in the scheduler's tree by its name. DFS, first match.
    MoonModule* findModuleByName(const char* name);
    static MoonModule* findInTree(MoonModule* mod, const char* name);

    // -----------------------------------------------------------------------
    // System metrics
    // -----------------------------------------------------------------------
    void serveSystem(platform::TcpConnection& conn);
    void writeModuleMetricsJson(JsonSink& sink, MoonModule* mod, bool& first);

    // -----------------------------------------------------------------------
    // Module CRUD
    // -----------------------------------------------------------------------
    void handleAddModule(platform::TcpConnection& conn, const char* body);
    void handleDeleteModule(platform::TcpConnection& conn, const char* moduleName);
    void handleReplaceModule(platform::TcpConnection& conn, const char* moduleName, const char* body);
    void serveTypes(platform::TcpConnection& conn);
    void writeTypeDefaults(JsonSink& sink, const char* typeName);
    void handleMoveModule(platform::TcpConnection& conn, const char* moduleName, const char* body);
    void handleReboot(platform::TcpConnection& conn);
    // OTA: POST /api/firmware/url body={"url":"..."}. Body parsed; URL handed
    // to platform::http_fetch_to_ota which spawns a task and returns. Caller
    // gets 202 immediately; progress streams via FirmwareUpdateModule controls.
    void handleFirmwareUrl(platform::TcpConnection& conn, const char* body);

    // -----------------------------------------------------------------------
    // WebSocket
    // -----------------------------------------------------------------------
    void handleWebSocketUpgrade(platform::TcpConnection& conn, const char* req);
    void pushStateToWebSockets();
    static bool sendWsTextFrame(platform::TcpConnection& conn, const char* data, int len);
    void broadcastPreviewFrame();
};

} // namespace mm
