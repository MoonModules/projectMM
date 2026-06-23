#pragma once

#include "core/MoonModule.h"
#include "core/Control.h"
#include "core/JsonSink.h"
#include "core/JsonUtil.h"         // recursive reader — restoreList parses the persisted array
#include "core/Sort.h"             // mm::insertionSort — generic bounded sort (core); we supply the comparator
#include "core/DeviceIdentify.h"   // DevType, classifyDevice, extractDeviceName (pure, unit-tested)
#include "core/FilesystemModule.h" // FilesystemModule::noteDirty — persist on sweep / age-out change
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

// Discovers other devices on the LAN, identifies what each is, and presents them
// as a browsable list. Core + domain-neutral: it finds "a projectMM / a WLED / a
// generic HTTP device", and light modules (Art-Net sync, future SuperSync) consume
// the list rather than living here. Submodule of NetworkModule — discovery depends
// on the network being up. See docs/moonmodules/core/DevicesModule.md.
//
// v1: a throttled subnet sweep (a few IPs per loop1s tick, never blocking the
// render loop) HTTP-probes each host and classifies the response; results render
// in the generic List control (this module is its ListSource). Read-only.
class DevicesModule : public MoonModule, public ListSource {
public:
    // Wire this device's own name (deviceName) before setup so the self row in the
    // list matches the status page / router / mDNS. Borrowed pointer — caller owns
    // stable storage (e.g. SystemModule::deviceName()).
    void setSelfName(const char* name) { selfName_ = name; }

    // ListSource — rows are produced straight from devices_ (no copy, no alloc).
    uint8_t listRowCount() const override { return deviceCount_; }

    void writeListRow(JsonSink& sink, uint8_t row) const override {
        if (row >= deviceCount_) { sink.append("{}"); return; }
        const Device& d = devices_[row];
        char ip[16];
        formatDottedQuad(ip, d.ip);
        sink.append("{\"name\":");
        sink.writeJsonString(d.name[0] ? d.name : ip);
        sink.appendf(",\"ip\":\"%s\",\"type\":\"%s\"", ip, devTypeStr(d.type));
        if (d.self) sink.append(",\"self\":true");
        sink.append("}");
    }

