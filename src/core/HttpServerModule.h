#pragma once

#include "core/MoonModule.h"
#include "core/BinaryBroadcaster.h"
#include "platform/platform.h"

#include <cstdint>

namespace mm {

// Forward declarations — bodies in HttpServerModule.cpp include the real headers.
class JsonSink;
class Scheduler;

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
class HttpServerModule : public MoonModule, public BinaryBroadcaster {
public:
    uint16_t port = 8080;

    void setScheduler(Scheduler* s) { scheduler_ = s; }
    void setUiPath(const char* path) { uiPath_ = path; }

    // BinaryBroadcaster — stream one binary WS frame to every connected client, pushed
    // incrementally so no frame-sized buffer is held. Producers (PreviewDriver) push the
    // payload bytes; this prepends the WS header. Domain-neutral: no knowledge of the content.
    void beginBinaryFrame(size_t totalLen) override;
    void pushBinaryFrame(const uint8_t* data, size_t len) override;
    bool endBinaryFrame() override;
    // Bumped on each new WS client (see handleWebSocketUpgrade). PreviewDriver watches it to
    // re-stream its coordinate table the moment a fresh page connects, so a refresh shows the
    // preview immediately.
    uint32_t clientGeneration() const override { return wsClientGeneration_; }

    // Keep running even when "disabled" via the UI — otherwise the user has no way
    // to re-enable themselves through the same UI. The `enabled` checkbox on this
    // card has no effect; that's intentional.
    bool respectsEnabled() const override { return false; }

    void onBuildControls() override;
    void setup() override;
    void teardown() override;
    void loop20ms() override;
    void loop1s() override;

    // -----------------------------------------------------------------------
    // Transport-free apply-core — "the REST API, callable in-process"
    // -----------------------------------------------------------------------
    // The add/set/clear-children operations the HTTP handlers do, factored out of
    // the TcpConnection so any transport can drive them. Two callers today: the
    // HTTP handlers (thin wrappers that map OpResult → status code) and the Improv
    // serial path (ImprovProvisioningModule applies a pushed op on the main loop —
    // "Improv = REST over serial"). One home for the apply logic; transports differ
    // only in how they frame the request and report the result.
    enum class OpResult : uint8_t {
        Ok,
        AlreadyExists,   // add is a no-op: a module with this id is already in the tree (still success)
        ModuleNotFound,  // module / parent name not in the tree
        ControlNotFound, // module exists but has no such control (a distinct 404)
        UnknownType,     // factory doesn't know the type
        BadRequest,      // missing field, top-level add, parent rejected child
        OutOfRange,      // numeric value outside bounds
        Malformed,       // value didn't parse (e.g. IPv4)
        ReadOnly,        // tried to write a display-only control
    };
    // body is a small JSON object: {"type","id","parent_id"} / {"module","control","value"}.
    OpResult applyAddModule(const char* typeName, const char* id, const char* parentId);
    OpResult applySetControl(const char* moduleName, const char* controlName, const char* valueJson);
    // Enumerate-then-DELETE every child of `parentName` (the catalog inject's
    // replaceChildren). Returns NotFound if the parent doesn't exist, else Ok.
    OpResult applyClearChildren(const char* parentName);
    // Parse a single REST op object ({"op":"add|set|clearChildren", …}) and dispatch
    // to the three above. The wire shape the Improv APPLY_OP frame carries.
    OpResult applyOp(const char* opJson);

private:
    platform::TcpServer server_;
    Scheduler* scheduler_ = nullptr;
    const char* uiPath_ = "src/ui";

    static constexpr int MAX_WS_CLIENTS = 4;
    platform::TcpConnection wsClients_[MAX_WS_CLIENTS];
    uint32_t wsClientGeneration_ = 0;   // ++ on each new WS client; see clientGeneration()

    // begin/push/endBinaryFrame stream a binary WS frame straight to every client with NO
    // frame-sized buffer: the header goes out on begin, each pushed slice is fanned to all
    // clients, and end reports whether every client got the whole frame. A producer (PreviewDriver
    // streaming the producer buffer / forEachCoord) holds no copy. wsFrameAllSent_ tracks the
    // current frame's all-sent result across the push calls.
    bool wsFrameAllSent_ = true;
    // Max TOTAL WouldBlock spins for one span in sendAllOrClose before a stuck client is closed.
    // A healthy socket WouldBlocks only a handful of times even for a 49 KB frame (the lwIP
    // buffer drains between writes), so this is generous enough not to drop a full-res frame on a
    // good link, yet finite so a wedged client can't spin forever. (No sleep — a slow link still
    // briefly occupies the caller's loop; the resumable cross-tick send is the follow-up for that.)
    static constexpr int kDirectSendSpins = 2000;

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
    // Write the whole span to one client via repeated non-blocking writeSome; close it + return
    // false if it can't all go (a stuck/too-slow client). The push primitive behind begin/push/end.
    static bool sendAllOrClose(platform::TcpConnection& ws, const uint8_t* data, size_t len);
};

} // namespace mm
