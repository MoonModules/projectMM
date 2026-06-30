#pragma once

#include "light/drivers/Drivers.h"
#include "core/JsonUtil.h"          // parse the bridge's JSON responses
#include "core/FilesystemModule.h"  // noteDirty — persist the app key after pairing
#include "core/DevicesModule.h"     // DevicesModule::active() — list the bridge as a device
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

// Philips Hue lights as a projectMM OUTPUT — a driver, not a listed device. The bulbs are
// pixels of an effect: make a small grid (e.g. 4×1×1), run any effect, and this driver reads
// its window of the shared buffer and pushes each light's colour to the bridge. Same shape as
// NetworkSendDriver (read a window, send it out), but over the Hue v1 HTTP API instead of UDP.
//
// What makes Hue different from a strip and shapes the design:
//  - It's HTTP, not a wire protocol: GET /api/<key>/lights, PUT .../lights/<id>/state.
//  - Connection churn, not just Hue's ~10/s command budget, bounds the rate: each PUT opens a
//    fresh TCP connection (the bridge speaks Connection: close), so loop() does AT MOST ONE PUT
//    every kPutIntervalMs — a millis() gate, never work-every-tick — so a synchronous round-trip
//    can't stall the single-thread render loop AND the TIME_WAIT sockets don't pile up. Smooth
//    ambient colour, not real-time (that's the Entertainment API, a separate future).
//  - Only colour-capable, reachable lights are driven (parseLights filters them); each PUT sends
//    hue/sat/bri from a textbook RGB→HSV plus a cadence-matched transitiontime so the bulb glides.
//  - It needs an app key: press the bridge's link button once, then POST /api to claim a key.
//    Pairing is a short bounded poll across a few loop1s ticks (never blocking the loop).
//
// Room / light selection. Two dropdowns ("room", "light") let the user aim the effect at a
// subset of the bridge's colour bulbs without touching the window controls. fetchGroups reads
// GET /api/<key>/groups and keeps the bridge's Rooms (name + member light ids); fetchLights keeps
// each colour light's name. Option index 0 is "All" in BOTH dropdowns, so the common "drive
// everything" case never shifts index and persists as 0 for free (the Select stores its uint8
// index). Picking a room narrows the light dropdown to that room's colour lights AND filters the
// driven set to that room; picking a specific light drives just that one. The filter builds
// drivenIdx_[] — the colour-light subset pushOneChangedLight actually walks — so room=All &
// light=All leaves the original behaviour (drive every colour bulb) untouched.
//
// Plain HTTP, no TLS — the Hue v1 API allows it (bench-confirmed on a BSB002 bridge). Prior
// art: the Hue v1 CLIP API (public docs); the effect-as-output mapping is projectMM's own.
class HueDriver : public DriverBase {
public:
    uint8_t  bridgeIp[4] = {};           // the bridge's LAN IP (from the UI)
    char     appKey[48] = {};            // the Hue username/app key (filled by Pair, persisted)

    void onBuildControls() override {
        controls_.addIPv4("bridgeIp", bridgeIp);
        controls_.addText("appKey", appKey, sizeof(appKey));   // persisted credential
        controls_.addButton("pair");                            // link-button pairing
        // Room + light filter. Both default to index 0 = "All". The option arrays are rebuilt
        // (in place, into stable member buffers) from the parsed bridge data; onBuildControls is
        // re-run on every control change (HttpServerModule), so these reflect the current room_.
        buildRoomOptions();
        buildLightOptions();
        controls_.addSelect("room", room_, roomOptions_, roomOptionCount_);
        controls_.addSelect("light", light_, lightOptions_, lightOptionCount_);
        addWindowControls();                                    // start / count — its slice of the buffer
        // The generic "status" line (setStatus) carries the pairing state + driven-of-total light
        // count — see refreshStatus(); no separate hueStatus / colourLights controls.
        refreshStatus();
    }

    void setSourceBuffer(Buffer* buf) override { sourceBuffer_ = buf; }

    // The shared output Correction (global brightness LUT + channel order), same as the physical
    // LED / network drivers — so the brightness slider and a swapped colour order reach the Hue
    // lights too. Applied per pixel before RGB→HSV; the RGBW/white part is irrelevant here (Hue
    // takes hue/sat), we use the RGB result.
    void setCorrection(const Correction* c) override { correction_ = c; }

    // A control click. "pair" starts the link-button pairing poll. Changing the bridge IP or app
    // key points the driver at a (possibly) different bridge, so the learned light list + push
    // cache are stale — drop them and let loop1s re-fetch against the new config.
    void onUpdate(const char* controlName) override {
        if (controlName && std::strcmp(controlName, "pair") == 0) {
            pairTicksLeft_ = kPairWindowTicks;   // begin: poll the bridge for ~30 s on loop1s
            std::snprintf(statusBuf_, sizeof(statusBuf_), "pairing: press the bridge button");
            setStatus(statusBuf_);
        } else if (controlName &&
                   (std::strcmp(controlName, "bridgeIp") == 0 || std::strcmp(controlName, "appKey") == 0)) {
            resetLightCache();   // re-fetch the light list + groups for the new bridge/key
        } else if (controlName && std::strcmp(controlName, "room") == 0) {
            // Room changed: the light dropdown's options now describe a different set, so the old
            // light_ index may point past the new (shorter) list — clamp it back to "All". The
            // option arrays themselves were already rebuilt by the rebuildControls() that ran just
            // before this onUpdate (it re-ran onBuildControls() against the new room_). Recompute
            // the driven subset and refresh the status line.
            if (light_ >= lightOptionCount_) light_ = 0;
            rebuildDriven();
            refreshStatus();
        } else if (controlName && std::strcmp(controlName, "light") == 0) {
            rebuildDriven();     // a different single light (or back to room's all)
            refreshStatus();     // the driven-of-total count changed → refresh the status line
        }
        DriverBase::onUpdate(controlName);
    }