    void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
        if (row >= deviceCount_) { sink.append("{}"); return; }
        const Device& d = devices_[row];
        char ip[16];
        formatDottedQuad(ip, d.ip);
        sink.append("{\"name\":");
        sink.writeJsonString(d.name[0] ? d.name : ip);
        sink.appendf(",\"ip\":\"%s\",\"url\":\"http://%s/\",\"type\":\"%s\"",
                     ip, ip, devTypeStr(d.type));
        writeSpeaks(sink, d.speaks);
        writeVia(sink, d.via);   // how it was found (mdns / scan / udp) — for the UI badge
        if (d.self) {
            sink.append(",\"self\":true");   // self is always "now" — no meaningful age
        } else if (d.cached) {
            // Restored from persistence, not re-confirmed live this session — `ageSec`
            // would be a fake "now" (the boot stamp), so emit `cached` instead. The UI
            // shows "last seen: cached"; once a strategy re-sees it, cached clears and a
            // real ageSec appears.
            sink.append(",\"cached\":true");
        } else {
            // Seconds since this device was last seen by any strategy. Computed here
            // (device-side) so the UI gets one finished number, not a raw boot-relative
            // clock it would have to reconcile; the same `now - lastSeenMs` the age-out
            // uses, in seconds. Snapshot at state-push time. Wrap-safe (unsigned).
            uint32_t ageSec = (platform::millis() - d.lastSeenMs) / 1000u;
            sink.appendf(",\"ageSec\":%u", static_cast<unsigned>(ageSec));
        }
        sink.append("}");
    }

    // ListSource restore (persistence load): parse the saved `devices` array with the
    // recursive mm::json reader and rebuild devices_, so the last-known list shows on
    // boot before any scan. Tolerant of a malformed/over-large file (parse fails →
    // false → empty list). Self is dropped (re-added live via upsertSelf with the
    // current IP); missed=0 so a device that's truly gone ages out after the first
    // live sweep.
    bool restoreList(const char* json, const char* key) override {
        deviceCount_ = 0;
        // Core does the parse / array-navigate / iterate / malformed-safety
        // (forEachListElement); this body is just "fill one device from this object".
        // Capture its result — we still need to sort before returning, so we can't
        // `return` it inline (that skipped sortByName, leaving the cache unsorted).
        const bool ok = mm::json::forEachListElement(json, key,
            [&](const mm::json::JsonDoc& doc, const mm::json::JsonNode* el) {
                if (deviceCount_ >= kMaxDevices) return;
                if (mm::json::readBool(mm::json::member(doc, el, "self"))) return;  // skip persisted self
                char ipStr[16] = {}, name[24] = {}, typeStr[12] = {};
                mm::json::readString(mm::json::member(doc, el, "ip"), ipStr, sizeof(ipStr));
                mm::json::readString(mm::json::member(doc, el, "name"), name, sizeof(name));
                mm::json::readString(mm::json::member(doc, el, "type"), typeStr, sizeof(typeStr));
                uint8_t octets[4];
                if (!parseDottedQuad(ipStr, octets)) return;
                Device& d = devices_[deviceCount_++];
                std::memcpy(d.ip, octets, 4);
                std::snprintf(d.name, sizeof(d.name), "%s", name);
                d.type = (std::strcmp(typeStr, "projectMM") == 0) ? DevType::ProjectMM
                       : (std::strcmp(typeStr, "WLED") == 0)      ? DevType::Wled
                                                                  : DevType::Generic;
                d.self = false;
                d.speaks = ProtoHttp;
                d.via = 0;        // no live sighting yet — via fills in when a strategy re-sees it
                d.cached = true;  // restored, not re-confirmed live → UI shows "cached", not a time
                // Stamp "now" so the cached entry gets a full kStaleMs grace window to
                // re-announce before age-out drops it (a still-alive but slow device).
                d.lastSeenMs = platform::millis();
            });
        sortByName();   // cached list shows alphabetically too, before the first sweep
        return ok;      // false on a malformed/missing file (list left empty)
    }

    void onBuildControls() override {
        MoonModule::onBuildControls();
        // `scan` is a momentary ACTION (rescan now), not an on/off state — a Button,
        // not a Bool toggle (a toggle next to the "scanning…" status reads as two
        // unrelated states). onUpdate runs the rescan.
        controls_.addButton("scan");
        // No "status" control — sweep state goes through MoonModule::setStatus(), the
        // standard status channel the UI renders generically (mod.status/severity).
        // Sweep progress (host 0..254, plain count not KB). Always present: the WS
        // state push patches values but not structure, so a show-only-while-scanning
        // hide flag wouldn't update live. At rest the value is 0 (empty bar); pressing
        // `scan` mid-sweep just restarts the sweep (harmless), so no need to gate it.
        controls_.addProgress("progress", scanProgress_, 254, /*bytes=*/false);
        controls_.addList("devices", *this);   // this module is the ListSource
    }

    void onUpdate(const char* controlName) override {
        if (std::strcmp(controlName, "scan") == 0) restartScan();
    }

    void setup() override {
        MoonModule::setup();
        // The last-known device list is restored automatically before setup() by the
        // persistence overlay (the `devices` List control is persistable and
        // round-trips as JSON — restoreList rebuilt devices_). So the UI shows it
        // INSTANTLY on boot — no waiting for a fresh sweep (the win for slow-to-
        // discover devices like a PC instance or generic host that mDNS can't find).
        if (deviceCount_) {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "%u device%s (cached)",
                          deviceCount_, deviceCount_ == 1 ? "" : "s");
        }
        setStatus(statusBuf_);   // "idle", or the cached-count summary
        // Don't scan here — the network isn't up yet (DHCP lands a few seconds after
        // boot). loop1s() kicks the ONE boot sweep once a local IP appears.
    }

    // The sweep advances one IP per tick. It runs on the render task and each probe
    // BLOCKS up to kProbeTimeoutMs on a dead host — a hot-path stall that would
    // flicker the LEDs. So the sweep runs ONCE at boot (LEDs not yet critical) and
    // otherwise only on an explicit `scan` press; there is NO periodic background
    // scan. Moving the blocking probe to its own FreeRTOS task is the enabler for
    // safe periodic scanning (and a UDP presence beacon) — see backlog.
    void loop1s() override {
        MoonModule::loop1s();
        if (hostCursor_ >= 0) {
            stepScan();
        } else if (!sweptOnce_) {
            // One-time boot sweep, started as soon as the network is up.
            uint8_t local[4] = {};
            localIp(local);
            if (local[0] || local[1] || local[2] || local[3]) restartScan();
        }
        // mDNS browse runs EVERY tick, independent of the HTTP sweep: it's async and
        // non-blocking (a cheap poll, no per-host timeout), so it's safe on loop1s
        // where the blocking HTTP probe is not. It catches devices that advertise a
        // service (WLED, projectMM, generic `_http._tcp`) as they come and go, without
        // a subnet sweep — the standard, push-style discovery the architecture calls for.
        stepMdns();
        // Age out here, not at sweep-end: discovery now arrives on several cadences
        // (a minutes-long HTTP sweep, a seconds-long mDNS lap, a future async UDP
        // beacon), so freshness is a per-device timestamp and the drop is a simple
        // "unseen too long" check every tick — independent of any one strategy's cycle.
        ageOut();
    }

    ModuleRole role() const override { return ModuleRole::Generic; }

