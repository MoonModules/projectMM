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
#include "light/Palette.h"          // Palettes::nearestForHue — maps HA's RGB colour picker onto our
                                    // hue→palette convention (same core→light bridge MqttModule uses
                                    // for hsv/set; see the note in MqttModule.cpp:7-14).
#include "light/drivers/Drivers.h"  // Drivers::latestSummary() — the real light count/channels for
                                    // the WLED /json shim (same one-narrow-reach as Palette above).
#include "platform/platform.h"
#include "ui/ui_embedded.h"

#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>   // strtol — bounded Content-Length parse
#include <cerrno>    // errno / ERANGE — Content-Length overflow check
#include <cstring>
#include <cstdint>

namespace mm {

void HttpServerModule::defineControls() {
    controls_.addUint16("port", port);
}

void HttpServerModule::setup() {
    instance_ = this;
    if (!server_.open(port)) {
        std::printf("HTTP server failed to open port %u\n", port);
    }
    // Any module's rebuildControls() (a schema change: hidden flags / option sets, from a control
    // set, a list mutation, or an async WiFi/Hue callback) now flips the WS full-resync flag through
    // this static hook, so a metadata change the value-patch can't carry still reaches every client.
    MoonModule::setSchemaChangedHook(&HttpServerModule::onSchemaChanged);
}

// Static schema-changed sink (see setup): route a module's rebuildControls() signal to the one
// live HttpServerModule's resync flag. instance_ mirrors the FilesystemModule::noteDirty pattern.
void HttpServerModule::onSchemaChanged() {
    if (instance_) instance_->requestFullResync();
}

void HttpServerModule::release() {
    // Drop any in-flight send before the clients go (frees an owned state-frame body; a preview frame
    // borrows its buffer, nothing to free) — the same self-safe drop cancelBufferedSend() does.
    cancelBufferedSend();
    for (auto& ws : wsClients_) ws.close();
    server_.close();
    if (instance_ == this) { MoonModule::setSchemaChangedHook(nullptr); instance_ = nullptr; }
    MoonModule::release();   // chain: uniform override-and-chain (no buffers/children today, but the convention holds)
}

void HttpServerModule::tick20ms() {
    // Drain the in-flight resumable preview frame on the TRANSPORT-poll cadence (20 ms), NOT the
    // per-render-tick tick(): pushing frame bytes to the socket must not be charged to the LED
    // render hot path. The render tick stays free of preview work; the preview frame rate is
    // bounded by this 20 ms drain cadence (a few fps at large full-res frames) — an acceptable
    // trade, since the preview is a *view* and the LEDs are not. This drain is the consumer-side
    // transport step, kept as a standalone call so it sits cleanly on the render/transport seam
    // (architecture.md § Parallelism). Drain BEFORE accept so a connection burst can't starve an
    // active send. No-op when nothing is in flight.
    drainPreviewSend();
    // Fast-path a PENDING FULL RESYNC on the 20 ms cadence instead of waiting for the 1 s tick: a
    // fresh WS connect (or a structural change) sets fullResyncPending_, and the client shows NOTHING
    // until the full state arrives — including no preview, since a preview frame can't take the shared
    // send slot before the state does. Gated on the flag, so this is a rare event (a connect), not a
    // per-20 ms serialize: the expensive buildStateJson runs only when a resync is actually pending,
    // and the steady-state value patch stays on tick1s (unchanged). Cuts connect→first-preview latency
    // from up to ~1 s + drain down to a few tens of ms. No-op in the common (no-resync) case.
    if (fullResyncPending_) pushStateToWebSockets();
    // Read any inbound WS frames: the native WLED app SETS state (its on/off + brightness
    // slider) by SENDING a {on,bri} text frame over /ws, not by HTTP POST — so we must read
    // the socket, not only push to it. Cheap (non-blocking, usually nothing pending).
    pollWledStateFromWebSockets();
    // Accept one HTTP connection per tick.
    auto conn = server_.accept();
    if (conn.valid()) handleConnection(conn);
}

void HttpServerModule::tick1s() {
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
    int contentLen = 0;   // declared body length (0 if no Content-Length); used by the streaming route
    if (headerEnd) {
        auto* clh = std::strstr(req, "Content-Length:");
        if (clh) {
            // Bounded parse (not atoi): a malformed/negative/overflowing Content-Length must not
            // flow downstream, where it's cast to size_t — a negative int would become a huge
            // length that UploadSource/handleFirmwareUpload would treat as "gigabytes still to
            // come". We reject anything that isn't a clean unsigned integer: strtol with an end
            // pointer catches non-numeric, trailing junk ("123abc"), and ERANGE overflow; then we
            // reject negative and clamp to a firmware-sized ceiling (8 MB > any image we flash),
            // returning 400 rather than acting on it. The value ends at CR/LF/space or the string end.
            constexpr long kContentLenMax = 8L * 1024 * 1024;
            const char* valStart = clh + 15;
            while (*valStart == ' ' || *valStart == '\t') valStart++;   // skip OWS after the colon
            char* valEnd = nullptr;
            errno = 0;
            const long parsed = std::strtol(valStart, &valEnd, 10);
            const bool consumedDigits = valEnd != valStart;
            const bool endsCleanly = *valEnd == '\r' || *valEnd == '\n' || *valEnd == ' ' ||
                                     *valEnd == '\t' || *valEnd == '\0';
            if (!consumedDigits || !endsCleanly || errno == ERANGE ||
                parsed < 0 || parsed > kContentLenMax) {
                sendResponse(conn, 400, "application/json",
                             "{\"error\":\"invalid content-length\"}");
                return;
            }
            contentLen = static_cast<int>(parsed);
            int headerSize = static_cast<int>(headerEnd + 4 - req);
            int bodyNeeded = headerSize + contentLen;
            // Only the STREAMING routes (/api/file, /api/firmware/upload) may carry a body larger than
            // buf — they take the buffered prefix and pull the remainder straight off the socket. For
            // every OTHER route the body is parsed whole from buf, so a body over the buffer must be
            // REJECTED (413), not truncated: a capped read would parse a JSON prefix as if complete
            // (its own bodyNeeded check wouldn't fire, since the cap makes the short read "enough").
            // The request line sits at the start of req; a substring match on the path is sufficient.
            const bool isStreamingRoute =
                std::strncmp(req, "POST /api/file", 14) == 0 ||
                std::strncmp(req, "POST /api/firmware/upload", 25) == 0;
            if (bodyNeeded > static_cast<int>(sizeof(buf) - 1)) {
                if (!isStreamingRoute) {
                    sendResponse(conn, 413, "application/json",
                                 "{\"error\":\"request body too large\"}");
                    return;
                }
                bodyNeeded = static_cast<int>(sizeof(buf) - 1);   // streaming: buffer the prefix only
            }
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
    // deviceModels.json entry — see docs/moonmodules/core/moxygen/SystemModule.md.
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
        // File Manager: GET /api/dir?path=<rel>[&hidden=1] → one directory's children as JSON
        // [{name,isDir,size}] (the lazy tree loads a node's children on expand).
        else if (std::strcmp(path, "/api/dir") == 0) serveDirListing(conn, queryStart ? queryStart + 1 : "");
        // File Manager: GET /api/file?path=<rel> → the file's contents (text, size-capped).
        else if (std::strcmp(path, "/api/file") == 0) serveFileContents(conn, queryStart ? queryStart + 1 : "");
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
        // Home Assistant's WLED integration fetches `/json` (the full combined blob, not `/json/si`),
        // and its Python `wled` library rejects a response missing any of Info.fs, State.nl,
        // State.udpn, State.lor — so the `/json/info` + `/json/state` shim (tuned to the WLED Android
        // app's minimal Moshi model) can't answer this endpoint. serveWledDeviceJson writes the
        // fuller shape python-wled parses; /json/info and /json/state stay minimal (Android-app path).
        else if (std::strcmp(path, "/json") == 0) serveWledDeviceJson(conn);
        // /presets.json — the second endpoint HA's WLED lib fetches after /json (on every state
        // update where info.uptime/info.fs.pmt are zero — see python-wled's _check_presets_changed).
        // If it 404s, python-wled raises WLEDEmptyResponseError and HA's config flow aborts with
        // HTTP 500. We don't implement WLED presets, so return a TRUTHY-but-empty presets object
        // (`{"0":{}}`): python-wled's __pre_deserialize__ maps it into `{0: Preset(0)}` then discards
        // 0 per its "Nobody cares about 0" rule — result is HA seeing zero presets. `{}` alone would
        // fail the `not presets` guard in wled.py; we need a non-empty dict.
        else if (std::strcmp(path, "/presets.json") == 0)
            sendResponse(conn, 200, "application/json", "{\"0\":{}}");
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
        } else if (std::strcmp(path, "/api/file") == 0 && body) {
            // File Manager: POST /api/file?path=<rel>, the body → streamed atomic write. `body`
            // points at the bytes already buffered (initialLen); the full length is Content-Length,
            // and handleWriteFile pulls any remainder straight off the socket — so an upload of any
            // size streams to the file without a whole-request buffer or a strlen (binary-safe).
            const size_t initialLen = static_cast<size_t>(totalRead) - static_cast<size_t>(body - req);
            handleWriteFile(conn, queryStart ? queryStart + 1 : "", body, initialLen,
                            static_cast<size_t>(contentLen));
        } else if (std::strcmp(path, "/api/dir") == 0) {
            // File Manager: POST /api/dir?path=<rel> → mkdir. The path is the whole operation (a
            // create is a filesystem action, not a stored control), so it rides the request query
            // — same path-as-query shape as /api/file, no persisted control holds it.
            handleMakeDir(conn, queryStart ? queryStart + 1 : "");
        } else if (std::strcmp(path, "/api/modules") == 0 && body) {
            handleAddModule(conn, body);
        } else if (std::strncmp(path, "/api/list/", 10) == 0) {
            // Editable list: POST /api/list/<module>/<control> appends a new row and returns
            // its stable id. The row's fields are then set via PATCH /api/list/.../<id>.
            handleListAddRow(conn, path + 10);
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
        } else if (std::strcmp(path, "/api/firmware/upload") == 0 && body) {
            // OTA from an uploaded .bin body (no URL, no host to serve it) — the browser POSTs the
            // firmware image straight to the device, which streams it into the OTA partition. Same
            // streamed-body handling as /api/file (initial buffered bytes + socket remainder).
            const size_t initialLen = static_cast<size_t>(totalRead) - static_cast<size_t>(body - req);
            handleFirmwareUpload(conn, body, initialLen, static_cast<size_t>(contentLen));
        } else {
            sendResponse(conn, 404, "text/plain", "Not found");
        }
    } else if (std::strcmp(method, "PATCH") == 0) {
        // Editable list: PATCH /api/list/<module>/<control>/<id> edits one row — a field
        // ({"field":F,"value":V}) or a reorder ({"to":N}). PATCH is the REST verb for a
        // partial update of an existing resource (the row); create is POST, delete is DELETE.
        if (std::strncmp(path, "/api/list/", 10) == 0 && body) {
            handleListPatchRow(conn, path + 10, body);
        } else {
            sendResponse(conn, 404, "text/plain", "Not found");
        }
    } else if (std::strcmp(method, "DELETE") == 0) {
        // DELETE /api/modules/ModuleName
        if (std::strncmp(path, "/api/modules/", 13) == 0) {
            handleDeleteModule(conn, path + 13);
        } else if (std::strncmp(path, "/api/list/", 10) == 0) {
            // Editable list: DELETE /api/list/<module>/<control>/<id> removes one row.
            handleListDeleteRow(conn, path + 10);
        } else if (std::strcmp(path, "/api/dir") == 0) {
            // File Manager: DELETE /api/dir?path=<rel> → remove a file or empty dir.
            handleRemoveEntry(conn, queryStart ? queryStart + 1 : "");
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
        "Access-Control-Allow-Methods: GET, POST, PATCH, DELETE, OPTIONS\r\n"
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

// --- File Manager file read/write (the /api/file endpoints) ---
//
// A file body isn't a control value, so these are their own small endpoints (not /api/control).
// The path comes as a query param `path=<rel>`; parseFilePath vets it (reject "..", root at the
// mount) — the single traversal guard shared by every filesystem HTTP entry (read, write, dir
// listing, mkdir, delete).
//
// Read + write both stream: the write pulls the request body chunk-by-chunk straight to the file
// (fsWriteStream), the read pulls the file into a size-fit buffer — so a file of any size up- and
// downloads intact without a fixed cap. kUploadMax is a per-request sanity ceiling; a legit upload
// is additionally rejected up front if it wouldn't fit the free filesystem space.
static constexpr size_t kUploadMax = 256 * 1024;   // 256 KB — sanity bound on one upload

// Copy the `path=` query value into `out` (decoding %XX and '+' minimally), rooted at the mount.
// Returns false on a missing/empty path or a ".." traversal attempt.
//
// Deliberately NOT a `.config`/dotfile denylist (PO decision): the File Manager is a device-admin
// tool on a trusted LAN, and reading the persisted `.config/*.json` is a feature (inspect/back up
// the device's own config), not a leak — there are no third-party secrets on the device, and the
// WiFi password is XOR-obfuscated in what it writes. The weak-protection is `show hidden` defaulting
// off (FileManagerModule), so `.config` isn't shown unless the operator asks. Reviewers periodically
// flag this as a secrets-exposure — it's an accepted design, not an oversight; leave it.
bool HttpServerModule::parseFilePath(const char* query, char* out, size_t cap) {
    const char* p = query ? std::strstr(query, "path=") : nullptr;
    if (!p) return false;
    p += 5;                                   // past "path="
    size_t i = 0;
    // The path may be its own query (stop at '&') and percent-encoded ('/' → %2F, ' ' → %20).
    while (*p && *p != '&' && i + 1 < cap) {
        char c = *p;
        if (c == '%' && p[1] && p[2]) {       // %XX → byte
            auto hex = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return -1;
            };
            const int hi = hex(p[1]), lo = hex(p[2]);
            if (hi >= 0 && lo >= 0) { c = static_cast<char>((hi << 4) | lo); p += 2; }
        } else if (c == '+') {
            c = ' ';
        }
        out[i++] = c;
        p++;
    }
    // Reject an overlong path outright rather than routing on a truncated prefix: if the loop stopped
    // because the buffer filled (still more path bytes to come, i.e. not at '\0' or the '&' delimiter),
    // the decoded value is incomplete and must not be treated as a valid path.
    if (*p && *p != '&') return false;
    out[i] = 0;
    if (i == 0 || std::strstr(out, "..")) return false;   // empty or traversal → reject
    if (out[0] != '/') {                                  // relative → root at the mount
        char rooted[160];
        const int n = std::snprintf(rooted, sizeof(rooted), "/%s", out);
        if (n <= 0 || static_cast<size_t>(n) >= cap) return false;
        std::strncpy(out, rooted, cap - 1); out[cap - 1] = 0;
    }
    return true;
}

// --- File Manager directory listing (the /api/dir endpoint) ---
//
// One directory's children as a JSON array, the source the lazy tree loads a node's children from.
// Single-level only (platform::fsList) — the recursion is the UI's job, one fetch per expanded node,
// the standard file-tree shape. The `hidden` query flag (hidden=1) includes dot-prefixed entries.
// The listing streams straight to the socket (as serveState does) — no whole-listing buffer. The
// fsList C callback carries the streaming sink + the hidden filter + a first-row flag via `user`.
namespace {
struct DirListState {
    JsonSink* sink;
    bool showHidden;
    bool first = true;
};
void dirListTrampoline(const char* name, bool isDir, uint32_t size, void* user) {
    auto* st = static_cast<DirListState*>(user);
    if (!st->showHidden && name[0] == '.') return;          // dotfile convention
    if (!st->first) st->sink->append(",");
    st->first = false;
    st->sink->append("{\"name\":");
    st->sink->writeJsonString(name);
    st->sink->appendf(",\"isDir\":%s,\"size\":%lu}",
                      isDir ? "true" : "false", static_cast<unsigned long>(size));
}
}  // namespace

void HttpServerModule::serveDirListing(platform::TcpConnection& conn, const char* query) {
    char path[160];
    if (!parseFilePath(query, path, sizeof(path))) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"bad path\"}");
        return;
    }
    const char* header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));

    JsonSink sink(conn);
    DirListState st{&sink, query && std::strstr(query, "hidden=1") != nullptr, true};
    sink.append("[");
    platform::fsList(path, &dirListTrampoline, &st);
    sink.append("]");
    sink.flush();
}

