#pragma once

#include "core/MoonModule.h"
#include "core/Control.h"
#include "core/JsonSink.h"
#include "core/JsonUtil.h"         // recursive reader — restoreList parses the persisted array
#include "core/Sort.h"             // mm::insertionSort — generic bounded sort (core); we supply the comparator
#include "core/DeviceIdentify.h"   // DevType, devTypeStr (the device-kind enum + its labels)
#include "core/DevicePlugin.h"     // the interop plugin seam + the bundled plugins
#include "core/FilesystemModule.h" // FilesystemModule::noteDirty — persist on list change
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

// Discovers other devices on the LAN by mDNS and presents them as a browsable list.
// Core + domain-neutral: it finds "a projectMM / a WLED device" and light modules
// (Art-Net sync, future SuperSync, device groups) consume the list rather than living
// here. Submodule of NetworkModule — discovery depends on the network being up. See
// docs/moonmodules/core/DevicesModule.md.
//
// Discovery is the standard mDNS-SD pattern: each device ANNOUNCES its service
// (`_http._tcp`+`mm=1` for projectMM, `_wled._tcp` for WLED — see mdnsInit), and this
// module passively LISTENS via a non-blocking mDNS poll (platform::mdnsListenPoll),
// rotating through the service types its DevicePlugins claim. A plugin classifies each
// hit into a `Device`. This is fast (no subnet sweep), hot-path-safe (the poll never
// blocks), and extensible (a new ecosystem is one new plugin file). Reliable
// device-to-device *commands* ride REST; latency-critical sync rides UDP — never this
// listener, which carries only lossy-OK presence.
class DevicesModule : public MoonModule, public ListSource {
public:
    // Wire this device's own name (deviceName) before setup so the self row matches the
    // status page / router / mDNS. Borrowed pointer — caller owns stable storage (SystemModule).
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
        if (d.self) {
            sink.append(",\"self\":true");   // self is always "now" — no meaningful age
        } else if (d.cached) {
            // Restored from persistence, not re-heard live this session — `ageSec`
            // would be a fake "now" (the boot stamp), so emit `cached` instead. The UI
            // shows "last seen: cached"; once mDNS re-announces it, cached clears.
            sink.append(",\"cached\":true");
        } else {
            // Seconds since the last mDNS sighting. Computed device-side so the UI gets
            // one finished number; the same `now - lastSeenMs` age-out uses. Wrap-safe.
            uint32_t ageSec = (platform::millis() - d.lastSeenMs) / 1000u;
            sink.appendf(",\"ageSec\":%u", static_cast<unsigned>(ageSec));
        }
        sink.append("}");
    }

    // ListSource restore (persistence load): parse the saved `devices` array with the
    // recursive mm::json reader and rebuild devices_, so the last-known list shows on
    // boot before any announcement arrives. Tolerant of a malformed/over-large file
    // (parse fails → false → empty list). Self is dropped (re-added live via upsertSelf
    // with the current IP). Tolerates an OLD persisted file with extra keys (e.g. the
    // former `via`) — the keyed reader ignores them (robust to any input).
    bool restoreList(const char* json, const char* key) override {
        deviceCount_ = 0;
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
                d.cached = true;  // restored, not re-heard live → UI shows "cached", not a time
                // Stamp "now" so the cached entry gets a full kStaleMs grace window to
                // re-announce before age-out drops it (a still-alive but quiet device).
                d.lastSeenMs = platform::millis();
            });
        sortByName();   // cached list shows alphabetically too, before the first sighting
        return ok;      // false on a malformed/missing file (list left empty)
    }

    void onBuildControls() override {
        MoonModule::onBuildControls();
        controls_.addList("devices", *this);   // this module is the ListSource
    }

    void setup() override {
        MoonModule::setup();
        // The last-known device list is restored automatically before setup() by the
        // persistence overlay (the `devices` List control round-trips as JSON). So the
        // UI shows it INSTANTLY on boot — no waiting for an announcement to re-arrive.
        if (deviceCount_) {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "%u device%s (cached)",
                          deviceCount_, deviceCount_ == 1 ? "" : "s");
        }
        setStatus(statusBuf_);
    }

    // Every tick: ensure we're online, poll the mDNS listener for the current service
    // (rotating through the plugins' services), and age out devices unheard for
    // kStaleMs. The poll is non-blocking — the hot-path-safe replacement for the old
    // blocking HTTP sweep + blocking mDNS browse.
    void loop1s() override {
        MoonModule::loop1s();
        uint8_t local[4] = {};
        localIp(local);
        const bool online = local[0] || local[1] || local[2] || local[3];
        if (!online) return;   // no network yet — nothing to discover

        if (!selfListed_) { upsertSelf(local); selfListed_ = true; }

        // Query mDNS on a SLOW cadence — one service every kQueryEverySec ticks, not
        // every tick. Each query is a bounded blocking call into the IDF mDNS task;
        // firing both services every second exhausts that task's internal queue/buffers
        // (it reports "Cannot allocate memory" with megabytes of heap free — its own
        // fixed pool, not real OOM) and destabilises the network. A query every few
        // seconds, cycling through the plugins' services, keeps discovery responsive
        // (a device is found within a couple of cycles) at a fraction of the load.
        if (++queryTick_ >= kQueryEverySec) {
            queryTick_ = 0;
            const DevicePlugin* p = plugins_[serviceCursor_];
            platform::mdnsListenPoll(p->service(), p->proto(), &DevicesModule::onMdnsHit, this);
            serviceCursor_ = (serviceCursor_ + 1) % kPluginCount;
        }

        ageOut(local);
    }

    ModuleRole role() const override { return ModuleRole::Generic; }

    // Test seam: feed a synthetic mDNS hit through the real classify→upsert pipeline,
    // exactly as the platform listener callback does. The desktop has no live mDNS, so
    // this is how the full discovery path (plugin claim, type priority, name/IP merge)
    // is exercised in unit + scenario tests without a network. Not used in production
    // (the platform callback `onMdnsHit` is the live entry point).
    void injectMdnsHitForTest(const platform::MdnsHost& host) { mergeHit(host); }

