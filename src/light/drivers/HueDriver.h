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
        addWindowControls();                                    // start / count — its slice of the buffer
        controls_.addReadOnly("hueStatus", statusBuf_, sizeof(statusBuf_));
        controls_.addReadOnlyInt("colourLights", colourCount_, "lights"); // size a layout to this
        refreshStatus();
    }

    void setSourceBuffer(Buffer* buf) override { sourceBuffer_ = buf; }

    // The shared output Correction (global brightness LUT + channel order), same as the physical
    // LED / network drivers — so the brightness slider and a swapped colour order reach the Hue
    // lights too. Applied per pixel before RGB→HSV; the RGBW/white part is irrelevant here (Hue
    // takes hue/sat), we use the RGB result.
    void setCorrection(const Correction* c) override { correction_ = c; }

    // A control click. "pair" starts the link-button pairing poll.
    void onUpdate(const char* controlName) override {
        if (controlName && std::strcmp(controlName, "pair") == 0) {
            pairTicksLeft_ = kPairWindowTicks;   // begin: poll the bridge for ~30 s on loop1s
            std::snprintf(statusBuf_, sizeof(statusBuf_), "pairing: press the bridge button");
            setStatus(statusBuf_);
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
        if (lightCount_ == 0) { fetchLights(); DriverBase::loop1s(); return; }
        if (++reportTick_ >= kReportEverySec) { reportTick_ = 0; reportBridge(); }
        DriverBase::loop1s();
    }

    void teardown() override {
        pairTicksLeft_ = 0;
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
    void parseLightsForTest(const char* json) { parseLights(json); }
    uint8_t lightCountForTest() const { return lightCount_; }    // kept colour+reachable lights
    uint16_t hueIdForTest(uint8_t i) const { return i < kMaxLights ? hueId_[i] : 0; }
    int8_t colourCountForTest() const { return colourCount_; }

    // Test seam for the RGB→HSV mapping (no bridge needed).
    static void rgbToHsvForTest(uint8_t r, uint8_t g, uint8_t b, uint16_t& h, uint8_t& s, uint8_t& v) {
        rgbToHsv(r, g, b, h, s, v);
    }

private:
    static constexpr uint8_t kMaxLights = 32;        // a LAN's worth of Hue bulbs; bounded, no heap
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
    uint8_t  pushCursor_ = 0;                         // round-robin position across the lights
    uint32_t lastPutMs_ = 0;                          // millis() of the last PUT — the loop() rate gate
    int      pairTicksLeft_ = 0;
    uint16_t reportTick_ = 0;                        // counts loop1s ticks toward kReportEverySec
    char     statusBuf_[40] = "unpaired";
    // Off-hot-path scratch for the /lights GET response (a full home is ~8 KB). A member, not a
    // loop1s stack frame — read once per fetchLights, not per render tick.
    char     lightsBuf_[8192] = {};

    bool haveBridge() const { return bridgeIp[0] || bridgeIp[1] || bridgeIp[2] || bridgeIp[3]; }

    // Does the JSON span [begin, end) contain `key` (e.g. "\"hue\"") — used to read a light's
    // capabilities off its state block (a colour light has "hue"; the bridge omits it otherwise).
    static bool containsKey(const char* begin, const char* end, const char* key) {
        const size_t kl = std::strlen(key);
        for (const char* s = begin; s + kl <= end; s++)
            if (std::strncmp(s, key, kl) == 0) return true;
        return false;
    }

    void bridgeStr(char out[16]) const {
        std::snprintf(out, 16, "%u.%u.%u.%u", bridgeIp[0], bridgeIp[1], bridgeIp[2], bridgeIp[3]);
    }

    void refreshStatus() {
        if (!appKey[0]) std::snprintf(statusBuf_, sizeof(statusBuf_), "unpaired");
        else if (lightCount_) std::snprintf(statusBuf_, sizeof(statusBuf_), "paired, %u lights", lightCount_);
        else std::snprintf(statusBuf_, sizeof(statusBuf_), "paired");
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
                lightCount_ = 0;                  // re-fetch the light list with the new key
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

    // --- Learn the bridge's light ids (window index → hue id, in id order).
    void fetchLights() {
        char host[16]; bridgeStr(host);
        char path[80]; std::snprintf(path, sizeof(path), "/api/%s/lights", appKey);
        // The /lights body for a real bridge runs several KB (a full home is ~8 KB for ~10
        // lights) — too big for a loop1s stack frame, so the read buffer is a driver member
        // (allocated once with the object, off the hot path). httpRequest reads straight into it.
        int st = platform::httpRequest("GET", host, 80, path, "", kSlowTimeoutMs,
                                       lightsBuf_, sizeof(lightsBuf_));
        if (st != 200) return;
        parseLights(lightsBuf_);
        refreshStatus();
        reportBridge();
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
        lightCount_ = 0;
        const char* p = resp;
        int pendingId = 0;               // a light id seen, not yet committed (need its span first)
        const char* pendingStart = nullptr;
        auto commit = [&](const char* objEnd) {
            if (pendingId > 0 && pendingStart && lightCount_ < kMaxLights
                && containsKey(pendingStart, objEnd, "\"hue\"")
                && containsKey(pendingStart, objEnd, "\"reachable\":true")) {
                hueId_[lightCount_++] = static_cast<uint16_t>(pendingId);
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
        const uint8_t n = lightCount_ < winLen ? lightCount_ : static_cast<uint8_t>(winLen);
        if (n == 0) return;

        for (uint8_t step = 0; step < n; step++) {
            const uint8_t i = (pushCursor_ + step) % n;
            const uint8_t* px = base + static_cast<size_t>(winStart + i) * cpl;
            // Apply the shared Correction (brightness LUT + channel order) so the global
            // brightness slider and a swapped colour order reach Hue too — same as the physical
            // drivers. apply() writes outChannels bytes; we read the first three (RGB) for HSV.
            uint8_t rgb[4] = { px[0], px[1], px[2], 0 };
            if (correction_) correction_->apply(px, rgb);
            char body[80];
            if (diffAndFormat(i, rgb[0], rgb[1], rgb[2], body, sizeof(body))) {
                char host[16]; bridgeStr(host);
                char path[96];
                std::snprintf(path, sizeof(path), "/api/%s/lights/%u/state", appKey, hueId_[i]);
                platform::httpRequest("PUT", host, 80, path, body, kHttpTimeoutMs, nullptr, 0);
                lastRgb_[i][0] = rgb[0]; lastRgb_[i][1] = rgb[1]; lastRgb_[i][2] = rgb[2];
                sent_[i] = true;
                pushCursor_ = static_cast<uint8_t>((i + 1) % n);   // resume after this one next time
                return;                                            // ONE PUT — done
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
        const uint32_t intervalMs = static_cast<uint32_t>(lightCount_ ? lightCount_ : 1) * kPutIntervalMs;
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