// POST /api/dir?path=<rel> → mkdir. The path rides the query and is vetted by parseFilePath (the
// same `..`-reject + root-at-mount guard /api/file and /api/dir GET use). A create is a filesystem
// action, not a stored control — no persisted `path` control holds it, so no flash write.
void HttpServerModule::handleMakeDir(platform::TcpConnection& conn, const char* query) {
    char path[160];
    if (!parseFilePath(query, path, sizeof(path))) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"bad path\"}");
        return;
    }
    if (platform::fsMkdir(path)) sendResponse(conn, 200, "application/json", "{\"ok\":true}");
    else sendResponse(conn, 500, "application/json", "{\"error\":\"mkdir failed\"}");
}

// DELETE /api/dir?path=<rel> → remove a file or EMPTY dir (fsRemove fails cleanly on a non-empty
// dir). Same path guard as handleMakeDir.
void HttpServerModule::handleRemoveEntry(platform::TcpConnection& conn, const char* query) {
    char path[160];
    if (!parseFilePath(query, path, sizeof(path))) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"bad path\"}");
        return;
    }
    if (platform::fsRemove(path)) sendResponse(conn, 200, "application/json", "{\"ok\":true}");
    else sendResponse(conn, 500, "application/json", "{\"error\":\"delete failed (folder not empty?)\"}");
}

void HttpServerModule::serveFileContents(platform::TcpConnection& conn, const char* query) {
    char path[160];
    if (!parseFilePath(query, path, sizeof(path))) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"bad path\"}");
        return;
    }
    // Stream the file straight to the socket in fixed 1 KB chunks (fsReadAt) with an explicit
    // Content-Length header — no whole-file buffer, and NUL-safe (sendResponse strlen()s its body, so
    // it can't carry binary). Symmetric with the streamed upload: a file of any size downloads whole.
    const long size = platform::fsSize(path);
    if (size < 0) { sendResponse(conn, 404, "application/json", "{\"error\":\"not found\"}"); return; }
    char header[160];
    const int hn = std::snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %ld\r\n"
        "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n", size);
    conn.write(reinterpret_cast<const uint8_t*>(header), static_cast<size_t>(hn));
    char chunk[1024];
    for (long offset = 0; offset < size;) {
        const size_t want = static_cast<size_t>(size - offset) < sizeof(chunk)
                          ? static_cast<size_t>(size - offset) : sizeof(chunk);
        const int got = platform::fsReadAt(path, offset, chunk, want);
        if (got <= 0) break;   // read error / early EOF — the client sees a short (truncated) body
        conn.write(reinterpret_cast<const uint8_t*>(chunk), static_cast<size_t>(got));
        offset += got;
    }
}