private:
    // DevType / classifyDevice / extractDeviceName live in DeviceIdentify.h (pure +
    // unit-tested). devTypeStr() there replaces the former local typeStr().

    // Protocols a device is known to speak, as a bitmask. v1 discovery only proves
    // HTTP (the scan probes it), so that's all that gets set today — but the field
    // exists so additional discovery strategies (mDNS browse, UDP/ArtPoll/DDP/OSC,
    // RTP-MIDI; see DevicesModule.md "Discovery is per-protocol") fill in more bits
    // without reshaping the device record or the wire format. A consumer (Art-Net
    // sync, fleet OTA) reads `speaks` to know how it can talk to a device.
    enum Proto : uint8_t {
        ProtoHttp   = 1 << 0,   // an HTTP API (REST) — the only one v1 discovers
        ProtoArtnet = 1 << 1,   // Art-Net / sACN (future: ArtPoll discovery)
        ProtoDdp    = 1 << 2,   // DDP (future)
        // … mDNS-advertised services, OSC, RTP-MIDI, etc. as strategies are added.
    };

    // How a device was discovered, as a bitmask — a device can be found by more than
    // one strategy at once (mDNS browse AND the HTTP sweep both see a projectMM peer),
    // so this is OR-ed like `speaks`, not a single last-writer-wins value. The detail
    // panel renders it so "what did mDNS find vs the scan" is visible. UDP (a future
    // presence beacon) is the next bit; it arrives on its own async cadence, which is
    // exactly why discovery freshness is a per-device timestamp, not a per-sweep
    // counter (no single sweep boundary to hang a counter off — see lastSeenMs).
    enum Via : uint8_t {
        ViaScan = 1 << 0,   // answered the HTTP subnet sweep
        ViaMdns = 1 << 1,   // advertised a browsed mDNS service
        ViaUdp  = 1 << 2,   // announced via a UDP presence beacon (future)
    };

    struct Device {
        uint8_t  ip[4] = {};
        char     name[24] = {};
        DevType  type = DevType::Generic;
        uint8_t  speaks = 0;     // Proto bitmask — protocols this device is known to speak
        uint8_t  via = 0;        // Via bitmask — which strategies have discovered it
        bool     self = false;
        bool     cached = false; // restored from persistence, not yet re-seen LIVE this
                                 // session (via is still empty, lastSeenMs is the boot
                                 // stamp, not a real sighting). Cleared on the first live
                                 // sighting; until then the UI shows "cached", not a time.
        uint32_t lastSeenMs = 0; // platform::millis() at the most recent sighting (any
                                 // strategy). Age-out drops a non-self device unseen for
                                 // kStaleMs — strategy-agnostic, so HTTP/mDNS/UDP, each on
                                 // its own cadence, all just stamp "now" when they see it.
    };

    // Append a `speaks` JSON array (e.g. ["http"]) for a device's protocol bitmask.
    static void writeSpeaks(JsonSink& sink, uint8_t speaks) {
        sink.append(",\"speaks\":[");
        bool first = true;
        auto emit = [&](uint8_t bit, const char* tag) {
            if (!(speaks & bit)) return;
            if (!first) sink.append(",");
            sink.appendf("\"%s\"", tag);
            first = false;
        };
        emit(ProtoHttp, "http");
        emit(ProtoArtnet, "artnet");
        emit(ProtoDdp, "ddp");
        sink.append("]");
    }

    // Append a `via` JSON array (e.g. ["scan","mdns"]) for a device's discovery bitmask
    // — how the device was found, so the UI can show mDNS-found vs scan-found at a glance.
    static void writeVia(JsonSink& sink, uint8_t via) {
        sink.append(",\"via\":[");
        bool first = true;
        auto emit = [&](uint8_t bit, const char* tag) {
            if (!(via & bit)) return;
            if (!first) sink.append(",");
            sink.appendf("\"%s\"", tag);
            first = false;
        };
        emit(ViaScan, "scan");
        emit(ViaMdns, "mdns");
        emit(ViaUdp, "udp");
        sink.append("]");
    }

    static constexpr uint8_t  kMaxDevices    = 32;  // a LAN's worth; bounded, no heap
    // One IP per tick: a probe blocks up to kProbeTimeoutMs on a dead host, and
    // loop1s must not stall the render loop. 1 IP/tick → a /24 sweep takes ~254 s
    // worst case (all-dead subnet), but each tick blocks at most one timeout. The
    // probe short-circuits after the FIRST GET times out (a dead host answers no
    // URL), so a sparse subnet costs ~1×timeout per empty IP, not 3×.
    static constexpr uint8_t  kProbesPerTick = 1;
    // Short timeout: this GET blocks the scheduler thread (and thus one render tick) on a dead host,
    // so it must stay small or the boot sweep stutters animation once a second for the ~4 min the
    // /24 takes. A live host on a LAN answers in a few ms; 30 ms covers a slow responder while
    // keeping the worst-case per-tick stall to ~30 ms. (The real fix — running the probe on its own
    // task so it never touches the render thread — is backlogged; this just bounds the symptom.)
    static constexpr uint32_t kProbeTimeoutMs = 30;
    // Drop a non-self device unseen by ANY strategy for this long. 24 h is deliberately
    // generous: mDNS re-confirms its devices every few-second browse lap (cheap), but an
    // HTTP-scan-only device (a PC instance, a generic host) has no cheap recurring
    // refresh — the sweep is boot-once + manual, not periodic — so a short timeout would
    // wrongly drop a still-alive device and force a re-scan. A day-long window lets such
    // a device persist on its single sighting while a genuinely-departed device still
    // clears itself within a day. Each sighting (HTTP/mDNS/UDP) restamps lastSeenMs.
    static constexpr uint32_t kStaleMs       = 24u * 60u * 60u * 1000u;  // 24 hours
    Device  devices_[kMaxDevices];
    uint8_t deviceCount_ = 0;
    bool    sweptOnce_ = false;     // the one boot sweep has completed
    const char* selfName_ = nullptr;  // this device's name (wired via setSelfName)
    uint32_t scanProgress_ = 0;     // current host index 1..254 (0 = idle), for the Progress bar
    char    statusBuf_[40] = "idle";
    // Probe response buffer — a member, not a per-call stack local: /api/state's
    // deviceName can sit past 512 B on a multi-module device, so this needs ~1 KB,
    // too large for a stack frame in the scheduler task. One probe runs per tick, so
    // a single reused buffer suffices (no concurrency). Part of the module's fixed
    // footprint (~1 KB), allocated once.
    char    probeBuf_[1024];

    // Sweep cursor: hostLow_ walks 1..254 across the local /24. -1 = no scan running
    // (no network yet, or sweep finished). The subnet's first three octets come from
    // the local IP, captured at restartScan().
    int16_t hostCursor_ = -1;
    uint8_t subnet_[3] = {};   // first three octets of the /24 being swept

    // True when a control or first-run kicks off a fresh full sweep. Captures the
    // local IP (and so the subnet); marks every known device unseen-this-sweep.
    void restartScan() {
        uint8_t local[4] = {};
        localIp(local);
        if (local[0] == 0 && local[1] == 0 && local[2] == 0 && local[3] == 0) {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "no network");
            setStatus(statusBuf_, Severity::Warning);
            hostCursor_ = -1;
            scanProgress_ = 0;   // back to idle — no stale bar left showing
            return;
        }
        subnet_[0] = local[0]; subnet_[1] = local[1]; subnet_[2] = local[2];
        hostCursor_ = 1;       // sweep .1 .. .254
        scanProgress_ = 1;
        std::snprintf(statusBuf_, sizeof(statusBuf_), "scanning %u.%u.%u.0/24",
                      subnet_[0], subnet_[1], subnet_[2]);
        setStatus(statusBuf_);
        // Ensure self is in the list even before its own IP is probed.
        upsertSelf(local);
    }

    // Probe up to kProbesPerTick hosts this tick; advance the cursor. When the sweep
    // completes, age out devices not seen and go idle until the next restartScan.
    void stepScan() {
        if (hostCursor_ < 0) return;
        uint8_t local[4] = {};
        localIp(local);
        for (uint8_t i = 0; i < kProbesPerTick && hostCursor_ <= 254; i++, hostCursor_++) {
            uint8_t ip[4] = {subnet_[0], subnet_[1], subnet_[2],
                             static_cast<uint8_t>(hostCursor_)};
            // Don't probe our own IP: upsertSelf already gave it the right identity
            // (projectMM, deviceName), and an HTTP request to ourselves mid-tick can
            // race the server / loopback and misclassify us as generic. Just keep it
            // fresh so age-out doesn't drop it.
            if (ipEq(ip, local)) { if (Device* d = findByIp(ip)) { d->lastSeenMs = platform::millis(); d->cached = false; } continue; }
            probe(ip);
        }
        // Advance the progress bar to the current cursor (1..254) so it tracks the sweep.
        if (hostCursor_ <= 254) scanProgress_ = static_cast<uint32_t>(hostCursor_);
        if (hostCursor_ > 254) {
            // Sweep finished — reset the bar to 0 (idle), not left full at 254.
            scanProgress_ = 0;
            hostCursor_ = -1;
            sweptOnce_ = true;
            // Age-out is no longer tied to the sweep end (it runs every tick in loop1s,
            // off the per-device timestamp); the sweep just reports its result + persists.
            std::snprintf(statusBuf_, sizeof(statusBuf_), "%u device%s",
                          deviceCount_, deviceCount_ == 1 ? "" : "s");
            setStatus(statusBuf_);
            // Persist the fresh set so the next boot shows it instantly. The `devices`
            // List control is persistable — marking dirty arms the standard
            // FilesystemModule debounce, which serializes the List as JSON.
            markDirty();
            FilesystemModule::noteDirty();
        }
    }


    // mDNS service types browsed, in round-robin. `_http._tcp` catches projectMM (we
    // advertise it via mdnsInit) and any generic web device; `_wled._tcp` is WLED's
    // own service. The list is the discovery surface — add `_esphome._tcp`,
    // `_home-assistant._tcp`, etc. here as classification for them lands (the hit's
    // service type already maps to a DevType in mdnsTypeFor). No state reshuffle.
    struct MdnsService { const char* service; const char* proto; DevType type; };
    static constexpr MdnsService kMdnsServices[] = {
        { "_http", "_tcp", DevType::Generic },   // projectMM + generic web devices
        { "_wled", "_tcp", DevType::Wled    },
    };
    static constexpr uint8_t kMdnsServiceCount =
        sizeof(kMdnsServices) / sizeof(kMdnsServices[0]);
    // Per-tick mDNS query timeout. Small: this is a blocking call on loop1s, so it must
    // stay well under the 1 s tick budget (and it shares loop1s with the HTTP sweep). A
    // peer that doesn't answer within this window is caught on a later pass — discovery is
    // continuous (every tick cycles to the next service type), so a short timeout per call
    // mdnsBrowse is synchronous and blocks the FULL timeout (the IDF query waits the whole
    // window for late responders — it does not return early), and loop1s shares the tick
    // thread, so this time is charged to the tick. Keep the timeout SHORT AND browse only
    // every kMdnsEveryTicks-th tick: one ~20 ms hiccup every ~15 s is invisible for a
    // discovery feature (peers don't come and go faster than that), and FPS is untouched in
    // between. (The old async API polled cheaply every tick but raced the mDNS task's expiry
    // timer and crashed on a UI refresh; a bounded synchronous call holds no handle, so it
    // can't. The throttle is how we keep that safety without the per-tick block cost.)
    static constexpr uint32_t kMdnsBrowseMs = 20;    // shorter blocking window → smaller render hiccup
    static constexpr uint8_t kMdnsEveryTicks = 15;   // browse less often → the hiccup is rarer (~15 s)

    uint8_t mdnsIndex_ = 0;        // which service in kMdnsServices is browsed
    uint8_t mdnsTick_ = 0;         // throttle counter for the browse cadence

    // Browse one service type on the throttled cadence: query it (blocking, bounded), merge
    // hits via the static callback, advance to the next type. The cycle wraps kMdnsServices
    // forever, so new advertisers are picked up on later passes.
    void stepMdns() {
        if (++mdnsTick_ < kMdnsEveryTicks) return;
        mdnsTick_ = 0;
        const MdnsService& s = kMdnsServices[mdnsIndex_];
        platform::mdnsBrowse(s.service, s.proto, kMdnsBrowseMs, &DevicesModule::onMdnsHost, this);
        advanceMdns();
    }

    void advanceMdns() { mdnsIndex_ = (mdnsIndex_ + 1) % kMdnsServiceCount; }

    // platform::MdnsHostCb — a found host for the service type at mdnsIndex_. Trampoline
    // to the instance; `user` is `this` (set in mdnsBrowsePoll above).
    static void onMdnsHost(const platform::MdnsHost& host, void* user) {
        static_cast<DevicesModule*>(user)->mergeMdnsHost(host);
    }

    void mergeMdnsHost(const platform::MdnsHost& host) {
        if (host.ip[0] == 0 && host.ip[1] == 0 && host.ip[2] == 0 && host.ip[3] == 0)
            return;   // unresolved — nothing to key on
        // The browsed service type maps to a DevType (Generic for `_http`, Wled for
        // `_wled`). A host on the GENERIC `_http` service carrying our `mm=1` TXT marker
        // is a projectMM device — promote it, so an mDNS-only sighting classifies + names
        // it without waiting for the HTTP scan. The promotion is gated on the base type
        // being Generic: a definite service type (e.g. `_wled`) already says what the
        // host is, so the marker must not override it (defensive — a real WLED won't
        // carry `mm=1`, but a future service mustn't be silently relabelled projectMM).
        const DevType baseType = kMdnsServices[mdnsIndex_].type;
        DevType type = (host.isProjectMM && baseType == DevType::Generic)
                       ? DevType::ProjectMM : baseType;
        upsertMdns(host.ip, type, host.hostname);
    }

    void localIp(uint8_t out[4]) const {
        platform::ethGetIPv4(out);
        if (!out[0] && !out[1] && !out[2] && !out[3]) platform::wifiStaGetIPv4(out);
    }

    // HTTP-probe one IP and classify. Tries port 80 first (ESP32 devices, WLED,
    // generic web UIs); if nothing answers there, tries port 8080 (a projectMM
    // DESKTOP instance serves its API on 8080, not 80 — see main_desktop.cpp).
    // A live :80 host stops after :80, so the extra :8080 attempt only costs a
    // second timeout on otherwise-empty IPs, keeping the per-IP budget bounded.
    void probe(const uint8_t ip[4]) {
        if (probePort(ip, 80)) return;
        probePort(ip, 8080);
    }

    // Probe one ip:port. Returns true if a host answered (so the caller can stop).
    bool probePort(const uint8_t ip[4], uint16_t port) {
        char url[48], ipStr[16];
        formatDottedQuad(ipStr, ip);

        // First GET doubles as the liveness check: status 0 == no host answered
        // (timeout / connection refused). The response goes in probeBuf_, a member
        // (NOT a stack local): /api/state's deviceName can sit past 512 B on a
        // multi-module device, so the buffer must be ~1 KB — too large for this
        // call's stack frame in the scheduler task, so it lives in the module's
        // fixed footprint and is reused each probe (one probe per tick).
        std::snprintf(url, sizeof(url), "http://%s:%u/api/state", ipStr, port);
        int status = platform::httpGet(url, kProbeTimeoutMs, probeBuf_, sizeof(probeBuf_));
        if (status == 0) return false;   // nothing on this port
        // Only a 200 body is real /api/state — a 404/500 error page that happens to
        // contain "modules" must not be misread as a projectMM (the WLED branch below
        // already gates on 200). A non-200 still means the host is ALIVE, so fall
        // through to the WLED probe / generic classification.
        if (status == 200) {
            DevType t = classifyDevice(probeBuf_, nullptr);
            if (t == DevType::ProjectMM) { upsert(ip, t, probeBuf_); return true; }
        }
        DevType t = DevType::Generic;

        // Not a projectMM — try the WLED info endpoint on this port.
        std::snprintf(url, sizeof(url), "http://%s:%u/json/info", ipStr, port);
        if (platform::httpGet(url, kProbeTimeoutMs, probeBuf_, sizeof(probeBuf_)) == 200) {
            t = classifyDevice(nullptr, probeBuf_);
            if (t == DevType::Wled) { upsert(ip, t, probeBuf_); return true; }
        }
        // Live host, not projectMM/WLED → generic HTTP device.
        upsert(ip, DevType::Generic, nullptr);
        return true;
    }

    // Find-or-insert a device by IP; refresh its type/name and mark it seen.
    void upsert(const uint8_t ip[4], DevType type, const char* body) {
        uint8_t local[4] = {};
        localIp(local);
        const bool isSelf = ipEq(ip, local);
        Device* d = findByIp(ip);
        if (!d) {
            if (deviceCount_ >= kMaxDevices) return;   // bounded; silently cap
            d = &devices_[deviceCount_++];
            std::memcpy(d->ip, ip, 4);
        }
        d->type = type;
        d->self = isSelf;
        d->lastSeenMs = platform::millis();
        d->cached = false;        // a live sighting — no longer just a cached entry
        d->speaks |= ProtoHttp;   // found via the HTTP scan → it speaks HTTP
        d->via |= ViaScan;        // discovered by the HTTP subnet sweep
        extractDeviceName(type, body, d->name, sizeof(d->name));
        if (!d->name[0]) formatDottedQuad(d->name, ip);   // fall back to the IP
        sortByName();   // keep the list ordered AS devices arrive — not just at sweep end
    }

    // True when `name` is just the device's own IP as a dotted quad — i.e. a placeholder
    // a sighting fell back to because no real name was known yet. A later sighting with a
    // genuine name should overwrite it (see upsertMdns); a real name never matches its IP.
    static bool isIpPlaceholder(const char* name, const uint8_t ip[4]) {
        char ipStr[16];
        formatDottedQuad(ipStr, ip);
        return std::strcmp(name, ipStr) == 0;
    }

    // Merge an mDNS browse hit. Like upsert() but the identity is weaker: mDNS proves
    // the host advertises a service (so it speaks HTTP and is alive → missed=0), and
    // for `_wled._tcp` the type is certain, but `_http._tcp` only says "some web
    // device" — so a Generic hit must NOT downgrade a device the HTTP probe already
    // identified as projectMM/WLED. We only raise the type (Generic → known), never
    // lower it. The hostname becomes the display name only if we don't have a better
    // one yet (the HTTP probe's deviceName wins when present). self is preserved.
    void upsertMdns(const uint8_t ip[4], DevType type, const char* hostname) {
        uint8_t local[4] = {};
        localIp(local);
        const bool isSelf = ipEq(ip, local);
        Device* d = findByIp(ip);
        if (!d) {
            if (deviceCount_ >= kMaxDevices) return;
            d = &devices_[deviceCount_++];
            std::memcpy(d->ip, ip, 4);
            d->type = type;            // first sighting — take the mDNS type as-is
        } else if (type != DevType::Generic) {
            d->type = type;            // a definite type (WLED) refines an existing row
        }
        d->self |= isSelf;
        d->lastSeenMs = platform::millis();
        d->cached = false;             // a live sighting — no longer just a cached entry
        d->speaks |= ProtoHttp;        // advertised an HTTP service → speaks HTTP
        d->via |= ViaMdns;             // discovered by the mDNS browse
        // Take the mDNS name when we don't have a real one yet. "No real name" means
        // either empty OR a dotted-quad IP placeholder a prior sighting fell back to —
        // a genuine advertised name (the peer's deviceName) should replace that IP.
        // A name from the HTTP probe (a real deviceName) still wins: it's not an IP, so
        // the isIpPlaceholder check leaves it alone.
        if (hostname && hostname[0] && (!d->name[0] || isIpPlaceholder(d->name, ip)))
            std::snprintf(d->name, sizeof(d->name), "%s", hostname);
        if (!d->name[0]) formatDottedQuad(d->name, ip);
        sortByName();
    }

    // Guarantee this device is listed (marked self) even before its IP is swept.
    void upsertSelf(const uint8_t ip[4]) {
        Device* d = findByIp(ip);
        if (!d) {
            if (deviceCount_ >= kMaxDevices) return;
            d = &devices_[deviceCount_++];
            std::memcpy(d->ip, ip, 4);
        }
        // Refresh identity on BOTH paths: if this IP was first seen by a sweep/mDNS as
        // generic, learning it's us must promote it to projectMM, not leave the stale
        // type. We are a projectMM and we speak HTTP.
        d->type = DevType::ProjectMM;
        d->speaks |= ProtoHttp;
        d->self = true;
        d->cached = false;                    // self is live by definition, not cached
        d->lastSeenMs = platform::millis();   // self is always "now" → never ages out
        if (!d->name[0]) {
            // Show our own name (deviceName, wired via setSelfName) so the self row
            // matches the status page / router / mDNS. "this device" is the last
            // resort when no name was wired — same robustness contract as the rest.
            const char* n = (selfName_ && selfName_[0]) ? selfName_ : "this device";
            std::snprintf(d->name, sizeof(d->name), "%s", n);
        }
        sortByName();   // keep the list ordered (self slots in by name like any device)
    }


    Device* findByIp(const uint8_t ip[4]) {
        for (uint8_t i = 0; i < deviceCount_; i++)
            if (ipEq(devices_[i].ip, ip)) return &devices_[i];
        return nullptr;
    }

    static bool ipEq(const uint8_t a[4], const uint8_t b[4]) {
        return std::memcmp(a, b, 4) == 0;
    }

    // Drop non-self devices unseen by ANY strategy for longer than kStaleMs (a
    // powered-off / departed device). Runs every tick off the per-device timestamp,
    // so it's independent of any one strategy's cadence (HTTP sweep, mDNS lap, future
    // UDP beacon). Stable compaction — preserves the by-name order upsert maintains.
    // self never ages out (its timestamp is restamped to "now" on every sweep step).
    // `now - lastSeenMs` in unsigned arithmetic is wrap-safe: the millis() counter
    // wraps every ~49 days, but the elapsed interval (< kStaleMs) stays well below
    // 2^31, so the subtraction yields the true elapsed time across a wrap.
    void ageOut() {
        const uint32_t now = platform::millis();
        uint8_t w = 0;
        for (uint8_t r = 0; r < deviceCount_; r++) {
            Device& d = devices_[r];
            if (!d.self && (now - d.lastSeenMs) > kStaleMs) continue;   // drop, stale
            if (w != r) devices_[w] = d;
            w++;
        }
        if (w == deviceCount_) return;   // nothing dropped — common case, no churn
        deviceCount_ = w;
        std::snprintf(statusBuf_, sizeof(statusBuf_), "%u device%s",
                      deviceCount_, deviceCount_ == 1 ? "" : "s");
        setStatus(statusBuf_);
        // A drop changes the persisted set — save it so the cached list stays accurate.
        markDirty();
        FilesystemModule::noteDirty();
    }

    // Order the list by device name (case-insensitive). Core's insertionSort does the
    // work; we supply only the comparator — the domain stays a one-liner. Off the hot
    // path (sweep-end / boot-load), bounded (<= kMaxDevices). The compare is inline
    // (not strcasecmp/_stricmp, which differ across POSIX/Windows desktop).
    void sortByName() {
        mm::insertionSort(devices_, deviceCount_, [](const Device& a, const Device& b) {
            return ciLess(a.name, b.name);
        });
    }

    // a < b, ASCII case-insensitive.
    static bool ciLess(const char* a, const char* b) {
        for (; *a && *b; a++, b++) {
            int ca = lower(*a), cb = lower(*b);
            if (ca != cb) return ca < cb;
        }
        return lower(*a) < lower(*b);   // shorter string sorts first
    }
    static int lower(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : static_cast<unsigned char>(c); }
};

}  // namespace mm