    // loop() runs every render tick. It must NEVER do more than ONE bounded bridge call, and
    // only when the rate-limit interval has elapsed (a millis() gate, NOT work-every-tick) —
    // otherwise a synchronous HTTP round-trip stalls the whole single-thread render loop (the
    // "never block the loop" rule, decisions.md). So: at most one PUT every kPutIntervalMs,
    // round-robined across the lights. Pairing + the bridge announce ride the slow 1 Hz tick.
    void loop() override {
        if (pairTicksLeft_ > 0) return;            // pairing owns the bridge during its window
        if (!appKey[0] || !haveBridge() || lightCount_ == 0) return;
        const uint32_t now = platform::millis();
        if (now - lastPutMs_ < kPutIntervalMs) return;   // not time yet — return instantly, no I/O
        lastPutMs_ = now;
        pushOneChangedLight();                     // exactly one bounded PUT this tick
    }

    // The 1 Hz tick handles the non-render-critical, slower bridge work: pairing poll, the
    // one-shot light fetch, and the periodic DevicesModule announce. Each is at most one bridge
    // call per second — acceptable on a 1 Hz tick, and never in the per-frame loop().
    void loop1s() override {
        if (pairTicksLeft_ > 0) { pollPairing(); DriverBase::loop1s(); return; }
        if (!appKey[0] || !haveBridge()) { DriverBase::loop1s(); return; }
        if (!sawLights_) { fetchLights(); DriverBase::loop1s(); return; }
        if (!sawGroups_) { fetchGroups(); DriverBase::loop1s(); return; }
        if (++reportTick_ >= kReportEverySec) { reportTick_ = 0; reportBridge(); }
        DriverBase::loop1s();
    }

    void teardown() override {
        pairTicksLeft_ = 0;
        freeNameBuffers();   // release the dropdown-name heap; a re-add re-fetches and re-allocs
        DriverBase::teardown();
    }

    // Test seam: drive the changed-light diff + PUT formatting without a live bridge — feed a
    // light's RGB and get back whether it would PUT + the body it would send. Records the push
    // (like pushChangedLights does) so a follow-up call with the same RGB exercises the
    // unchanged-skip path.
    bool wouldPushForTest(uint8_t idx, uint8_t r, uint8_t g, uint8_t b, char* outBody, size_t cap) {
        if (!diffAndFormat(idx, r, g, b, outBody, cap)) return false;
        if (idx < kMaxLights) {
            lastRgb_[idx][0] = r; lastRgb_[idx][1] = g; lastRgb_[idx][2] = b;
            sent_[idx] = true;
        }
        return true;
    }

    // Test seam: parse a real /lights JSON body through fetchLights' colour-light extractor.
    void parseLightsForTest(const char* json) { parseLights(json); rebuildDriven(); }
    uint8_t lightCountForTest() const { return lightCount_; }    // kept colour+reachable lights
    uint16_t hueIdForTest(uint8_t i) const { return i < kMaxLights ? hueId_[i] : 0; }
    int8_t colourCountForTest() const { return colourCount_; }

    // Test seam: parse a real /groups JSON body through fetchGroups' Room extractor. Call
    // parseLightsForTest FIRST — room membership resolves against the known colour lights (hueId_),
    // exactly as production order guarantees (fetchGroups runs only after fetchLights).
    void parseGroupsForTest(const char* json) { parseGroups(json); rebuildDriven(); }
    uint8_t roomCountForTest() const { return roomCount_; }      // kept Rooms (type=="Room")

    // Test seams for the room→light filtering. setRoomForTest/setLightForTest mirror what a UI
    // Select change does (write the index, then re-derive the driven subset); drivenCountForTest /
    // drivenIdForTest report the filtered set pushOneChangedLight walks.
    void setRoomForTest(uint8_t r) { room_ = r; if (light_ >= lightOptionCount_) light_ = 0; rebuildDriven(); refreshStatus(); }
    void setLightForTest(uint8_t l) { light_ = l; rebuildDriven(); refreshStatus(); }
    void refreshStatusForTest() { refreshStatus(); }
    uint8_t drivenCountForTest() const { return drivenLightCount_; }
    uint16_t drivenIdForTest(uint8_t i) const { return i < drivenLightCount_ ? hueId_[drivenIdx_[i]] : 0; }

    // Test seam for the RGB→HSV mapping (no bridge needed).
    static void rgbToHsvForTest(uint8_t r, uint8_t g, uint8_t b, uint16_t& h, uint8_t& s, uint8_t& v) {
        rgbToHsv(r, g, b, h, s, v);
    }