// Source state for the streamed upload: yields the body bytes already sitting in the request buffer,
// then reads the remainder straight off the socket — feeding fsWriteStream in fixed chunks so the
// device never holds the whole upload in RAM.
namespace {
// This drain runs SYNCHRONOUSLY on the tick20ms() tick, which is inside Scheduler::tick — so it
// blocks rendering for the duration of the transfer (LEDs freeze until the upload completes or a
// bound trips). Accepted trade-off: an upload is user-initiated and transient (and a firmware upload
// reboots the device anyway), so a brief freeze is fine where a persistent one wouldn't be. The two
// bounds cap how long that freeze can last, because neither alone is enough:
//   - kUploadIdleMs: max wait for the NEXT byte, reset on every successful read. Scales to
//     any size the endpoint accepts — a big but steady upload (256 KB over slow LittleFS +
//     weak WiFi) never trips it, because progress keeps resetting the clock. But idle-only
//     lets a slowloris trickle one byte just under the idle limit forever, freezing rendering
//     (and the HTTP server) for as long as it keeps dribbling.
//   - kUploadHardMs: an absolute whole-request ceiling that closes that hole. Sized well
//     above a legit worst case (256 KB / ~50 KB/s ≈ 5 s, plus wide margin) so a real slow
//     upload finishes, but far below the days a byte-per-idle-window trickler would need.
// A single budget can't do both jobs; the pair does (idle scales, hard caps the total). The
// zero-freeze fix (drain a chunk per tick, like drainPreviewSend) is backlogged; the bounded
// synchronous drain is the accepted interim.
constexpr uint32_t kUploadIdleMs = 5000;    // max gap between successful reads before abort
constexpr uint32_t kUploadHardMs = 60000;   // absolute whole-request ceiling (anti-slowloris)
// A firmware image is MB-scale (1.5+ MB), not the KB-scale of a config file, and pushing it over weak
// WiFi can legitimately take minutes — past kUploadHardMs (60 s), which sized the whole-request cap for
// a 256 KB file and aborted a real firmware push at ~87%. So the firmware path gets its own larger
// ceiling. Sizing: 1.5 MB at a poor-but-real 10 KB/s is ~2.5 min, so 3 min covers any firmware over any
// LAN link with margin — deliberately NOT more, because this cap also bounds the worst-case render
// freeze: like the file upload, the firmware drain runs SYNCHRONOUSLY (otaWriteStream loops uploadPull
// to completion inside one tick20ms tick), so a slow-but-steady transfer freezes rendering for its whole
// duration. kUploadIdleMs (5 s, reset per read) still bounds a *stalled* transfer; this bounds a *slow*
// one. The proper fix is the same zero-freeze drain-a-chunk-per-tick pattern drainPreviewSend uses
// (backlogged, see the kUploadHardMs comment above) — until it lands, keep this ceiling as tight as a
// real upload allows. A firmware push reboots on success, so the freeze is at least terminal, not a
// lingering degradation.
constexpr uint32_t kFirmwareUploadHardMs = 180000;  // 3 min absolute ceiling for a firmware push
struct UploadSource {
    platform::TcpConnection* conn;
    const char* initial;      // body bytes already read into the request buffer
    size_t initialLeft;       // how many of those remain to hand out
    size_t remaining;         // total body bytes still to deliver (Content-Length − delivered)
    uint32_t hardDeadline;    // absolute millis by which the whole body must arrive
};
size_t uploadPull(char* out, size_t cap, void* user, bool* abort) {
    auto* s = static_cast<UploadSource*>(user);
    if (s->remaining == 0) return 0;   // all body delivered → clean EOF
    // Whole-request ceiling, checked on EVERY pull (not only while the socket is dry): a paced
    // trickler that always keeps one byte ready makes each read return > 0 immediately, so a cap
    // tested only in the wait loop would never fire. Enforcing it here makes it truly absolute.
    if (static_cast<int32_t>(platform::millis() - s->hardDeadline) >= 0) { *abort = true; return 0; }
    // Drain the already-buffered prefix first.
    if (s->initialLeft) {
        const size_t n = s->initialLeft < cap ? s->initialLeft : cap;
        std::memcpy(out, s->initial, n);
        s->initial += n; s->initialLeft -= n; s->remaining -= n;
        return n;
    }
    // Then pull the rest off the socket, bounded by BOTH the per-pull idle deadline (recomputed
    // here, only advances while we wait — bounds a stall) and the request-lifetime hardDeadline
    // (set once at construction — bounds the total). If the body is still incomplete when the
    // socket closes early or either deadline lapses, signal *abort — fsWriteStream then discards
    // the temp file rather than committing a truncated upload (a 0 here is NOT a clean end). Both
    // compares are subtraction-based, wraparound-safe across the ~49.7-day millis() rollover.
    const size_t want = s->remaining < cap ? s->remaining : cap;
    const uint32_t deadline = platform::millis() + kUploadIdleMs;
    for (;;) {
        const int r = s->conn->read(reinterpret_cast<uint8_t*>(out), want);
        if (r > 0) { s->remaining -= static_cast<size_t>(r); return static_cast<size_t>(r); }
        if (r == 0) { *abort = true; return 0; }                 // peer closed with body remaining
        // Idle timeout (the hard whole-request cap is enforced at the top of uploadPull, so it
        // covers the pacing case this wait loop can't). Both compares are wraparound-safe.
        if (static_cast<int32_t>(platform::millis() - deadline) >= 0) { *abort = true; return 0; }
        platform::delayMs(1);
    }
}
}  // namespace

void HttpServerModule::handleWriteFile(platform::TcpConnection& conn, const char* query,
                                       const char* initialBody, size_t initialLen, size_t contentLen) {
    char path[160];
    if (!parseFilePath(query, path, sizeof(path))) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"bad path\"}");
        return;
    }
    if (contentLen > kUploadMax) {
        sendResponse(conn, 413, "application/json", "{\"error\":\"file too large\"}");
        return;
    }
    // Reject up front if it wouldn't fit the free filesystem space (friendlier than filling the FS
    // and failing mid-write — fsWriteStream also fails cleanly + discards the temp if it does fill).
    // total − used = free. An overwrite would reclaim the old file's space, but treat free
    // conservatively (don't credit the overwrite) so the check never over-promises.
    const size_t total = platform::filesystemTotal();
    const size_t used = platform::filesystemUsed();
    const size_t freeBytes = total > used ? total - used : 0;
    if (total > 0 && contentLen > freeBytes) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "{\"error\":\"not enough space (%lu free)\"}",
                      static_cast<unsigned long>(freeBytes));
        sendResponse(conn, 507, "application/json", msg);   // 507 Insufficient Storage
        return;
    }
    // Never hand the source more than Content-Length of the already-buffered bytes: a buffer can hold
    // bytes past the body (a pipelined next request), which must not be written into the file.
    const size_t initial = initialLen < contentLen ? initialLen : contentLen;
    UploadSource src{&conn, initialBody, initial, contentLen,
                     platform::millis() + kUploadHardMs};
    if (platform::fsWriteStream(path, &uploadPull, &src)) {
        sendResponse(conn, 200, "application/json", "{\"ok\":true}");
    } else {
        sendResponse(conn, 500, "application/json", "{\"error\":\"write failed\"}");
    }
}

// OTA from an uploaded .bin body: stream the request body straight into the OTA partition
// (platform::otaWriteStream), reusing the exact uploadPull the file-upload path uses — the only
// difference is the sink (OTA partition vs a file). On success the device reboots into the new
// image; the 200 goes out first (otaWriteStream's ~600 ms pre-reboot delay covers the round-trip).
void HttpServerModule::handleFirmwareUpload(platform::TcpConnection& conn, const char* initialBody,
                                            size_t initialLen, size_t contentLen) {
    if constexpr (!platform::hasOta) {
        sendResponse(conn, 501, "application/json", "{\"error\":\"OTA not supported on this platform\"}");
        return;
    }
    // Same 409 concurrency guard as handleFirmwareUrl: one OTA at a time (both write g_ota* state).
    if (!otaTryStart()) {
        sendResponse(conn, 409, "application/json", "{\"error\":\"ota already in progress\"}");
        return;
    }
    const size_t initial = initialLen < contentLen ? initialLen : contentLen;
    // Firmware gets the MB-scale ceiling, not the file path's 60 s — a 1.5 MB push over WiFi
    // outruns kUploadHardMs and would abort mid-flash (the exact "upload aborted" a real firmware
    // push hit at ~87%). See kFirmwareUploadHardMs.
    UploadSource src{&conn, initialBody, initial, contentLen,
                     platform::millis() + kFirmwareUploadHardMs};
    g_otaBytesTotal = static_cast<uint32_t>(contentLen);   // the UI's "Y KB" (Content-Length up front)
    g_otaBytesRead = 0;                                    // clear any stale count from a prior OTA
    // Stream the body into the OTA partition. otaWriteStream commits the image + flips the boot
    // pointer but does NOT reboot — it returns so we can send a 200 first, then reboot the same
    // way /api/reboot does (response, close, brief drain, platform::reboot). That gives the browser
    // a clean "flashed" response instead of an aborted socket it can't tell from a real failure.
    const bool ok = platform::otaWriteStream(&uploadPull, &src, contentLen,
                                             g_otaStatus, sizeof(g_otaStatus), &g_otaBytesRead);
    if (!ok) {
        otaFinish();
        char msg[96];
        std::snprintf(msg, sizeof(msg), "{\"error\":\"ota failed: %.60s\"}", g_otaStatus);
        sendResponse(conn, 500, "application/json", msg);
        return;
    }
    FilesystemModule::flushPending();
    sendResponse(conn, 200, "application/json", "{\"ok\":true}");
    conn.close();
    platform::delayMs(200);
    platform::reboot();  // noreturn — boots the flashed image
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
            // Skip modules that opt out of the UI via appearsInUi() — the one mechanism for
            // "not a card in /api/state": HttpServerModule (the server itself) and FilesystemModule
            // (a pure persistence engine, no controls) both return false.
            if (!mod || !mod->appearsInUi()) continue;
            if (!first) sink.append(",");
            first = false;
            writeModuleJson(sink, mod);
        }
    }

    sink.append("]}");
}