private:
    struct Device {
        uint8_t  ip[4] = {};
        char     name[24] = {};
        DevType  type = DevType::Generic;
        bool     self = false;
        bool     cached = false; // restored from persistence, not yet re-heard live this
                                 // session. Cleared on the first live sighting.
        uint32_t lastSeenMs = 0; // platform::millis() at the most recent mDNS sighting.
                                 // Age-out drops a non-self device unheard for kStaleMs.
    };

    static constexpr uint8_t  kMaxDevices = 32;   // a LAN's worth; bounded, no heap
    // Query mDNS for one service this often (in loop1s ticks ≈ seconds). Deliberately
    // slow: a blocking mDNS query every tick exhausts the IDF mDNS task's internal pool
    // ("Cannot allocate memory" with megabytes of heap free) and destabilises the
    // network. With kPluginCount services round-robined, each is re-queried every
    // kQueryEverySec × kPluginCount seconds — responsive enough for discovery, light on
    // the mDNS task. (The old DevicesModule throttled its browse the same way.)
    static constexpr uint32_t kQueryEverySec = 5;
    // Drop a non-self device unheard for kStaleMs. Must exceed the full re-query cycle
    // (kQueryEverySec × kPluginCount × a few) so a present device isn't dropped between
    // its service's queries. A departed device clears within ~this window.
    static constexpr uint32_t kStaleMs = 60u * 1000u;

    // The interop plugins, polled round-robin. Order matters: the projectMM plugin
    // claims an _http._tcp+mm=1 hit before a generic fallback would. A new system
    // (ESPHome, Tasmota, Hue) is added by writing one plugin and listing it here — no
    // other change. const singletons, no per-device state.
    MmPlugin   mmPlugin_;
    WledPlugin wledPlugin_;
    static constexpr uint8_t kPluginCount = 2;
    const DevicePlugin* plugins_[kPluginCount] = { &mmPlugin_, &wledPlugin_ };
    uint8_t  serviceCursor_ = 0;  // which plugin's service mDNS-polls this cycle
    uint32_t queryTick_ = 0;      // counts loop1s ticks toward the next mDNS query

    Device  devices_[kMaxDevices];
    uint8_t deviceCount_ = 0;
    bool    selfListed_ = false;
    const char* selfName_ = nullptr;   // this device's name (wired via setSelfName)
    char    statusBuf_[40] = "idle";

    void localIp(uint8_t out[4]) const {
        platform::ethGetIPv4(out);
        if (!out[0] && !out[1] && !out[2] && !out[3]) platform::wifiStaGetIPv4(out);
    }

    // platform::MdnsHostCb trampoline — a resolved mDNS hit. Offer it to each plugin
    // that claims this hit's service; the first to classify it wins.
    static void onMdnsHit(const platform::MdnsHost& host, void* user) {
        static_cast<DevicesModule*>(user)->mergeHit(host);
    }

    void mergeHit(const platform::MdnsHost& host) {
        if (!host.ip[0] && !host.ip[1] && !host.ip[2] && !host.ip[3]) return;  // unresolved
        for (const DevicePlugin* p : plugins_) {
            if (std::strcmp(p->service(), host.service) != 0) continue;  // not this plugin's service
            DiscoveredDevice found;
            if (p->classify(host, found)) { upsertDevice(host.ip, found); return; }
        }
        // No plugin claimed it — a service we listen on but don't recognise; ignore.
    }

    // Find-or-insert a device a plugin recognised; refresh type/name, mark seen. Our own
    // _http._tcp+mm=1 announcement resolves to our own IP; mark that row self.
    void upsertDevice(const uint8_t ip[4], const DiscoveredDevice& found) {
        uint8_t local[4] = {};
        localIp(local);
        const bool isSelf = ipEq(ip, local);
        Device* d = findByIp(ip);
        if (!d) {
            if (deviceCount_ >= kMaxDevices) return;   // bounded; silently cap
            d = &devices_[deviceCount_++];
            std::memcpy(d->ip, ip, 4);
            d->type = found.type;          // first sighting — take the plugin's type
        }
        // A projectMM device advertises BOTH _http._tcp (mm=1) AND _wled._tcp, so its
        // _wled._tcp hit would otherwise relabel it WLED. projectMM is the stronger
        // identity (the mm=1 TXT is definitive): never downgrade an established projectMM
        // device. So only RAISE the type toward projectMM, never away from it.
        const bool isMm = isSelf || found.type == DevType::ProjectMM;
        if (isMm) d->type = DevType::ProjectMM;
        else if (d->type != DevType::ProjectMM) d->type = found.type;  // refine a non-MM row
        d->self = isSelf;
        d->lastSeenMs = platform::millis();
        d->cached = false;
        // Update the display name from this hit when it's AUTHORITATIVE for the device's
        // kind, so a device RENAME propagates live (a peer changing its deviceName shows
        // its new name here on the next query — no re-query needed). Authority rule: an
        // _http+mm=1 hit carries a projectMM device's real deviceName, so it always wins
        // for a projectMM row; a _wled hit's hostname is authoritative for a WLED row.
        // A _wled hit must NOT overwrite a projectMM device's deviceName (a projectMM
        // peer also answers _wled with its host name) — that's the lower-authority case.
        // Always fill an empty/placeholder name regardless of authority.
        const bool authoritative =
            (found.type == DevType::ProjectMM && d->type == DevType::ProjectMM) ||  // mm hit for an MM row
            (found.type == DevType::Wled);                                          // a WLED hit for WLED
        if (found.name[0] && (!d->name[0] || isIpPlaceholder(d->name, ip) || authoritative))
            std::snprintf(d->name, sizeof(d->name), "%s", found.name);
        if (!d->name[0]) formatDottedQuad(d->name, ip);
        sortByName();
        refreshStatus();
    }

    // Guarantee this device is listed (marked self). Self never ages out (restamped
    // "now" each tick it's online).
    void upsertSelf(const uint8_t ip[4]) {
        Device* d = findByIp(ip);
        if (!d) {
            if (deviceCount_ >= kMaxDevices) return;
            d = &devices_[deviceCount_++];
            std::memcpy(d->ip, ip, 4);
        }
        d->type = DevType::ProjectMM;
        d->self = true;
        d->cached = false;
        d->lastSeenMs = platform::millis();
        if (!d->name[0]) {
            const char* n = (selfName_ && selfName_[0]) ? selfName_ : "this device";
            std::snprintf(d->name, sizeof(d->name), "%s", n);
        }
        sortByName();
    }

    // True when `name` is just the device's own IP — a placeholder a sighting fell back
    // to before a real name was known. A later sighting with a genuine name overwrites it.
    static bool isIpPlaceholder(const char* name, const uint8_t ip[4]) {
        char ipStr[16];
        formatDottedQuad(ipStr, ip);
        return std::strcmp(name, ipStr) == 0;
    }

    Device* findByIp(const uint8_t ip[4]) {
        for (uint8_t i = 0; i < deviceCount_; i++)
            if (ipEq(devices_[i].ip, ip)) return &devices_[i];
        return nullptr;
    }

    static bool ipEq(const uint8_t a[4], const uint8_t b[4]) {
        return std::memcmp(a, b, 4) == 0;
    }

    // Drop non-self devices unheard for longer than kStaleMs. Self is restamped here so
    // it never ages out while online. Stable compaction — preserves by-name order.
    // `now - lastSeenMs` unsigned is wrap-safe (elapsed stays < 2^31 across the millis wrap).
    void ageOut(const uint8_t local[4]) {
        const uint32_t now = platform::millis();
        uint8_t w = 0;
        for (uint8_t r = 0; r < deviceCount_; r++) {
            Device& d = devices_[r];
            if (d.self || ipEq(d.ip, local)) d.lastSeenMs = now;   // self never goes stale
            if (!d.self && (now - d.lastSeenMs) > kStaleMs) continue;   // drop, stale
            if (w != r) devices_[w] = d;
            w++;
        }
        if (w == deviceCount_) return;   // nothing dropped — common case, no churn
        deviceCount_ = w;
        refreshStatus();
    }

    void refreshStatus() {
        std::snprintf(statusBuf_, sizeof(statusBuf_), "%u device%s",
                      deviceCount_, deviceCount_ == 1 ? "" : "s");
        setStatus(statusBuf_);
        // Persist the current set so the next boot shows it instantly. The `devices`
        // List control is persistable — marking dirty arms the FilesystemModule debounce.
        markDirty();
        FilesystemModule::noteDirty();
    }

    // Order the list by device name (case-insensitive). Core's insertionSort does the
    // work; we supply only the comparator. Off the hot path, bounded (<= kMaxDevices).
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