    // Test seam: the truncation signal fetchLights grows against (a complete /lights body ends '}').
    static bool bodyLooksCompleteForTest(const char* body) { return bodyLooksComplete(body); }

private:
    static constexpr uint8_t kMaxLights = 32;        // a LAN's worth of Hue bulbs; bounded, no heap
    static constexpr uint8_t kMaxRooms  = 16;        // bounded room count; option index 0 is "All"
    static constexpr uint8_t kNameLen   = 24;        // per-light / per-room friendly-name buffer
    // kMaxLights == 32 == the width of a uint32_t, so a Room's colour-light membership fits one
    // bitmask (bit i ⇔ colour light hueId_[i]) — resolved at parse time, since fetchGroups runs
    // after fetchLights (the sawGroups_ gate), so hueId_ is already populated. A bitmask is the
    // textbook small-set membership (a bit test replaces a per-id scan), and 16×4 B = 64 B beats a
    // 16×32 id-list's 1 KB inline. static_assert pins the width assumption.
    static_assert(kMaxLights == 32, "Room membership bitmask (roomMask_) assumes 32 colour lights");
    // One PUT at most every kPutIntervalMs (a millis() gate in loop()). Each PUT opens a fresh
    // TCP connection (the bridge speaks Connection: close), so the rate is bounded by connection
    // CHURN, not just Hue's command budget: at ~7/s the TIME_WAIT sockets pile into the hundreds
    // and the bridge starts refusing connections (PUTs fail, lights freeze). 500 ms → ~2 PUTs/s
    // keeps TIME_WAIT small and is plenty for smooth ambient colour (each light glides over its
    // ~2 s refresh via the matched transitiontime). Real-time would need keep-alive or the
    // Entertainment API — out of scope; this is the standard API's comfortable rate.
    static constexpr uint32_t kPutIntervalMs = 500;
    static constexpr int     kPairWindowTicks = 30;  // ~30 s pairing window (link-button press)
    static constexpr uint16_t kReportEverySec = 30;  // re-announce the bridge to DevicesModule
    // Per-frame PUT (loop()) timeout. A successful PUT to a LAN bridge returns in ~20-50 ms, so
    // this only bounds the WORST case (an unreachable/overloaded bridge) — not the normal cost.
    // 200 ms gives comfortable margin over the real latency (a 60 ms cap intermittently tripped
    // under rapid back-to-back PUTs, failing them) while still bounding a bad frame. kSlowTimeoutMs
    // is for the 1 Hz calls (pair / fetch / announce), where the 8 KB /lights GET wants headroom.
    static constexpr uint32_t kHttpTimeoutMs = 200;
    static constexpr uint32_t kSlowTimeoutMs = 400;

    Buffer* sourceBuffer_ = nullptr;
    const Correction* correction_ = nullptr;   // shared brightness LUT + channel order (may be null)

    // Per-light Hue id + the last RGB we pushed (the changed-only filter). hueId maps a window
    // index → the bridge's light id, learned from GET /api/<key>/lights.
    uint16_t hueId_[kMaxLights] = {};
    uint8_t  lastRgb_[kMaxLights][3] = {};
    bool     sent_[kMaxLights] = {};                 // have we pushed this light at least once
    // hueId_ holds ONLY colour-capable lights (the bridge's "Extended color light"s) — a
    // dimmable-only white or an on/off plug is skipped, so every window pixel maps to a bulb
    // that can show the effect's full colour. lightCount_ is that filtered count.
    uint8_t  lightCount_ = 0;                         // number of colour-capable lights
    int8_t   colourCount_ = 0;                        // same, as the read-only control / bridge field
    bool     sawLights_ = false;                      // fetchLights ran → the list is trustworthy
    // Friendly names for the dropdowns. Heap, NOT inline: a fixed [kMaxLights][kNameLen] array
    // would reserve 768 B whether the bridge has 4 lights or 32 (and cap at 32). Instead one
    // contiguous block of (count × kNameLen) is allocated to the ACTUAL light/room count when the
    // fetch runs, and freed in release()/teardown — so memory scales to the real bridge and
    // sizeof(HueDriver) stays small (the lightsBuf_ stack-overflow lesson, applied to the names).
    char*    lightNames_ = nullptr;                   // kMaxLights × kNameLen; lightNameAt(i) indexes it
    char*    roomNames_  = nullptr;                   // kMaxRooms  × kNameLen
    char* lightNameAt(uint8_t i) { return lightNames_ ? lightNames_ + static_cast<size_t>(i) * kNameLen : nullptr; }
    char* roomNameAt(uint8_t i)  { return roomNames_  ? roomNames_  + static_cast<size_t>(i) * kNameLen : nullptr; }
    // Allocate the two name blocks lazily on first parse (so an unconfigured driver pays nothing),
    // and free them on teardown / cache reset (so a removed-then-readded bridge starts clean). The
    // blocks are sized to the kMax bound, not the live count, because the parser fills them
    // incrementally and the count isn't known until it finishes — keeping the names off the
    // resident sizeof(HueDriver) is the win (the lightsBuf_ stack-probe lesson), not per-byte fit.
    void ensureNameBuffers() {
        if (!lightNames_) lightNames_ = static_cast<char*>(platform::alloc(static_cast<size_t>(kMaxLights) * kNameLen));
        if (!roomNames_)  roomNames_  = static_cast<char*>(platform::alloc(static_cast<size_t>(kMaxRooms)  * kNameLen));
    }
    void freeNameBuffers() {
        platform::free(lightNames_); lightNames_ = nullptr;
        platform::free(roomNames_);  roomNames_  = nullptr;
    }

    // --- Rooms (GET /api/<key>/groups, type=="Room"): name + a colour-light membership bitmask.
    uint32_t roomMask_[kMaxRooms] = {};               // bit i set ⇔ this Room references colour light hueId_[i]
    uint8_t  roomCount_ = 0;                           // number of Rooms kept
    bool     sawGroups_ = false;                      // fetchGroups ran → the room list is trustworthy