// FNV-1a 32-bit — a small, fast, recognisable string hash. Used to digest a control's serialised
// value (and the leaf's path) for the diff-on-the-wire cache, so the cache holds an 8-byte
// {path,value} hash per leaf rather than the value string. Not cryptographic; a hash collision (two
// different values, same 32-bit digest) at worst skips ONE update and self-heals on the next change.
static uint32_t fnv1a(const char* s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= static_cast<uint8_t>(s[i]); h *= 16777619u; }
    return h;
}

// The diff-on-the-wire core. Visit every UI leaf the periodic push would send — each module's live
// header telemetry (tickTimeUs / dynamicBytes, which the UI shows per card) and each control's value —
// in the SAME order buildStateJson emits, so a leaf's path "<module>/<name>" is stable across ticks.
// For each leaf: build its path-hash + a hash of its serialised value; `fn(pathHash, valueHash, path,
// valueSink)` decides what to do (emit a patch entry, or just (re)baseline the cache). Names are unique
// tree-wide (deduplicateNamesInTree at setup/load + ensureUniqueName on every runtime add/replace,
// both before the resync that re-baselines), so "<module>/<name>" uniquely identifies a leaf.
template <class Fn>
void HttpServerModule::forEachStateLeaf(Fn&& fn) {
    if (!scheduler_) return;
    for (uint8_t m = 0; m < scheduler_->moduleCount(); m++)
        if (auto* mod = scheduler_->module(m))
            if (mod->appearsInUi()) visitModuleLeaves(mod, std::forward<Fn>(fn));
}

template <class Fn>
void HttpServerModule::visitModuleLeaves(MoonModule* mod, Fn&& fn) {
    char path[80];
    // Module-header telemetry leaves the UI shows live per card. `@` prefixes a header field so it can't
    // collide with a control name. Only the fields that actually change per tick (timing/memory) — role,
    // classSize, enabled are static and ride the full state.
    auto leaf = [&](const char* fieldPath, const char* valueJson) {
        JsonSink vs; vs.append(valueJson);
        fn(fnv1a(fieldPath, std::strlen(fieldPath)), fnv1a(vs.data(), vs.size()), fieldPath, vs);
    };
    char num[24];
    std::snprintf(path, sizeof(path), "%s/@tickTimeUs", mod->name());
    std::snprintf(num, sizeof(num), "%u", static_cast<unsigned>(mod->tickTimeUs())); leaf(path, num);
    std::snprintf(path, sizeof(path), "%s/@dynamicBytes", mod->name());
    std::snprintf(num, sizeof(num), "%u", static_cast<unsigned>(mod->dynamicBytes())); leaf(path, num);
    // Status + severity change per tick too — a driver can fault at any moment (a Hue pairing result, a
    // loopback verdict, a bus that won't init). They MUST ride the patch: the diff push is the only thing
    // that runs every second, so a status carried by the full state alone sits stale until an unrelated
    // resync — and a module whose card is collapsed behind a tab would surface no fault at all. The
    // value-hash gate means an unchanged status costs nothing on the wire. Same wire strings writeStatus
    // emits (a null status is the empty string, which the UI treats as "no status").
    {
        JsonSink sv;
        sv.append("\"");
        sv.writeJsonString(mod->status() ? mod->status() : "");
        sv.append("\"");
        std::snprintf(path, sizeof(path), "%s/@status", mod->name());
        leaf(path, sv.data());
    }
    {
        static const char* sevStr[] = {"status", "warning", "error"};
        JsonSink sv;
        sv.appendf("\"%s\"", sevStr[static_cast<int>(mod->severity())]);
        std::snprintf(path, sizeof(path), "%s/@severity", mod->name());
        leaf(path, sv.data());
    }
    // Each control's value.
    auto& ctrls = mod->controls();
    for (uint8_t i = 0; i < ctrls.count(); i++) {
        auto& c = ctrls[i];
        std::snprintf(path, sizeof(path), "%s/%s", mod->name(), c.name);
        JsonSink vs; writeControlValue(vs, c);
        fn(fnv1a(path, std::strlen(path)), fnv1a(vs.data(), vs.size()), path, vs);
    }
    for (uint8_t i = 0; i < mod->childCount(); i++)
        if (auto* ch = mod->child(i)) visitModuleLeaves(ch, std::forward<Fn>(fn));
}

// Look up a leaf's cached value-hash by path-hash; returns nullptr if not yet seen. Linear over the
// flat cache — the tree is ~92 leaves, so this is a handful of int compares per leaf (cheap, no map).
HttpServerModule::LeafHash* HttpServerModule::findLeaf(uint32_t pathHash) {
    for (uint16_t i = 0; i < leafHashCount_; i++)
        if (leafHashes_[i].path == pathHash) return &leafHashes_[i];
    return nullptr;
}

void HttpServerModule::baselineLeafHashes() {
    // Count the leaves, size the buffer to fit exactly, then fill from scratch. resize is
    // non-preserving (frees + reallocs on a size change), which is fine BECAUSE we re-fill completely
    // right after. Off the hot path (runs on a full-state resync, not per tick).
    uint16_t n = 0;
    forEachStateLeaf([&](uint32_t, uint32_t, const char*, JsonSink&) { n++; });
    leafHashes_.resize(n);
    leafHashCount_ = 0;
    forEachStateLeaf([&](uint32_t ph, uint32_t vh, const char*, JsonSink&) {
        if (leafHashCount_ < leafHashes_.count()) leafHashes_[leafHashCount_++] = {ph, vh};
    });
}

uint16_t HttpServerModule::buildStatePatch(JsonSink& sink) {
    sink.append("{\"patch\":[");
    uint16_t changed = 0;
    forEachStateLeaf([&](uint32_t ph, uint32_t vh, const char* path, JsonSink& vs) {
        LeafHash* h = findLeaf(ph);
        if (h && h->value == vh) return;              // unchanged — the common case, emit nothing
        if (h) h->value = vh;                          // known leaf, value changed → update cache
        // A leaf NOT in the baseline means the tree grew without a re-baseline — which can't happen on
        // any real path: every structural mutation calls requestFullResync() → baselineLeafHashes()
        // before the next patch, so the baseline always covers the current tree. We therefore do NOT
        // try to grow the cache here: ScratchBuffer::resize is non-preserving (frees + reallocs), so a
        // mid-patch grow would discard every existing hash and corrupt the cache. Instead just EMIT the
        // leaf (the UI still gets it) and leave the cache untouched; the next structural resync
        // re-baselines cleanly. In practice this branch is never taken.
        if (changed++) sink.append(",");
        sink.append("{\"path\":\"");
        sink.append(path);
        sink.append("\",\"value\":");
        sink.append(vs.data());                        // the already-serialised value
        sink.append("}");
    });
    sink.append("]}");
    return changed;
}

void HttpServerModule::writeModuleJson(JsonSink& sink, MoonModule* mod) {
    // Per-module header: name, role, enabled, tickTimeUs (fps/ms display),
    // classSize (static C++ object bytes) + dynamicBytes (heap), controls
    const char* roleStr = roleName(mod->role());
    const char* type = mod->typeName();
    if (!type) type = "";
    sink.appendf(
        "{\"name\":\"%s\",\"type\":\"%s\",\"role\":\"%s\",\"enabled\":%s,"
        "\"tickTimeUs\":%u,\"classSize\":%u,\"dynamicBytes\":%u",
        mod->name() ? mod->name() : "",
        type,
        roleStr,
        mod->enabled() ? "true" : "false",
        static_cast<unsigned>(mod->tickTimeUs()),
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
        // An editable List (the CRUD primitive) tells the UI to show add/delete/reorder + inline
        // row editors; a plain List stays read-only. The row objects carry a stable "id" the
        // /api/list/* ops address, and each editable row's detail carries its field descriptors.
        if (c.type == ControlType::List) {
            const auto* ls = static_cast<const ListSource*>(c.ptr);
            if (ls && ls->isEditableList()) sink.append(",\"editable\":true");
        }
        sink.append(c.hidden ? ",\"hidden\":true}" : "}");
    }
}

