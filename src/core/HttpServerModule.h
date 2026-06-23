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

    // BinaryBroadcaster — send a binary WS frame to every connected client.
    // Producers (PreviewDriver) build the payload chunks; this prepends the WS
    // header. Domain-neutral: no knowledge of what the bytes carry.
    bool broadcastBinary(const platform::WriteChunk* payload, int chunkCount) override;
    // Bumped on each new WS client (see handleWebSocketUpgrade). PreviewDriver watches
    // it to re-send its coordinate table the moment a fresh page connects, so a refresh
    // shows the preview immediately instead of waiting for the next ~1 Hz re-broadcast.
    uint32_t clientGeneration() const override { return wsClientGeneration_; }
    uint16_t lastDrainTicks() const override { return wsPreviewLastDrainTicks_; }

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

    // Live-preview send queue. broadcastBinary() copies ONE in-flight frame here and
    // returns — it never blocks the render task on the socket. drainWsSends() (called from
    // loop20ms, the transport poll) flushes it across ticks as each client's socket accepts
    // bytes. The frame is staged ONCE (one buffer, not ×clients — the no-PSRAM RAM budget);
    // the only per-client state is a sent-byte offset, so the same staged frame fans out to
    // every connected client. Backpressure is PER CLIENT: broadcastBinary refuses a new frame
    // while ANY client is still draining the previous one (the buffer is single — we can't
    // overwrite it mid-send), so a slow browser drops frames (its offset just lags) without
    // ever stalling the tick or overflowing. This producer→consumer handoff is the seam the
    // architecture's two-task split (§145) will host on the consumer/network task.
    uint8_t* wsPreviewBuf_ = nullptr;             // owned; WS header + payload of one frame
    size_t   wsPreviewCap_ = 0;                   // allocated capacity (bytes)
    size_t   wsPreviewLen_ = 0;                   // bytes of the current frame (0 = idle/empty)
    size_t   wsPreviewSent_[MAX_WS_CLIENTS] = {}; // per-client bytes already written
    uint16_t wsPreviewAge_ = 0;                   // consecutive NO-PROGRESS drain ticks
    uint16_t wsPreviewDrainTicks_ = 0;            // ticks the CURRENT frame has been draining
    uint16_t wsPreviewLastDrainTicks_ = 1;        // ticks the last COMPLETED frame took (lastDrainTicks)
    // Stuck-client guard: counts only drain ticks where NOT ONE byte moved (any progress
    // resets it), so a big frame on a healthy link legitimately spans many ticks. If a client
    // wedges (TCP window stuck, never erroring) and nothing moves for this many ticks, the
    // frame is abandoned so the preview never freezes for everyone; the lagging client resyncs
    // on the next self-contained frame. ~3 s of ZERO progress at 20 ms/tick — long enough that
    // a momentarily-busy (rendering-bound) browser, which still reads a little each tick, is
    // never killed; only a genuinely wedged socket trips it.
    static constexpr uint16_t kPreviewMaxDrainTicks = 150;

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
    // Flush the live-preview staging buffer to its client a slice at a time (non-blocking).
    // Called each loop20ms; finishes a frame across as many ticks as the socket needs.
    void drainWsSends();
};

} // namespace mm