    // --- Filter selection (Select indices, persisted as uint8) and the derived driven subset.
    uint8_t  room_ = 0;                               // 0 = "All", else roomName_[room_-1]
    uint8_t  light_ = 0;                              // 0 = "All", else the n-th light of the current option list
    uint8_t  drivenIdx_[kMaxLights] = {};             // colour-light array-indices actually driven (after filter)
    uint8_t  drivenLightCount_ = 0;                   // size of drivenIdx_ — what pushOneChangedLight walks

    // --- Stable option pointer arrays for the two Selects. addSelect borrows the pointer; these
    // live for the driver's lifetime and are refilled in place (pointing into the *Name_ buffers)
    // by buildRoomOptions / buildLightOptions on every onBuildControls. "All" is always index 0.
    const char* roomOptions_[kMaxRooms + 1] = {};
    uint8_t     roomOptionCount_ = 1;
    const char* lightOptions_[kMaxLights + 1] = {};
    uint8_t     lightOptionCount_ = 1;

    uint8_t  pushCursor_ = 0;                         // round-robin position across the lights
    uint8_t  drivenCount_ = 0;                        // lights driven this pass (n); fade-time basis
    uint32_t lastPutMs_ = 0;                          // millis() of the last PUT — the loop() rate gate
    int      pairTicksLeft_ = 0;
    uint16_t reportTick_ = 0;                        // counts loop1s ticks toward kReportEverySec
    char     statusBuf_[40] = "unpaired";
    // /lights read buffer is sized dynamically in fetchLights (grow-and-retry): a small first try
    // covers a typical home; it doubles up to the cap only when a bigger bridge's response fills it.
    // 16 KB caps the worst case (kMaxLights=32 × ~512 B/light). Heap-allocated per fetch, never an
    // inline member (an 8 KB member would overflow the main-task stack in registerType's probe).
    static constexpr size_t kLightsBufInitial = 2048;
    static constexpr size_t kLightsBufMax     = 16384;

    bool haveBridge() const { return bridgeIp[0] || bridgeIp[1] || bridgeIp[2] || bridgeIp[3]; }

    // Does the JSON span [begin, end) contain `key` (e.g. "\"hue\"") — used to read a light's
    // capabilities off its state block (a colour light has "hue"; the bridge omits it otherwise).
    static bool containsKey(const char* begin, const char* end, const char* key) {
        const size_t kl = std::strlen(key);
        for (const char* s = begin; s + kl <= end; s++)
            if (std::strncmp(s, key, kl) == 0) return true;
        return false;
    }

    // Read a `"<key>":"<value>"` string from WITHIN a JSON object span [begin, end) — the
    // span-bounded analogue of containsKey, used to grab one light's / one room's "name" without
    // matching the first "name" elsewhere in the bridge's big response. Copies the raw value up to
    // its closing quote (the bridge's names carry no escapes worth decoding) into out[cap], NUL-
    // terminated; leaves out empty if the key isn't in the span.
    static void parseStringIn(const char* begin, const char* end, const char* key, char* out, size_t cap) {
        if (cap == 0) return;
        out[0] = 0;
        char search[24];
        std::snprintf(search, sizeof(search), "\"%s\":\"", key);
        const size_t sl = std::strlen(search);
        for (const char* s = begin; s + sl <= end; s++) {
            if (std::strncmp(s, search, sl) != 0) continue;
            const char* v = s + sl;
            size_t oi = 0;
            for (; v < end && *v != '"' && oi + 1 < cap; v++) out[oi++] = *v;
            out[oi] = 0;
            return;
        }
    }

    void bridgeStr(char out[16]) const {
        std::snprintf(out, 16, "%u.%u.%u.%u", bridgeIp[0], bridgeIp[1], bridgeIp[2], bridgeIp[3]);
    }

    // The single status line, folding what were three separate controls (status / hueStatus /
    // colourLights). Shows the pairing state and the light count as driven-of-total: "paired,
    // 3-4 lights" = the room/light filter narrowed 4 colour lights to 3 driven. When nothing is
    // filtered (driven == total) it collapses to the plain count, "paired, 4 lights".
    void refreshStatus() {
        if (!appKey[0]) std::snprintf(statusBuf_, sizeof(statusBuf_), "unpaired");
        else if (!lightCount_) std::snprintf(statusBuf_, sizeof(statusBuf_), "paired");
        else if (drivenLightCount_ < lightCount_)
            std::snprintf(statusBuf_, sizeof(statusBuf_), "paired, %u-%u lights", drivenLightCount_, lightCount_);
        else std::snprintf(statusBuf_, sizeof(statusBuf_), "paired, %u lights", lightCount_);
        setStatus(statusBuf_);
    }