// Apply-core: set one control's value. `valueJson` is a small JSON object holding
// the value under the "value" key ({"value":8}) — the same body the HTTP handler
// receives, so applyControlValue (which reads by key) is reused verbatim. Transport-
// free: no TcpConnection, returns an OpResult the caller maps to its own reporting.
HttpServerModule::OpResult HttpServerModule::applySetControl(
        const char* moduleName, const char* controlName, const char* valueJson) {
    // The generic control-set is a Scheduler primitive (it owns the tree + persistence hook),
    // shared with every other control writer — Improv, the WLED bridge, IrService. This wrapper
    // only maps its result onto the HTTP OpResult so the response carries the right status code.
    if (!scheduler_) return OpResult::ModuleNotFound;
    switch (scheduler_->setControl(moduleName, controlName, valueJson)) {
        // A schema change (hidden flags / option sets) from this set is handled centrally:
        // Scheduler::setControl always calls the target's rebuildControls(), which fires the
        // schema-changed hook → requestFullResync(). So no per-path resync is needed here.
        case Scheduler::SetControlResult::Ok:              return OpResult::Ok;
        case Scheduler::SetControlResult::ModuleNotFound:  return OpResult::ModuleNotFound;
        case Scheduler::SetControlResult::ControlNotFound: return OpResult::ControlNotFound;
        case Scheduler::SetControlResult::OutOfRange:      return OpResult::OutOfRange;
        case Scheduler::SetControlResult::Malformed:       return OpResult::Malformed;
        case Scheduler::SetControlResult::ReadOnly:        return OpResult::ReadOnly;
    }
    return OpResult::ModuleNotFound;   // unreachable; keeps -Wreturn-type happy
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

// The Scheduler owns the module tree, so the tree-walk-by-name lives there (firstByName);
// this only adds the scheduler_ null-guard the request handlers rely on (scheduler_ is unset
// until setScheduler() runs), then delegates — one recursive lookup, not two.
MoonModule* HttpServerModule::findModuleByName(const char* name) {
    return scheduler_ ? scheduler_->firstByName(name) : nullptr;
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
// known-sufficient field set — see docs/moonmodules/core/moxygen/HttpServerModule.md.)
void HttpServerModule::serveWledInfo(platform::TcpConnection& conn) {
    const char* header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));

    // Identity: the deviceName (from SystemModule), the live IP, the MAC.
    const char* name; uint8_t mac[6]; uint8_t ip[4];
    resolveWledIdentity(name, mac, ip);
    (void)ip;  // serveWledInfo doesn't need the IP; keep the call uniform.

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

// See header. Extracts the deviceName / IP / MAC lookup the WLED shim needs at four
// call sites (/json/info, /json/state /json/si, /json), so a future change to how identity
// is discovered updates one place.
void HttpServerModule::resolveWledIdentity(const char*& name, uint8_t mac[6], uint8_t ip[4],
                                           const char* nameFallback) {
    name = nameFallback;
    if (MoonModule* sys = findModuleByName("System")) {
        const char* dn = static_cast<SystemModule*>(sys)->deviceName();
        if (dn && dn[0]) name = dn;
    }
    for (int i = 0; i < 6; i++) mac[i] = 0;
    platform::getMacAddress(mac);
    for (int i = 0; i < 4; i++) ip[i] = 0;
    platform::ethGetIPv4(ip);
    if (!ip[0] && !ip[1] && !ip[2] && !ip[3]) platform::wifiStaGetIPv4(ip);
}

// The WLED info object, written into an open sink (no HTTP header). Shared by
// /json/info and the `info` half of /json/si.
// Emit the WLED `name` field with the 💫 projectMM marker prefixed, so a projectMM board stands out
// among plain WLED devices in Home Assistant's device list (which keys everything off the WLED
// integration). The marker lives ONLY in the WLED-compat name HA reads — the real deviceName (UI,
// mDNS hostname, MQTT topics) stays unprefixed, so identity/hostnames carry no emoji. writeJsonString
// owns the quotes + escaping; the marker is a plain UTF-8 literal that passes through unescaped.
void HttpServerModule::writeWledName(JsonSink& sink, const char* name) {
    char prefixed[80];
    std::snprintf(prefixed, sizeof(prefixed), "\xF0\x9F\x92\xAB %s", name ? name : "");
    sink.writeJsonString(prefixed);
}

void HttpServerModule::writeWledInfoBody(JsonSink& sink, const char* name, const uint8_t mac[6]) {
    sink.appendf("{\"name\":");
    writeWledName(sink, name);
    // Real led count (the light domain's Drivers::latestSummary) + wifi rssi/signal, so the WLED
    // app card and the WS push show the true device shape. signal maps rssi→0-100 like WLED.
    const unsigned ledCount = Drivers::latestSummary()->lightCount;
    const int rssi = platform::wifiStaRssi();
    int signal = (rssi == 0) ? 0 : (2 * (rssi + 100));
    if (signal < 0) signal = 0; else if (signal > 100) signal = 100;
    sink.appendf(",\"mac\":\"%02x%02x%02x%02x%02x%02x\","
                 "\"leds\":{\"count\":%u},\"wifi\":{\"rssi\":%d,\"signal\":%d},"
                 "\"brand\":\"WLED\",\"product\":\"MoonModules\"}",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 ledCount, rssi, signal);
}

// The WLED state object, written into an open sink. `on` + `bri` mirror Drivers on/brightness.
// `seg[0].col[0]` reports the ACTIVE PALETTE's identity colour, not the live first-LED — so
// every WLED consumer (the WLED native app's device card, HA's WLED integration colour picker,
// Homebridge's HSV via the MQTT pair, the /ws push) sees the same stable palette-representative
// value and matches the palette-picker → RGB round-trip. Live first-LED was tried first and
// dropped: it dimmed the picker under low master brightness (near-black) and jittered with the
// effect animation ("the picked color moves" — user report). `Palettes::representativeRgb`
// returns V=255, so brightness stays HA's `state.bri × seg.bri` responsibility and doesn't
// double-dim. Rationale for the seg[0].on / seg[0].bri fields lives inline below.
void HttpServerModule::writeWledStateBody(JsonSink& sink) {
    const uint8_t bri = driversBrightness(scheduler_);
    const RGB pc = Palettes::representativeRgb(driversPalette(scheduler_));
    // nl/udpn/lor/transition/ps/pl/mainseg are additive to the Android-app minimum (Moshi ignores
    // unknown/extra fields), and REQUIRED for HA's WLED integration: `python-wled` parses the POST
    // /json/state response through State.from_dict too — the same required-fields contract as /json.
    // Without them, HA `light.turn_on` succeeds on the device but the response parse raises, which HA
    // wraps as HTTP 500 on `services/light/turn_on`. nl/udpn as empty objects satisfy the parser via
    // their dataclass defaults; lor=0 is LiveDataOverride.OFF.
    // seg[0].on MUST be present: HA WLED's is_on for a WLEDSegmentLight reads
    // state.segments[<seg>].on (light.py:244), NOT top-level state.on. Without it,
    // python-wled parses segment.on as its dataclass default None, `bool(None)` is
    // False, and HA's UI shows the light off even when the device is on — the
    // "brightness/color work but the toggle doesn't" symptom pinned on the bench.
    const char* onStr = driversOn(scheduler_) ? "true" : "false";
    // seg[0].pal = the active palette index, so HA's WLED integration highlights the current entry
    // in its palette dropdown (light.py reads state.segments[<seg>].palette). It shares the Drivers
    // `palette` control with col[0] above (representativeRgb of the SAME index), so the HA palette
    // dropdown and colour picker stay two views of one value: selecting a palette repaints the picker
    // on HA's next poll, and picking a colour snaps to the nearest palette (applyWledState).
    const uint8_t pal = driversPalette(scheduler_);
    sink.appendf("{\"on\":%s,\"bri\":%u,\"transition\":7,\"ps\":-1,\"pl\":-1,"
                 "\"nl\":{},\"udpn\":{},\"lor\":0,\"mainseg\":0,"
                 // seg[0].bri MUST be present alongside seg[0].on (same reason): HA WLED reads brightness
                 // from state.segments[<seg>].brightness (light.py's _attr_brightness), NOT top-level
                 // state.bri. Without it python-wled parses segment.brightness as the dataclass default
                 // 0, so HA renders the slider at zero even when the device is at full. Same on-the-
                 // bench root-cause as seg[0].on — HA's SegmentLight class reads *segment* fields.
                 // seg[0].bri = 255 (segment is 100% of master), state.bri = actual — the real WLED
                 // convention. HA WLEDSegmentLight with the default has_main_light=False computes
                 // (segment.bri × state.bri) / 255 (coordinator.py + light.py:220-222), so sending
                 // 255 in the segment lets HA render the actual master value. Sending `bri` in both
                 // would show bri²/255 instead — verified against ha-core wled/coordinator.py.
                 // fx=0 accompanies pal: a real WLED segment always reports BOTH the effect and the
                 // palette index, and python-wled's Segment model (HA's WLED integration) pairs them —
                 // sending pal without fx yields a half-populated segment real WLED never produces, and
                 // HA's light-platform setup then leaves the light entity stuck `restored`/unavailable
                 // (the sensors still work — only the segment-derived light breaks). fx=0 = "Solid", the
                 // single effect this shim exposes (fxcount=1), so the pair is consistent.
                 "\"seg\":[{\"id\":0,\"on\":%s,\"bri\":255,\"fx\":0,\"pal\":%u,\"col\":[[%u,%u,%u]]}]}",
                 onStr, bri, onStr, pal, pc.r, pc.g, pc.b);
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

// /json — the FULL combined blob Home Assistant's WLED integration fetches (frenck/python-wled). The
// crucial deltas from /json/si (which targets the WLED Android app's minimal Moshi model): python-wled
// requires `info.fs` (Filesystem), `state.nl` (Nightlight), `state.udpn` (UDPSync), and `state.lor`
// (LiveDataOverride) — every other field carries a default in the dataclass and is optional. We also
// send `ver >= "0.14.0"` because python-wled's __pre_deserialize__ raises WLEDUnsupportedVersionError
// on anything below (skipped only when `ver` is absent, but sending it makes HA's update-badge behave).
// `effects` and `palettes` each carry one entry so HA renders a one-option picker rather than none.
// Independent of /json/info + /json/state so THIS surface can grow to satisfy python-wled without
// disturbing the Android-app validated minimum. Prior art: WLED's own /json response shape (public
// docs at https://kno.wled.ge/interfaces/json-api/); we write ours fresh against the model dataclass
// contract, not by copying WLED's implementation.
void HttpServerModule::serveWledDeviceJson(platform::TcpConnection& conn) {
    const char* header =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));

    const char* name; uint8_t mac[6]; uint8_t ip[4];
    resolveWledIdentity(name, mac, ip);

    JsonSink sink(conn);
    // state — writeWledStateBody emits the {on,bri,seg,...} block reused by /json/state and
    // /json/si; wrap it under "state":. Keeping one authoritative writer avoids the two paths
    // drifting on which seg[0] fields HA actually reads.
    sink.appendf("{\"state\":");
    writeWledStateBody(sink);
    // info — `ver` is a sentinel `"99.0.0"`, NOT the projectMM semver. Reason: HA's WLED
    // integration parses WLED tags as CalVer (`16.0.1` is year-16, not `0.16.1`), so a
    // projectMM semver like `2.1.0-dev` compares LOWER than WLED's current `16.0.1` (2 < 16)
    // and HA flags a bogus "update to WLED 16.0.1" whose `.bin` would brick a projectMM
    // device. `AwesomeVersion("99.0.0") > AwesomeVersion("<any WLED tag>")` in the CalVer
    // regime, so HA's WLED update-check is always silent for us. First tried `mm::kVersion`
    // (assuming SemVer parsing) — the bench P4 showed HA still flagging 16.0.1 after the flash
    // because the CalVer branch was the actual one taken. Real projectMM version lives on the
    // MQTT `update/state` topic (`installed_version` under the HA update entity), which is where
    // "did projectMM ship a new release" belongs — the WLED shim is for the LIGHT ENTITY, not
    // the firmware version. `arch`/`brand`/`product`/`mac`/`ip` populate HA's device card
    // (mf/mdl/sw_version rendered from these); `leds`/`wifi`/`fs` are the objects python-wled's
    // Info dataclass requires or expects for the sensor entities (heap, uptime, signal).
    // Real values for the diagnostic sensors HA renders from the `wifi` + `freeheap` blocks.
    // signal maps rssi→0-100 the way WLED does (0 at -100 dBm, 100 at -50 dBm); bssid/channel come
    // from the associated AP. On an ETHERNET device there is no Wi-Fi AP, so these read 0/empty — and
    // `info.wifi` is Optional in python-wled, so we OMIT the whole `wifi` object rather than send a
    // zeroed one. HA then creates no Wi-Fi sensors for an eth device (a real WLED-on-eth behaves the
    // same), instead of the greyed "Wi-Fi RSSI/BSSID/channel/signal" rows an all-zero block produces.
    const bool onEth = platform::ethConnected();
    const int rssi = platform::wifiStaRssi();
    uint8_t bssid[6] = {};
    platform::wifiStaBssid(bssid);
    const int channel = platform::wifiStaChannel();
    int signal = (rssi == 0) ? 0 : (2 * (rssi + 100));
    if (signal < 0) signal = 0; else if (signal > 100) signal = 100;
    // Real pipeline shape from the light domain (Drivers::latestSummary) + render rate.
    const LightSummary* ls = Drivers::latestSummary();
    const unsigned ledCount = ls->lightCount;
    const unsigned renderFps = scheduler_ ? scheduler_->fps() : 0;
    const char* rgbw = (ls->channelsPerLight >= 4) ? "true" : "false";
    sink.appendf(",\"info\":{\"ver\":\"99.0.0\",\"vid\":2410150,\"name\":");
    writeWledName(sink, name);
    sink.appendf(",\"mac\":\"%02x%02x%02x%02x%02x%02x\","
                 "\"ip\":\"%u.%u.%u.%u\",\"arch\":\"esp32\","
                 "\"brand\":\"WLED\",\"product\":\"MoonModules\",\"release\":\"MoonModules\","
                 // lc + seglc = LightCapability.RGB_COLOR (1) so HA WLED's segment light picks
                 // ColorMode.RGB (via LIGHT_CAPABILITIES_COLOR_MODE_MAPPING in ha-core/wled/const.py),
                 // which grants a brightness slider AND colour picker. LightCapability.NONE (0)
                 // maps to ColorMode.ONOFF, which is why the entity was on/off-only initially.
                 // BOTH are capability CODES, not counts: HA reads seglc[segment_id] as that segment's
                 // capability bitmask (1 = RGB), then LIGHT_CAPABILITIES_COLOR_MODE_MAPPING[seglc[0]]
                 // gives the colour mode. Putting the LED count here (e.g. seglc:[24]) has no mapping,
                 // so WLEDSegmentLight ends up with NO supported colour modes and HA refuses to add the
                 // light entity ("does not set supported color modes") — it stays `restored`/unavailable
                 // while the sensors still work. seglc is therefore the constant 1, matching lc; the LED
                 // count lives only in `count`. fps = the real render rate (scheduler_->fps()).
                 "\"leds\":{\"count\":%u,\"fps\":%u,\"rgbw\":%s,\"wv\":false,\"cct\":false,"
                 "\"maxpwr\":0,\"maxseg\":1,\"pwr\":0,\"lc\":1,\"seglc\":[1]},",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 ip[0], ip[1], ip[2], ip[3],
                 ledCount, renderFps, rgbw);
    // wifi — only for a Wi-Fi device (omitted on Ethernet; see the comment above the getters).
    if (!onEth) {
        sink.appendf("\"wifi\":{\"bssid\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
                     "\"rssi\":%d,\"channel\":%d,\"signal\":%d},",
                     bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                     rssi, channel, signal);
    }
    sink.appendf("\"fs\":{\"t\":256,\"u\":32,\"pmt\":1},"
                 // uptime + pmt drive python-wled's presets change-detect: when both are non-zero and
                 // stable, HA computes a stable "boot_time" and stops refetching /presets.json every
                 // state update. pmt=1 (stable-since-boot) + uptime in seconds gives it the anchor.
                 "\"freeheap\":%u,\"uptime\":%u,\"udpport\":21324,\"live\":false,"
                 // ws=-1 tells python-wled (HA's WLED integration lib) that WebSocket updates are
                 // unsupported in this build. Its __post_deserialize__ maps -1 to None, and its
                 // coordinator falls back to HTTP polling. Sending 0 (the WLED convention for
                 // "supported, no clients yet") makes HA open a WS to our own /ws endpoint, which
                 // serves projectMM-native state frames — not the WLED-shaped Info+State updates the
                 // python-wled parser requires — and floods HA's log with `MissingField: filesystem`
                 // on every frame. Fix pinned on the bench with `sudo docker logs homeassistant`.
                 "\"lm\":\"\",\"lip\":\"\",\"ws\":-1,"
                 // palcount = the real built-in count (matches the palettes[] array below); fxcount
                 // stays 1 (this shim exposes one effect surface). cpal/umpal = 0 (no custom palettes).
                 "\"fxcount\":1,\"palcount\":%u,\"cpalcount\":0,\"umpalcount\":0,\"str\":false}",
                 static_cast<unsigned>(platform::freeHeap()),
                 static_cast<unsigned>(platform::millis() / 1000u),
                 static_cast<unsigned>(mm::palettes::kCount));
    // effects + palettes — python-wled's __pre_deserialize__ turns each array into an indexed dict.
    // effects stays one real entry ("Solid"): this shim drives a single Layer, so a longer effect list
    // would be a lie. palettes is the REAL built-in list (Palette.h paletteNames / kBuiltins) so HA's
    // palette dropdown offers every palette the device has, indexed to match seg[0].pal and the Drivers
    // `palette` control — the same one-narrow-reach into light/ that the representative colour uses.
    sink.appendf(",\"effects\":[\"Solid\"],\"palettes\":[");
    mm::paletteNames(sink);
    sink.appendf("]}");
    sink.flush();
}

// /json/si — the combined {state, info} the WLED app reads in one call for its card.
void HttpServerModule::serveWledStateInfo(platform::TcpConnection& conn) {
    const char* header =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));

    const char* name; uint8_t mac[6]; uint8_t ip[4];
    resolveWledIdentity(name, mac, ip);
    (void)ip;  // /json/si's info body carries no IP field; keep the call uniform.

    JsonSink sink(conn);
    sink.appendf("{\"state\":");
    writeWledStateBody(sink);
    sink.appendf(",\"info\":");
    writeWledInfoBody(sink, name, mac);
    sink.appendf("}");
    sink.flush();
}

// Apply a WLED state-set body ({on?, bri?}) to the Drivers controls through the shared apply-core
// (the same path /api/control and Improv APPLY_OP use). `on` and `bri` are independent: `on` sets
// the real master-power control (so toggling off preserves the brightness level), `bri` sets the
// level. Shared by the HTTP POST /json/state handler and the inbound-WebSocket path.
void HttpServerModule::applyWledState(const char* body) {
    if (mm::json::hasKey(body, "on")) {
        applySetControl("Drivers", "on",
                        mm::json::parseBool(body, "on") ? "{\"value\":true}" : "{\"value\":false}");
    }
    if (mm::json::hasKey(body, "bri")) {
        int bri = mm::json::parseInt(body, "bri");
        if (bri < 0) bri = 0;
        if (bri > 255) bri = 255;
        char valueJson[32];
        std::snprintf(valueJson, sizeof(valueJson), "{\"value\":%d}", bri);
        applySetControl("Drivers", "brightness", valueJson);
    }
    // WLED palette: seg[0].pal is the palette index. HA's WLED integration writes here when a user
    // picks from the palette dropdown (the entries served by paletteNames in /json). It maps straight
    // to the Drivers `palette` control — the direct-index counterpart to the col[] nearest-match below;
    // both feed the same control, so the dropdown and the colour picker stay one value. Parsed from the
    // segment object so a top-level stray "pal" can't hijack it.
    const char* segStart = std::strstr(body, "\"seg\":");
    const char* palStart = segStart ? std::strstr(segStart, "\"pal\":") : nullptr;
    if (palStart) {
        int pal = std::atoi(palStart + 6);
        if (pal < 0) pal = 0;
        if (pal >= mm::palettes::kCount) pal = mm::palettes::kCount - 1;
        char valueJson[24];
        std::snprintf(valueJson, sizeof(valueJson), "{\"value\":%d}", pal);
        applySetControl("Drivers", "palette", valueJson);
    }
    // WLED colour: seg[0].col[0] is [r,g,b]. HA's WLED integration writes here when a user picks a
    // colour in the RGB picker. Palettes::nearestForRgb is the canonical RGB→palette entry (see the
    // comment at its declaration): it applies the same RGB→(hue,sat) conversion representativeHueSat
    // uses on the palette side, then runs the 2D-distance sweep. Value channel is ignored — HA's own
    // brightness slider handles bri via the `bri` field above.
    const char* colStart = std::strstr(body, "\"col\":[[");
    if (colStart) {
        int r = 0, g = 0, b = 0;
        if (std::sscanf(colStart + 8, "%d,%d,%d", &r, &g, &b) == 3) {
            const uint8_t rc = static_cast<uint8_t>(r < 0 ? 0 : (r > 255 ? 255 : r));
            const uint8_t gc = static_cast<uint8_t>(g < 0 ? 0 : (g > 255 ? 255 : g));
            const uint8_t bc = static_cast<uint8_t>(b < 0 ? 0 : (b > 255 ? 255 : b));
            const uint8_t idx = mm::Palettes::nearestForRgb(rc, gc, bc);
            char valueJson[24];
            std::snprintf(valueJson, sizeof(valueJson), "{\"value\":%u}", static_cast<unsigned>(idx));
            applySetControl("Drivers", "palette", valueJson);
        }
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
        static_cast<unsigned>(mod->tickTimeUs()),
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

    // Lifecycle in Scheduler::setup() order: defineControls() (bind buffers) →
    // setup() (may read them) → applyState() (build if effectively-enabled, else release).
    mod->defineControls();
    mod->setup();
    mod->applyState();
    if (scheduler_) scheduler_->prepareTree();
    requestFullResync();   // structural change (see requestFullResync)

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
// of stacking). Same removeChild → release → deleteTree the HTTP delete does.
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
        c->release();
        Scheduler::deleteTree(c);
        removedAny = true;
    }
    if (removedAny) {
        if (scheduler_) scheduler_->prepareTree();
    requestFullResync();   // structural change (see requestFullResync)
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
// discrepancy is documented in docs/moonmodules/core/moxygen/ImprovProvisioningModule.md.
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
    // top-level shape is policy-fixed. Reject the delete here instead of release+delete'ing
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
    mod->release();
    Scheduler::deleteTree(mod);

    if (scheduler_) scheduler_->prepareTree();
    requestFullResync();   // structural change (see requestFullResync)

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
    fresh->defineControls();
    fresh->setup();
    fresh->applyState();

    // Tear down the old subtree (release + recursive delete) — same pair
    // FilesystemModule::applyNode uses; a bare delete would leak its children.
    if (old) {
        old->release();
        Scheduler::deleteTree(old);
    }

    // Disambiguate only now that the tree is in its final shape: `fresh` is in
    // place and `old` is gone. Run before this and firstByName wouldn't find
    // `fresh` (not yet linked) and would append a spurious " 2"; run after the
    // old module is removed and a genuine same-named sibling is the only thing
    // that triggers a suffix. No-op for a preserved custom name that's unique.
    if (scheduler_) scheduler_->ensureUniqueName(fresh);

    // Re-run prepare across the tree so Layer LUT / Drivers buffer
    // wiring re-forms — a replaced effect/driver re-wires like a freshly added one.
    if (scheduler_) scheduler_->prepareTree();
    requestFullResync();   // structural change (see requestFullResync)

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
    probe->defineControls();
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
    probe->release();
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
    if (scheduler_) scheduler_->prepareTree();
    requestFullResync();   // structural change (see requestFullResync)
    sendResponse(conn, 200, "application/json", "{\"ok\":true}");
}

// Resolve `/api/list/<module>/<control>[/<id>]` (the tail after "/api/list/") into the module's
// editable List source, the parsed id (if the tail has one), and a flag saying whether an id was
// present. Returns nullptr (and sends the right 4xx) on any failure: bad path, unknown module or
// control, a control that isn't an editable list. Shared by the add / patch / delete handlers so
// the parse + validation lives once.
ListSource* HttpServerModule::resolveEditableList(platform::TcpConnection& conn, const char* tail,
                                                  uint32_t& outId, bool& outHasId) {
    // Split the tail into "<module>/<control>[/<id>]" on '/'. Names have no '/', so two slashes
    // at most: module, control, and an optional numeric id.
    char moduleName[32] = {};
    char controlName[32] = {};
    outHasId = false;
    outId = 0;
    const char* s1 = std::strchr(tail, '/');
    if (!s1) { sendResponse(conn, 400, "application/json", "{\"error\":\"bad list path\"}"); return nullptr; }
    const size_t mLen = static_cast<size_t>(s1 - tail);
    if (mLen == 0 || mLen >= sizeof(moduleName)) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"bad module\"}"); return nullptr;
    }
    std::memcpy(moduleName, tail, mLen);
    const char* cStart = s1 + 1;
    const char* s2 = std::strchr(cStart, '/');
    const size_t cLen = s2 ? static_cast<size_t>(s2 - cStart) : std::strlen(cStart);
    if (cLen == 0 || cLen >= sizeof(controlName)) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"bad control\"}"); return nullptr;
    }
    std::memcpy(controlName, cStart, cLen);
    if (s2 && s2[1]) {   // an id segment follows the control
        // Bounded parse (same rigour as the Content-Length parse above): require at least one digit,
        // reject overflow and any trailing non-digit, so a malformed id ("/5abc", "/xyz", an overflow)
        // is a clean 400 rather than a silently-truncated or zero id.
        const char* idStart = s2 + 1;
        char* idEnd = nullptr;
        errno = 0;
        const unsigned long parsed = std::strtoul(idStart, &idEnd, 10);
        if (idEnd == idStart || *idEnd != '\0' || errno == ERANGE || parsed > 0xFFFFFFFFul) {
            sendResponse(conn, 400, "application/json", "{\"error\":\"bad id\"}"); return nullptr;
        }
        outId = static_cast<uint32_t>(parsed);
        outHasId = true;
    }

    MoonModule* mod = findModuleByName(moduleName);
    if (!mod) { sendResponse(conn, 404, "application/json", "{\"error\":\"module not found\"}"); return nullptr; }
    auto& cs = mod->controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (cs[i].type == ControlType::List && std::strcmp(cs[i].name, controlName) == 0) {
            auto* src = static_cast<ListSource*>(cs[i].ptr);
            if (!src || !src->isEditableList()) {
                sendResponse(conn, 400, "application/json", "{\"error\":\"list not editable\"}");
                return nullptr;
            }
            listMutationModule_ = mod;   // remembered so afterListMutation marks IT dirty (persistence)
            return src;
        }
    }
    sendResponse(conn, 404, "application/json", "{\"error\":\"control not found\"}");
    return nullptr;
}