    // --- Pairing: POST /api {"devicetype":"projectMM#<name>"} until the user presses the button.
    void pollPairing() {
        if (!haveBridge()) { pairTicksLeft_ = 0; std::snprintf(statusBuf_, sizeof(statusBuf_), "set bridge IP first"); setStatus(statusBuf_); return; }
        char host[16]; bridgeStr(host);
        // The pairing body is tiny, but httpRequest reads headers + body into one buffer and the
        // bridge's headers run ~700 bytes — size past them or the success body gets squeezed out.
        char resp[1024];
        int st = platform::httpRequest("POST", host, 80, "/api",
                                       "{\"devicetype\":\"projectMM#device\"}", kSlowTimeoutMs,
                                       resp, sizeof(resp));
        if (st == 200 && std::strstr(resp, "\"username\"")) {
            // [{"success":{"username":"<key>"}}] — extract the username.
            char key[48] = {};
            mm::json::parseString(resp, "username", key, sizeof(key));
            if (key[0]) {
                std::snprintf(appKey, sizeof(appKey), "%s", key);
                pairTicksLeft_ = 0;
                resetLightCache();                // clear the light list + the per-light push cache
                                                  // (sent_/lastRgb_) so the new session re-sends all
                refreshStatus();
                markDirty();                      // persist the new app key
                FilesystemModule::noteDirty();
                return;
            }
        }
        // "link button not pressed" → keep polling until the window elapses.
        if (--pairTicksLeft_ <= 0) {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "pairing timed out");
            setStatus(statusBuf_);
        }
    }

    // Drop the learned light list + room list + push cache so loop1s re-fetches (bridge/key change).
    void resetLightCache() {
        lightCount_ = 0;
        colourCount_ = 0;
        sawLights_ = false;
        roomCount_ = 0;
        sawGroups_ = false;
        pushCursor_ = 0;
        for (uint8_t i = 0; i < kMaxLights; i++) sent_[i] = false;
        freeNameBuffers();   // drop the old bridge's names; the re-fetch re-allocs for the new one
        rebuildDriven();   // empty caches → empty driven set, until the re-fetch repopulates them
    }

    // A complete /lights response is a JSON object: its last non-whitespace char is '}'. A read cut
    // short by a too-small buffer ends mid-content, so this is the truncation signal fetchLights
    // grows against. (Not a full JSON validator — the bridge's well-formed body is the contract;
    // this only distinguishes "whole" from "cut off".)
    static bool bodyLooksComplete(const char* body) {
        size_t len = std::strlen(body);
        while (len > 0 && (body[len - 1] == '\n' || body[len - 1] == '\r'
                           || body[len - 1] == ' ' || body[len - 1] == '\t')) len--;
        return len > 0 && body[len - 1] == '}';
    }

    // --- Learn the bridge's light ids (window index → hue id, in id order).
    void fetchLights() {
        char host[16]; bridgeStr(host);
        char path[80]; std::snprintf(path, sizeof(path), "/api/%s/lights", appKey);
        // The /lights body grows with the bridge's light count (~300-800 bytes/light of metadata),
        // so its size is unknown up front. Size the read buffer DYNAMICALLY: start small and, if the
        // response came back filling the buffer (httpRequest truncates to its capacity, which would
        // silently drop trailing lights from parseLights' linear scan), double and refetch until it
        // fits or we hit the cap. A typical home (a few lights) fits the first try; only a large
        // bridge grows. The buffer lives on the heap (PSRAM when present) for the fetch and is freed
        // after — fetchLights runs at 1 Hz, off the render loop, so the alloc/refetch isn't hot-path.
        // It is NOT an inline member: an 8 KB member would make sizeof(HueDriver) overflow the
        // main-task stack when registerType<HueDriver> constructs a throwaway probe.
        for (size_t cap = kLightsBufInitial; cap <= kLightsBufMax; cap *= 2) {
            char* buf = static_cast<char*>(platform::alloc(cap));
            if (!buf) return;
            const int st = platform::httpRequest("GET", host, 80, path, "", kSlowTimeoutMs, buf, cap);
            if (st != 200) { platform::free(buf); return; }
            // Detect a truncated read by the body's SHAPE, not its length: httpRequest strips the
            // HTTP headers in place, so strlen(body) is body-only and never reaches cap-1 even when
            // the raw read filled the buffer. So grow + retry while the body looks incomplete, until
            // it parses whole or we hit the cap (then parse best-effort).
            const bool truncated = !bodyLooksComplete(buf) && (cap < kLightsBufMax);
            if (!truncated) {
                parseLights(buf);
                rebuildControls();   // the light-dropdown options changed → re-bind for the UI
                refreshStatus();
                reportBridge();
                platform::free(buf);
                return;
            }
            platform::free(buf);
        }
    }

    // List the bridge in DevicesModule (so it shows alongside discovered WLED/projectMM peers,
    // carrying its dimmable-light count for layout sizing). The bridge isn't a UDP-presence
    // device, so it's registered explicitly through the static seam — no compile-time core↔light
    // dependency beyond the same DevicesModule::active() shape AudioModule::latestFrame() uses.
    void reportBridge() {
        auto* dev = DevicesModule::active();
        if (!dev || !haveBridge()) return;
        char host[16]; bridgeStr(host);
        // httpRequest reads headers + body into this one buffer, and the bridge's response
        // headers alone run ~700 bytes — so size for headers + the small config body, not just
        // the body, or the body gets squeezed out.
        char cfg[1024], name[24] = {};
        if (platform::httpRequest("GET", host, 80, "/api/0/config", "", kSlowTimeoutMs, cfg, sizeof(cfg)) == 200)
            mm::json::parseString(cfg, "name", name, sizeof(name));   // the bridge's friendly name
        dev->upsertHueBridge(bridgeIp, name, static_cast<uint8_t>(colourCount_));
    }

    // Extract the COLOUR-capable, REACHABLE light ids from a /lights JSON body:
    // {"1":{…},"5":{…},…}. A colour light's object carries a "hue" field in its state; a
    // dimmable-only white or an on/off plug does not. A light that's powered off / out of mesh
    // reports "reachable":false. We keep only lights that are BOTH colour-capable and reachable
    // — those are the ones an effect can actually animate right now — so the window maps every
    // pixel to a live colour bulb. The bridge response (~8 KB / hundreds of fields) exceeds the
    // recursive JSON reader's node arena, so this is a lightweight forward scan: spot each
    // top-level id key, then keep it iff its object span (up to the next id key) has both.
    void parseLights(const char* resp) {
        ensureNameBuffers();
        lightCount_ = 0;
        const char* p = resp;
        int pendingId = 0;               // a light id seen, not yet committed (need its span first)
        const char* pendingStart = nullptr;
        auto commit = [&](const char* objEnd) {
            if (pendingId > 0 && pendingStart && lightCount_ < kMaxLights && lightNames_
                && containsKey(pendingStart, objEnd, "\"hue\"")
                && containsKey(pendingStart, objEnd, "\"reachable\":true")) {
                hueId_[lightCount_] = static_cast<uint16_t>(pendingId);
                // Keep the friendly name for the dropdown — read the "name" string from this
                // light's object span (bounded, NUL-terminated). Falls back to the id if absent.
                char* name = lightNameAt(lightCount_);
                parseStringIn(pendingStart, objEnd, "name", name, kNameLen);
                if (!name[0]) std::snprintf(name, kNameLen, "%d", pendingId);
                lightCount_++;
            }
        };
        while (true) {
            const char* q = std::strchr(p, '"');           // next key open-quote
            if (!q) break;
            int id = std::atoi(q + 1);                      // light id is a quoted integer key
            const char* close = std::strchr(q + 1, '"');
            // A top-level light-id key: a quoted positive integer followed by ':'.
            if (id > 0 && close && close[1] == ':') {
                commit(q);                                  // the PREVIOUS light's object ends here
                pendingId = id;
                pendingStart = close + 1;
            }
            p = close ? close + 1 : q + 1;
        }
        commit(resp + std::strlen(resp));                   // the last light runs to the end
        sawLights_ = true;
        colourCount_ = static_cast<int8_t>(lightCount_ > 127 ? 127 : lightCount_);
        rebuildDriven();   // the colour-light set changed → re-derive the filtered driven subset
    }

    // --- Learn the bridge's Rooms (GET /api/<key>/groups). Same dynamic grow-and-retry read as
    // fetchLights — the /groups body grows with the room+zone count, so size the heap buffer up
    // until the response parses whole. fetchGroups runs at 1 Hz, off the render loop, after
    // fetchLights (gated by sawGroups_), so this alloc/refetch is never hot-path.
    void fetchGroups() {
        char host[16]; bridgeStr(host);
        char path[80]; std::snprintf(path, sizeof(path), "/api/%s/groups", appKey);
        for (size_t cap = kLightsBufInitial; cap <= kLightsBufMax; cap *= 2) {
            char* buf = static_cast<char*>(platform::alloc(cap));
            if (!buf) return;
            const int st = platform::httpRequest("GET", host, 80, path, "", kSlowTimeoutMs, buf, cap);
            if (st != 200) { platform::free(buf); return; }
            const bool truncated = !bodyLooksComplete(buf) && (cap < kLightsBufMax);
            if (!truncated) {
                parseGroups(buf);
                rebuildControls();   // the room dropdown options changed → re-bind for the UI
                platform::free(buf);
                return;
            }
            platform::free(buf);
        }
    }

    // Extract the Rooms from a /groups JSON body: {"1":{"name":"Living","lights":["3","5"],
    // "type":"Room",…},…}. Keep only type=="Room" (drop Zones, LightGroups, Entertainment); for
    // each, store its name and the light ids its "lights" array references. Same lightweight
    // forward scan as parseLights (the response exceeds the recursive reader's node arena): spot
    // each top-level id key, then read the object span up to the next id key.
    void parseGroups(const char* resp) {
        ensureNameBuffers();
        roomCount_ = 0;
        const char* p = resp;
        int pendingId = 0;
        const char* pendingStart = nullptr;
        auto commit = [&](const char* objEnd) {
            if (pendingId > 0 && pendingStart && roomCount_ < kMaxRooms && roomNames_
                && containsKey(pendingStart, objEnd, "\"type\":\"Room\"")) {
                char* name = roomNameAt(roomCount_);
                parseStringIn(pendingStart, objEnd, "name", name, kNameLen);
                if (!name[0]) std::snprintf(name, kNameLen, "%d", pendingId);
                roomMask_[roomCount_] = roomMaskFor(pendingStart, objEnd);
                roomCount_++;
            }
        };
        while (true) {
            const char* q = std::strchr(p, '"');
            if (!q) break;
            int id = std::atoi(q + 1);
            const char* close = std::strchr(q + 1, '"');
            if (id > 0 && close && close[1] == ':') {
                commit(q);                                  // the PREVIOUS group's object ends here
                pendingId = id;
                pendingStart = close + 1;
            }
            p = close ? close + 1 : q + 1;
        }
        commit(resp + std::strlen(resp));                   // the last group runs to the end
        sawGroups_ = true;
    }

    // Resolve a Room's "lights":["3","5",…] array (within [begin, end)) to a colour-light
    // membership bitmask: for each listed bridge id, set bit i if it equals a kept colour light
    // hueId_[i]. Ids the Room lists that aren't colour-capable (a white bulb, a plug) simply don't
    // match and are dropped. Scans from the "lights" key to the array's ']' so a later array
    // (e.g. a Zone's "lights" in a wider scan) can't bleed in.
    uint32_t roomMaskFor(const char* begin, const char* end) const {
        const char* s = begin;
        const size_t kl = std::strlen("\"lights\":[");
        for (; s + kl <= end; s++) if (std::strncmp(s, "\"lights\":[", kl) == 0) { s += kl; break; }
        uint32_t mask = 0;
        for (const char* q = s; q < end && *q != ']'; ) {
            if (*q == '"') {
                const int id = std::atoi(q + 1);
                for (uint8_t i = 0; i < lightCount_; i++)        // map the id to its colour-light bit
                    if (hueId_[i] == id) { mask |= (1u << i); break; }
                const char* c = std::strchr(q + 1, '"');         // skip to the value's closing quote
                if (!c || c >= end) break;
                q = c + 1;
            } else q++;
        }
        return mask;
    }

    // The colour-light array-indices (into hueId_ / lightName_) that the CURRENT room selection
    // exposes: room_==0 ("All") → every colour light, in order; else only the colour lights whose
    // id appears in that Room's member list. Writes up to kMaxLights indices into `out`, returns
    // the count. The single source of truth both the light-dropdown options and the driven set
    // derive from, so the dropdown and the driven subset can never disagree.
    uint8_t roomColourLights(uint8_t* out) const {
        uint8_t n = 0;
        if (room_ == 0 || room_ > roomCount_) {              // "All" (or a stale index) → every colour light
            for (uint8_t i = 0; i < lightCount_; i++) out[n++] = i;
            return n;
        }
        const uint32_t mask = roomMask_[room_ - 1];
        for (uint8_t i = 0; i < lightCount_; i++)            // keep colour lights in this Room's bitmask
            if (mask & (1u << i)) out[n++] = i;
        return n;
    }

    // Rebuild the room dropdown options: {"All", room0, room1, …}, pointing into roomName_.
    void buildRoomOptions() {
        roomOptions_[0] = "All";
        uint8_t n = 1;
        for (uint8_t i = 0; i < roomCount_ && n <= kMaxRooms; i++) roomOptions_[n++] = roomNameAt(i);
        roomOptionCount_ = n;
    }

    // Rebuild the light dropdown options: {"All", <names of the current room's colour lights>},
    // pointing into lightName_. The option count tracks the current room, so the light index
    // selects within that narrowed list (index 0 = "All", index k = the k-th listed light).
    void buildLightOptions() {
        lightOptions_[0] = "All";
        uint8_t idx[kMaxLights];
        const uint8_t m = roomColourLights(idx);
        uint8_t n = 1;
        for (uint8_t i = 0; i < m && n <= kMaxLights; i++) lightOptions_[n++] = lightNameAt(idx[i]);
        lightOptionCount_ = n;
    }

    // Derive drivenIdx_ from the current room+light filter — the subset pushOneChangedLight walks.
    //   room=All & light=All → every colour light (the original behaviour, unchanged).
    //   room=X               → that room's colour lights.
    //   light=Y              → just that one light (the Y-th of the current room's list).
    void rebuildDriven() {
        drivenLightCount_ = 0;
        uint8_t idx[kMaxLights];
        const uint8_t m = roomColourLights(idx);
        if (light_ == 0 || light_ > m) {                     // "All" within the (possibly room-narrowed) set
            for (uint8_t i = 0; i < m; i++) drivenIdx_[drivenLightCount_++] = idx[i];
        } else {                                             // a single light: the (light_-1)-th listed one
            drivenIdx_[drivenLightCount_++] = idx[light_ - 1];
        }
        if (pushCursor_ >= drivenLightCount_) pushCursor_ = 0;
    }

    // Push AT MOST ONE changed light per call (the loop() gate already limited the rate). The
    // round-robin cursor walks every light over successive calls, so each gets its turn; we
    // advance the cursor whether or not this light changed, scanning at most one full lap so an
    // all-unchanged frame costs no PUT and returns fast (no blocking I/O on the render loop).
    void pushOneChangedLight() {
        if (!sourceBuffer_ || !sourceBuffer_->data()) return;
        nrOfLightsType winStart, winLen;
        windowSlice(sourceBuffer_->count(), winStart, winLen);
        const uint8_t cpl = sourceBuffer_->channelsPerLight();
        if (cpl < 3) return;
        const uint8_t* base = sourceBuffer_->data();
        // Walk the FILTERED driven set (drivenIdx_), not every colour light: room=All & light=All
        // makes it the full colour-light set (unchanged behaviour), a room/light pick narrows it.
        const uint8_t n = drivenLightCount_ < winLen ? drivenLightCount_ : static_cast<uint8_t>(winLen);
        if (n == 0) return;
        drivenCount_ = n;   // the round-robin size — drives the Hue fade time (transitionDeciseconds)

        for (uint8_t step = 0; step < n; step++) {
            const uint8_t i = (pushCursor_ + step) % n;        // position within the driven window
            const uint8_t li = drivenIdx_[i];                  // the colour-light array index it maps to
            const uint8_t* px = base + static_cast<size_t>(winStart + i) * cpl;
            // Apply the shared Correction (brightness LUT + channel order) so the global
            // brightness slider and a swapped colour order reach Hue too — same as the physical
            // drivers. apply() writes outChannels bytes; we read the first three (RGB) for HSV.
            uint8_t rgb[4] = { px[0], px[1], px[2], 0 };
            if (correction_) correction_->apply(px, rgb);
            char body[80];
            if (diffAndFormat(li, rgb[0], rgb[1], rgb[2], body, sizeof(body))) {
                char host[16]; bridgeStr(host);
                char path[96];
                std::snprintf(path, sizeof(path), "/api/%s/lights/%u/state", appKey, hueId_[li]);
                const int st = platform::httpRequest("PUT", host, 80, path, body, kHttpTimeoutMs, nullptr, 0);
                // Mark the light sent only on a successful PUT — on a failure/timeout it stays
                // eligible so the next lap retries it instead of skipping it as "already sent".
                if (st == 200) {
                    lastRgb_[li][0] = rgb[0]; lastRgb_[li][1] = rgb[1]; lastRgb_[li][2] = rgb[2];
                    sent_[li] = true;
                }
                pushCursor_ = static_cast<uint8_t>((i + 1) % n);   // resume after this one next time
                return;                                            // ONE PUT attempt — done
            }
        }
        // No light changed this lap — nothing to send. Cursor stays put.
    }

    // The changed-only diff + the Hue state body. Returns true (and fills `out`) when light
    // `idx`'s RGB differs from the last push (or was never sent). Every driven light is colour-
    // capable (parseLights keeps only those), so the body carries the full colour: on/off, plus
    // bri (value) + hue + sat from a textbook RGB→HSV — so a colour effect actually animates.
    // "transitiontime" is the bridge's built-in fade — the smoothing knob. Set to roughly the
    // per-light update interval (a light updates every kPutIntervalMs × lightCount), so the bulb
    // glides from its current colour to the next instead of snapping. The bridge's default is
    // 400 ms (too long for our cadence — it smears and looks frozen); we compute a value matched
    // to the actual rate so transitions are smooth but keep up. transitiontime is in deciseconds
    // (×100 ms). The Hue standard API tops out ~10 cmd/s — true real-time needs the Entertainment
    // API; this is smooth ambient colour, the standard API's sweet spot.
    bool diffAndFormat(uint8_t idx, uint8_t r, uint8_t g, uint8_t b, char* out, size_t cap) {
        if (idx >= kMaxLights) return false;
        if (sent_[idx] && lastRgb_[idx][0] == r && lastRgb_[idx][1] == g && lastRgb_[idx][2] == b)
            return false;   // unchanged — skip
        const uint8_t tt = transitionDeciseconds();
        if ((r | g | b) == 0) { std::snprintf(out, cap, "{\"on\":false,\"transitiontime\":%u}", tt); return true; }
        uint16_t hue; uint8_t sat, val;
        rgbToHsv(r, g, b, hue, sat, val);
        std::snprintf(out, cap, "{\"on\":true,\"bri\":%u,\"hue\":%u,\"sat\":%u,\"transitiontime\":%u}",
                      val, hue, sat, tt);
        return true;
    }

    // Fade time matched to how often THIS light is refreshed: with n lights round-robined one
    // per kPutIntervalMs, each light's turn comes every (n × kPutIntervalMs) ms. Convert to
    // deciseconds and clamp to ≥1 (0 = snap) so the fade lasts about until the next update —
    // continuous glide, no visible steps.
    uint8_t transitionDeciseconds() const {
        // Use the count actually driven this pass (n = min(lightCount_, window)), not the full
        // discovered lightCount_ — a partial window refreshes each of its lights sooner, so a
        // lightCount_-based fade would overshoot and lag. drivenCount_ is set by pushOneChangedLight.
        const uint8_t driven = drivenCount_ ? drivenCount_ : (lightCount_ ? lightCount_ : 1);
        const uint32_t intervalMs = static_cast<uint32_t>(driven) * kPutIntervalMs;
        const uint32_t ds = intervalMs / 100;
        return static_cast<uint8_t>(ds < 1 ? 1 : (ds > 30 ? 30 : ds));
    }

    // Textbook RGB→HSV mapped to Hue's ranges: hue 0..65535 (Hue's 16-bit wheel), sat 0..254,
    // val(=bri) 0..254. Integer math, no float — the standard max/min/chroma formulation.
    static void rgbToHsv(uint8_t r, uint8_t g, uint8_t b, uint16_t& hueOut, uint8_t& satOut, uint8_t& valOut) {
        const uint8_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
        const uint8_t mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        const uint8_t chroma = mx - mn;
        valOut = static_cast<uint8_t>(mx > 254 ? 254 : mx);                 // value → bri
        satOut = mx == 0 ? 0 : static_cast<uint8_t>((chroma * 254u) / mx);  // saturation
        if (chroma == 0) { hueOut = 0; return; }                           // grey → hue irrelevant
        // Hue in sixths of the wheel, scaled to 0..65535. h6 is the position within [0,6).
        int32_t h6;  // numerator over chroma, in units where a full sixth = chroma*... see below
        if (mx == r)      h6 = ((g - b) * 65535) / (6 * chroma) + (g < b ? 65535 : 0);
        else if (mx == g) h6 = ((b - r) * 65535) / (6 * chroma) + 65535 / 3;
        else              h6 = ((r - g) * 65535) / (6 * chroma) + (65535 * 2) / 3;
        if (h6 < 0) h6 += 65535;
        hueOut = static_cast<uint16_t>(h6 % 65536);
    }
};

}  // namespace mm