// After a list mutation: persist the owning module's storage and re-run the tree so a consumer
// (a driver referencing a preset by id) picks up the change on the next prepare. Mirrors the
// add/delete/move module handlers' dirty + prepareTree tail.
void HttpServerModule::afterListMutation() {
    // Mark the owning module dirty so its subtree is actually written — noteDirty() alone only sets
    // the debounce flag; the flush loop skips a subtree whose module isn't dirty (subtreeDirty). This
    // is the same markDirty()+noteDirty() pair the add/delete/move module handlers use; without the
    // markDirty a mutated list persisted nothing and was lost on reboot.
    if (listMutationModule_) listMutationModule_->markDirty();
    FilesystemModule::noteDirty();
    if (scheduler_) {
        // Rebuild EVERY module's controls: a list mutation can change what OTHER modules present —
        // adding/removing a light preset changes the option set of every driver's `preset` Select
        // (which is built from the library). Without this, a driver's Select keeps its stale option
        // count and a just-added preset is unselectable ("value out of range"). Mirrors the phase-2b
        // tree-wide rebuild after persistence load.
        for (uint8_t i = 0; i < scheduler_->moduleCount(); i++)
            if (auto* m = scheduler_->module(i)) m->rebuildControls();
        // Re-resolve each driver's preset → correction so an EDIT flows to output immediately. This
        // is a tier-1 correction refresh (rebuildCorrection → onCorrectionChanged), NOT a tier-3
        // prepareTree(): a preset edit changes correction data, not pipeline STRUCTURE, so it must
        // not re-run prepare() — that reinits each driver's output peripheral (an RMT channel
        // teardown blanks the strip for a tick, even on drivers not using the edited preset), which
        // Live-reconfiguration forbids (a config change applies with no visible glitch). Drivers is
        // the one container that owns driver corrections; core already couples to it (latestSummary).
        if (auto* drivers = static_cast<Drivers*>(findModuleByName("Drivers")))
            drivers->rebuildAllCorrections();
        // The tree-wide rebuildControls() above changed visible SCHEMA (option sets, hidden flags);
        // each of those rebuildControls() calls fires the schema-changed hook → requestFullResync(),
        // so connected clients re-read the fresh schema. No explicit resync needed here.
    }
}

void HttpServerModule::handleListAddRow(platform::TcpConnection& conn, const char* tail) {
    uint32_t id; bool hasId;
    ListSource* src = resolveEditableList(conn, tail, id, hasId);
    if (!src) return;   // response already sent
    uint32_t newId = 0;
    if (!src->addListRow(newId)) {
        sendResponse(conn, 409, "application/json", "{\"error\":\"list full or add refused\"}");
        return;
    }
    afterListMutation();
    char body[48];
    std::snprintf(body, sizeof(body), "{\"ok\":true,\"id\":%lu}", static_cast<unsigned long>(newId));
    sendResponse(conn, 200, "application/json", body);
}

void HttpServerModule::handleListPatchRow(platform::TcpConnection& conn, const char* tail, const char* jsonBody) {
    uint32_t id; bool hasId;
    ListSource* src = resolveEditableList(conn, tail, id, hasId);
    if (!src) return;
    if (!hasId) { sendResponse(conn, 400, "application/json", "{\"error\":\"row id required\"}"); return; }
    // A PATCH is either a reorder ({"to":N}) or a field edit ({"field":F,"value":V}).
    if (mm::json::hasKey(jsonBody, "to")) {
        int to = mm::json::parseInt(jsonBody, "to");
        // Bound before the uint8_t cast: a value > 255 would wrap (300 → 44) into a valid-looking
        // but wrong target index. Reject anything outside 0..255 up front; moveListRow validates the
        // remaining range against the actual row count.
        if (to < 0 || to > 255 || !src->moveListRow(id, static_cast<uint8_t>(to))) {
            sendResponse(conn, 400, "application/json", "{\"error\":\"move failed\"}");
            return;
        }
    } else {
        char field[32] = {};
        mm::json::parseString(jsonBody, "field", field, sizeof(field));
        if (!field[0]) { sendResponse(conn, 400, "application/json", "{\"error\":\"field required\"}"); return; }
        if (!src->setListRowField(id, field, jsonBody)) {
            sendResponse(conn, 400, "application/json", "{\"error\":\"field edit failed\"}");
            return;
        }
    }
    afterListMutation();
    sendResponse(conn, 200, "application/json", "{\"ok\":true}");
}

void HttpServerModule::handleListDeleteRow(platform::TcpConnection& conn, const char* tail) {
    uint32_t id; bool hasId;
    ListSource* src = resolveEditableList(conn, tail, id, hasId);
    if (!src) return;
    if (!hasId) { sendResponse(conn, 400, "application/json", "{\"error\":\"row id required\"}"); return; }
    if (!src->deleteListRow(id)) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"delete failed (bad id or protected)\"}");
        return;
    }
    afterListMutation();
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
    if (!otaTryStart()) {
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
                                     &g_otaBytesRead, &g_otaBytesTotal,
                                     &g_otaInFlight)) {
        otaFinish();
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
            // A new client needs the FULL state, not a patch against a baseline it never received.
            // Global cache → resync everyone (cheap, connects are rare); the next push sends full state.
            requestFullResync();
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

    if (fullResyncPending_) {
        // FULL STATE — sent on connect and after a structural change (a value patch can't describe a
        // reshaped tree). It's the one large frame (~30 KB), so route it through the resumable sender
        // to drain in chunks on tick20ms, NOT a blocking write on the render tick. buildStateJson
        // serialises the WHOLE tree — the expensive path — but only when fullResyncPending_, not every
        // second.
        // The shared send slot may hold an in-flight frame. A borrowed PREVIEW frame is a *view* — the
        // full state is what makes a freshly-connected client usable at all, so the resync PREEMPTS a
        // preview (otherwise continuous preview from another client could keep the new client blank;
        // preview resumes on its next frame). An OWNED frame in flight is ITSELF a state drain from a
        // prior push that hasn't finished — don't stomp it; let it complete and skip this push (the
        // slot is single-occupancy, and a half-then-half state is worse than one whole one arriving a
        // tick later). fullResyncPending_ stays TRUE until startBufferedTextSend actually accepts the
        // new payload, so a rejected/failed start retries next tick rather than dropping the resync.
        if (!bufferedSendIdle()) {
            if (previewSend_.ownsBody) return;   // a state drain is already in flight — let it finish
            cancelBufferedSend();                // preempt a borrowed preview
        }
        JsonSink sink;
        buildStateJson(sink);
        const size_t len = sink.size();
        char* owned = sink.detach();   // move ownership to the sender (frees on drain-complete)
        if (owned && startBufferedTextSend(owned, len)) {
            baselineLeafHashes();       // the full state IS the new baseline — next tick patches from here
            fullResyncPending_ = false;   // cleared only on a confirmed accept; a failed start retries
        }
    } else {
        // PATCH — the steady-state path. buildStatePatch walks the tree, value-hashes each leaf, and
        // emits ONLY the ones whose value changed since the last push (typically a handful of telemetry
        // leaves, ~1–2 KB). This is the whole fix: the 30 KB of unchanging option/detail metadata is
        // NEVER serialised or sent here, so tick1s no longer spikes the render thread. The patch is
        // small, so it sends inline (no resumable drain) — a non-blocking per-client write of ~2 KB.
        JsonSink sink;
        const uint16_t changed = buildStatePatch(sink);
        if (changed > 0) {
            for (auto& ws : wsClients_) {
                if (!ws.valid()) continue;
                if (!sendWsTextFrame(ws, sink.data(), static_cast<int>(sink.size()))) ws.close();
            }
        }
        // changed == 0 → nothing to send this second (an idle device); the common quiet case.
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

    const char* name; uint8_t mac[6]; uint8_t ip[4];
    resolveWledIdentity(name, mac, ip);
    (void)ip;  // the WS-push info body carries no IP field; keep the call uniform.

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
// Build a WS frame header (FIN + `opcode`, unmasked; 7/16/64-bit length form) for a `payloadLen`-byte
// payload into previewSend_.hdr[0..]. Returns the header length. Shared by the binary (preview) and
// text (state) buffered sends so the length-form logic lives once.
static size_t writeWsFrameHeader(uint8_t* h, uint8_t opcode, size_t payloadLen) {
    h[0] = opcode;
    if (payloadLen < 126) { h[1] = static_cast<uint8_t>(payloadLen); return 2; }
    if (payloadLen < 65536) {
        h[1] = 126; h[2] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
        h[3] = static_cast<uint8_t>(payloadLen & 0xFF); return 4;
    }
    h[1] = 127;
    for (int i = 0; i < 8; i++)
        h[2 + i] = static_cast<uint8_t>((static_cast<uint64_t>(payloadLen) >> (56 - 8 * i)) & 0xFF);
    return 10;
}

bool HttpServerModule::sendBufferedFrame(const uint8_t* header, size_t headerLen,
                                         const uint8_t* body, size_t bodyLen) {
    // Drop-new backpressure: one frame in flight at a time. A caller that asks while a send is active
    // is told "busy" — the in-flight frame is kept and this new one is rejected, which the producer
    // reads as "link is behind" and uses to shed frame rate (it requeues nothing, so the loop runs on).
    if (previewSend_.active) return false;

    const size_t totalLen = headerLen + bodyLen;   // WS payload length = app header + body
    // Build the WS frame header (binary opcode) directly into previewSend_.hdr, followed by the app
    // header — so the cursor streams them as one span.
    const size_t wsLen = writeWsFrameHeader(previewSend_.hdr, 0x82, totalLen);
    // The app header follows the WS header in the same buffer. sizeof(hdr)=16 holds the 10-byte WS
    // form + the preview app headers (≤10 bytes); guard so a future larger header can't overrun.
    if (wsLen + headerLen > sizeof(previewSend_.hdr)) return false;
    // memcpy, not a hand-rolled byte loop: the loop indexed hdr[wsLen + i], and the compiler cannot
    // see through writeWsFrameHeader that wsLen is at most 10 — so it must assume the index could be
    // anywhere and warns on the write (-Wstringop-overflow). memcpy states the same intent with the
    // destination and length in one expression, which it CAN check against the guard above.
    std::memcpy(previewSend_.hdr + wsLen, header, headerLen);

    previewSend_.hdrLen = wsLen + headerLen;
    previewSend_.body = body;
    previewSend_.bodyLen = bodyLen;
    previewSend_.ownsBody = false;   // preview borrows its pixel buffer (kept alive by PreviewDriver)
    for (int i = 0; i < MAX_WS_CLIENTS; i++) previewSend_.sent[i] = 0;
    previewSend_.active = true;
    // Deliberately do NOT drain here. sendBufferedFrame is called from PreviewDriver's tick() on the
    // RENDER thread; a socket writeSome is variable-cost (0..~ms) and would land that cost — and its
    // jitter — directly on the render tick, hitching the LEDs. So we only queue the frame (copy the
    // header, point at the body) and let drainPreviewSend() push bytes purely on tick20ms, off the
    // render hot path. The frame starts draining within one transport poll (≤20 ms).
    return true;
}

// Queue a TEXT frame whose body this module OWNS, through the same resumable slot. Used by the state
// push so the 20 KB JSON drains in chunks on tick20ms rather than a blocking write on the render tick.
bool HttpServerModule::startBufferedTextSend(char* ownedBody, size_t bodyLen) {
    // A send already in flight: drop this one and free its buffer — the next second's state is fresher.
    if (previewSend_.active) { platform::free(ownedBody); return false; }
    // No app header for the state frame (the JSON is the whole payload), just the WS text header.
    const size_t wsLen = writeWsFrameHeader(previewSend_.hdr, 0x81, bodyLen);
    previewSend_.hdrLen = wsLen;
    previewSend_.body = reinterpret_cast<const uint8_t*>(ownedBody);
    previewSend_.bodyLen = bodyLen;
    previewSend_.ownsBody = true;    // we allocated this JSON buffer; the drain frees it when done
    for (int i = 0; i < MAX_WS_CLIENTS; i++) previewSend_.sent[i] = 0;
    previewSend_.active = true;
    return true;   // drained on tick20ms, same as preview — never a blocking write on the render tick
}

// Per-client cursor over the logical [hdr ++ body] stream: write whatever the socket takes now (up
// to one memory-adaptive chunk), advance the cursor, leave the rest for the next tick. A real
// socket error closes that client (its WS message ends incomplete → the browser discards it). The
// send completes when every live client has the whole frame, or when no client is left.
void HttpServerModule::drainPreviewSend() {
    // Core-0 side of the sender lease. The offloaded PreviewDriver (core 1) holds this while it arms a
    // frame or streams the coordinate table; taking it here keeps this drain's socket writes from
    // interleaving with that stream inside one WS frame, and keeps us off a half-armed previewSend_.
    // try_lock, not a wait: this runs on the render thread's tick20ms, where blocking is forbidden —
    // core 1 releases within one message, so we simply drain on the next 20 ms tick instead.
    LockGuard lease{wsLock_};
    if (!lease) return;
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
    if (!anyLiveClient || allDone) {
        if (previewSend_.ownsBody) {   // free the state JSON buffer we allocated for this frame
            platform::free(const_cast<uint8_t*>(previewSend_.body));
            previewSend_.body = nullptr;
            previewSend_.ownsBody = false;
        }
        previewSend_.active = false;
    }
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
