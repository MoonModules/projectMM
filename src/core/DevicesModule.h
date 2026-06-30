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

// Discovers other devices on the LAN by UDP presence broadcast and presents them as a
// browsable list. Core + domain-neutral: it finds "a projectMM / a WLED device" and light
// modules (Art-Net sync, future SuperSync, device groups) consume the list rather than
// living here. Submodule of NetworkModule — discovery depends on the network being up.
// See docs/moonmodules/core/DevicesModule.md.
//
// Discovery is PASSIVE UDP: each device BROADCASTS a small presence packet on a well-known
// port (WLED + projectMM both use UDP 65506 with the 44-byte WLED-compatible header — see
// WledPacket), and this module LISTENS (a bound UdpSocket per port its DevicePlugins claim,
// drained non-blocking each tick). A plugin classifies each datagram into a `Device`.
// projectMM also broadcasts its OWN presence on a slow cadence so peers discover it (and a
// WLED app browsing 65506 can list it too). This replaces the former mDNS *query* path,
// which destabilised our own mDNS advertise (a PTR query for a service we also host
// exhausts the IDF mDNS pool — see docs/history/decisions.md). mDNS is now
// advertise-ONLY (so the WLED native app + Home Assistant discover us); discovery never
// queries. Fast (no subnet sweep), hot-path-safe (non-blocking recv), extensible (a new
// ecosystem is one new plugin file). Reliable device-to-device *commands* ride REST;
// latency-critical sync rides its own UDP — never this listener (lossy-OK presence only).
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
        if (d.type == DevType::Hue) sink.appendf(",\"colour\":%u", d.colourCount);
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
        if (d.type == DevType::Hue) sink.appendf(",\"colour\":%u", d.colourCount);
        if (d.self) {
            sink.append(",\"self\":true");   // self is always "now" — no meaningful age
        } else if (d.cached) {
            // Restored from persistence, not re-heard live this session — `ageSec`
            // would be a fake "now" (the boot stamp), so emit `cached` instead. The UI
            // shows "last seen: cached"; once a presence packet re-arrives, cached clears.
            sink.append(",\"cached\":true");
        } else {
            // Seconds since the last presence sighting. Computed device-side so the UI gets
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
                d.type = (std::strcmp(typeStr, "projectMM") == 0)  ? DevType::ProjectMM
                       : (std::strcmp(typeStr, "WLED") == 0)       ? DevType::Wled
                       : (std::strcmp(typeStr, "Hue bridge") == 0) ? DevType::Hue
                                                                   : DevType::Generic;
                d.colourCount = static_cast<uint8_t>(
                    mm::json::readInt(mm::json::member(doc, el, "colour")));   // 0 for non-bridge rows
                d.self = false;
                d.cached = true;  // restored, not re-heard live → UI shows "cached", not a time
                // Stamp "now" so the cached entry gets its kCachedGraceMs PROBATION window
                // (not the full 24 h) to be re-confirmed by a live packet before age-out
                // drops it. The persisted file has no real last-seen time — faking it as the
                // full 24 h would let a long-gone device survive forever across reboots (the
                // clock resets every boot). A live packet promotes it (clears `cached`, real
                // 24 h window); silence within probation means it's a ghost — drop it.
                d.lastSeenMs = platform::millis();
            });
        sortByName();   // cached list shows alphabetically too, before the first sighting
        return ok;      // false on a malformed/missing file (list left empty)
    }

    void onBuildControls() override {
        MoonModule::onBuildControls();
        controls_.addList("devices", *this);   // this module is the ListSource
    }

    // The boot DevicesModule (exactly one exists). A foreign-bridge driver in the light domain
    // (HueDriver) registers a discovered bridge through this without a compile-time dependency
    // on DevicesModule's address — the same static-seam shape as AudioModule::latestFrame().
    static DevicesModule* active() { return active_; }

    // Register a Hue bridge a HueDriver has connected to. Unlike upsertDevice (driven by a UDP
    // presence packet), a bridge is discovered out-of-band — the driver already holds its IP +
    // app key — so this is the explicit entry point for that. Idempotent: updates the name +
    // colour count of the existing row, or inserts one. `colour` is how many of the bridge's
    // lights are colour-capable, the figure for sizing a layout. Persisted like any device row.
    void upsertHueBridge(const uint8_t ip[4], const char* name, uint8_t colour) {
        Device* d = findByIp(ip);
        bool persistChanged = false;
        if (!d) {
            if (deviceCount_ >= kMaxDevices) return;   // bounded; silently cap
            d = &devices_[deviceCount_++];
            std::memcpy(d->ip, ip, 4);
            persistChanged = true;
        }
        if (d->type != DevType::Hue) { d->type = DevType::Hue; persistChanged = true; }
        if (name && name[0] && std::strcmp(d->name, name) != 0) {
            std::snprintf(d->name, sizeof(d->name), "%s", name);
            persistChanged = true;
        }
        if (!d->name[0]) { formatDottedQuad(d->name, ip); persistChanged = true; }
        if (d->colourCount != colour) { d->colourCount = colour; persistChanged = true; }
        d->lastSeenMs = platform::millis();   // transient — keeps the bridge from ageing out
        d->cached = false;
        if (persistChanged) { sortByName(); refreshStatus(); }
    }

    void setup() override {
        MoonModule::setup();
        active_ = this;
        // The last-known device list is restored automatically before setup() by the
        // persistence overlay (the `devices` List control round-trips as JSON). So the
        // UI shows it INSTANTLY on boot — no waiting for an announcement to re-arrive.
        if (deviceCount_) {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "%u device%s (cached)",
                          deviceCount_, deviceCount_ == 1 ? "" : "s");
        }
        setStatus(statusBuf_);
    }

    // Every tick: ensure we're online, drain inbound presence packets through the plugins,
    // broadcast our own presence on a slow cadence, and age out devices unheard for
    // kStaleMs. The drain is non-blocking (recvFrom returns -1 when nothing pending), so it
    // never stalls the tick — the hot-path-safe replacement for the old mDNS query.
    void loop1s() override {
        MoonModule::loop1s();
        uint8_t local[4] = {};
        localIp(local);
        const bool online = local[0] || local[1] || local[2] || local[3];
        if (!online) return;   // no network yet — nothing to discover

        // Re-register the self row every tick against the CURRENT local IP (idempotent —
        // find-or-update). Doing it once would pin the first-seen address forever; a later
        // DHCP renew / WiFi↔Eth switch changes our IP, and upsertSelf must follow it (and
        // ageOut drops the row left at the old address). Cheap: a bounded findByIp + stamp.
        upsertSelf(local);
        ensureListener();

        // Drain every presence packet received since the last tick (bounded — a busy LAN
        // sends a handful per interval), classifying each through the plugins.
        uint8_t buf[64];
        uint8_t srcIp[4];
        for (int i = 0; i < kMaxDrainPerTick; i++) {
            int n = listener_.recvFrom(buf, sizeof(buf), srcIp);
            if (n <= 0) break;   // -1 = nothing pending; done for this tick
            mergePacket(buf, static_cast<size_t>(n), srcIp);
        }

        // Broadcast our own presence every kBroadcastEverySec ticks so peers discover us.
        if (++broadcastTick_ >= kBroadcastEverySec) {
            broadcastTick_ = 0;
            broadcastPresence(local);
        }

        ageOut(local);
    }

    ModuleRole role() const override { return ModuleRole::Generic; }

    // Test seam: feed a synthetic presence datagram through the real classify→upsert
    // pipeline, exactly as the live recvFrom loop does. The desktop unit/scenario tests
    // drive the full discovery path (plugin claim, type priority, name/IP merge) with
    // hand-built packets — no network needed. Not used in production.
    void injectPacketForTest(const uint8_t* data, size_t len, const uint8_t srcIp[4]) {
        mergePacket(data, len, srcIp);
    }

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
        uint8_t  colourCount = 0; // Hue bridge only: how many of its lights are colour-capable
                                    // (the figure for sizing a layout). 0 for non-bridge rows.
    };

    // The boot instance, for active() — the foreign-bridge static seam (mirrors AudioModule).
    static inline DevicesModule* active_ = nullptr;

    static constexpr uint8_t  kMaxDevices = 32;   // a LAN's worth; bounded, no heap
    // Broadcast our presence every this-many loop1s ticks (≈ seconds). Slow + light, like
    // WLED's ~30 s beacon; a new device appears within this window. A departed device
    // clears within kStaleMs (sized to a few intervals so a present-but-quiet device isn't
    // dropped between its broadcasts).
    static constexpr uint32_t kBroadcastEverySec = 10;
    // Keep a device listed for 24 h after its last sighting, then drop. The list is a
    // durable "devices I've seen" history (persisted to flash, restored on boot), not just
    // "live right now": a device that goes offline survives a reboot and lingers a full day,
    // its freshness dot ageing green → yellow (>1 min) → red (>1 h) so the UI shows it
    // fading before it finally purges. A still-present device re-broadcasts every ~10 s, so
    // 24 h is never hit by a live peer.
    static constexpr uint32_t kStaleMs = 24u * 60u * 60u * 1000u;
    // Probation for a CACHED (restored-from-persistence, never-re-heard) device: keep it
    // only this long for a live packet to re-confirm it, else drop it as a ghost. Short, so
    // a stale persisted entry doesn't survive across reboots — the persisted file has no
    // real last-seen time, so a cached device's clock is "boot", not "actually last seen".
    static constexpr uint32_t kCachedGraceMs = 60u * 1000u;
    static constexpr int      kMaxDrainPerTick = 16;   // cap packets processed per tick (bounded work)

    // The interop plugins. Order matters: MmPlugin is first, so a projectMM peer's
    // marker-stamped packet is typed projectMM before WledPlugin would see it as a plain
    // WLED. A new system (ESPHome, Tasmota, Hue) is added by writing one plugin and listing
    // it here — no other change. const singletons, no per-device state.
    MmPlugin   mmPlugin_;
    WledPlugin wledPlugin_;
    static constexpr uint8_t kPluginCount = 2;
    const DevicePlugin* plugins_[kPluginCount] = { &mmPlugin_, &wledPlugin_ };

    platform::UdpSocket listener_;       // bound to the presence port; drained each tick
    bool     listenerBound_ = false;
    uint32_t broadcastTick_ = 0;         // counts loop1s ticks toward the next presence broadcast

    Device  devices_[kMaxDevices];
    uint8_t deviceCount_ = 0;
    const char* selfName_ = nullptr;   // this device's name (wired via setSelfName)
    char    statusBuf_[40] = "idle";

    void localIp(uint8_t out[4]) const {
        platform::ethGetIPv4(out);
        if (!out[0] && !out[1] && !out[2] && !out[3]) platform::wifiStaGetIPv4(out);
    }

    // Offer a received presence datagram to each plugin; the first to classify it wins.
    // (Order matters: MmPlugin is first, so a projectMM peer's marked packet is typed
    // projectMM before WledPlugin would see it as a plain WLED.)
    void mergePacket(const uint8_t* data, size_t len, const uint8_t srcIp[4]) {
        if (!srcIp[0] && !srcIp[1] && !srcIp[2] && !srcIp[3]) return;  // no source
        for (const DevicePlugin* p : plugins_) {
            DiscoveredDevice found;
            if (p->classifyPacket(data, len, srcIp, found)) { upsertDevice(srcIp, found); return; }
        }
        // No plugin claimed it — an unrecognised packet on a port we listen on; ignore.
    }

    // Bind the discovery listener once the network is up. Idempotent — a no-op once bound.
    // open() first (creates the fd AND enables SO_BROADCAST, which the presence broadcast
    // needs); then bind() to the plugins' discovery port. The port comes from the plugins'
    // discoveryPort() — the seam owns it, not a hardcoded constant — so adding a plugin on
    // the same port is free. Today both plugins share one port (projectMM + WLED on 65506),
    // so one socket receives + broadcasts; the assert pins that invariant. A future plugin
    // on a DIFFERENT port is the trigger to grow this to one socket per distinct port (the
    // shape is already a loop over plugins everywhere else).
    void ensureListener() {
        if (listenerBound_) return;
        const uint16_t port = plugins_[0]->discoveryPort();
        for (const DevicePlugin* p : plugins_)
            if (p->discoveryPort() != port) return;   // divergent ports unsupported yet — see note
        if (!listener_.open()) return;
        if (listener_.bind(port)) {
            listenerBound_ = true;
        } else {
            // bind failed (port busy this tick) — CLOSE the just-opened socket before
            // returning, or each retry would open() a fresh fd and leak one per loop1s
            // until the process runs out, slowing everything to a crawl.
            listener_.close();
        }
    }

    // Broadcast our presence: a WLED-valid 44-byte packet (so WLED apps/devices browsing
    // 65506 list us) stamped with the projectMM marker (so peer projectMM devices type us
    // correctly). Discovery-only — carries no command, so a receiving WLED only lists us.
    void broadcastPresence(const uint8_t ip[4]) {
        uint8_t pkt[WledPacket::kSize];
        const char* n = (selfName_ && selfName_[0]) ? selfName_ : "projectMM";
        WledPacket::build(pkt, ip, n, boardTypeByte(), /*lightsOn=*/true);
        WledPacket::stampMmMarker(pkt);
        const uint8_t bcast[4] = {255, 255, 255, 255};
        listener_.sendToAddr(bcast, WledPacket::kPort, pkt, sizeof(pkt));
    }

    // WLED's board-type byte (low 7 bits): 32=ESP32, 33=S2, 34=S3, 35=C3, 36=P4. Best-effort
    // from the chip model string; an unknown chip falls back to 32 (plain ESP32) — purely
    // informational in the packet (WLED shows an icon), never gates discovery.
    static uint8_t boardTypeByte() {
        const char* m = platform::chipModel();
        if (std::strstr(m, "S3")) return 34;
        if (std::strstr(m, "S2")) return 33;
        if (std::strstr(m, "C3")) return 35;
        if (std::strstr(m, "P4")) return 36;
        return 32;
    }

    // Find-or-insert a device a plugin classified from a UDP presence packet; refresh
    // type/name, mark seen. Our own presence packet (carrying the projectMM marker) resolves
    // to our own source IP; mark that row self. Persistence is armed ONLY when a SAVED field
    // (name/ip/type/self) actually changes — a mere re-sighting (lastSeenMs/cached) doesn't
    // alter the serialized list, so it must not trigger a flash write every ~10 s broadcast.
    void upsertDevice(const uint8_t ip[4], const DiscoveredDevice& found) {
        uint8_t local[4] = {};
        localIp(local);
        const bool isSelf = ipEq(ip, local);
        Device* d = findByIp(ip);
        bool persistChanged = false;
        if (!d) {
            if (deviceCount_ >= kMaxDevices) return;   // bounded; silently cap
            d = &devices_[deviceCount_++];
            std::memcpy(d->ip, ip, 4);
            d->type = found.type;          // first sighting — take the plugin's type
            persistChanged = true;         // a new row changes the saved list
        }
        // A projectMM device broadcasts a marked, WLED-VALID packet — without the marker
        // check a peer would relabel WLED. projectMM is the stronger identity (the marker is
        // definitive): never downgrade an established projectMM device, only RAISE toward it.
        const bool isMm = isSelf || found.type == DevType::ProjectMM;
        const DevType newType = isMm ? DevType::ProjectMM
                              : (d->type != DevType::ProjectMM ? found.type : d->type);
        if (d->type != newType) { d->type = newType; persistChanged = true; }
        if (d->self != isSelf) { d->self = isSelf; persistChanged = true; }
        d->lastSeenMs = platform::millis();    // transient — not persisted
        d->cached = false;                     // transient — not persisted
        // Update the display name when this packet is AUTHORITATIVE for the device's kind,
        // so a peer RENAME propagates live (its next broadcast carries the new name). A
        // projectMM-marked packet is authoritative for a projectMM row; a plain WLED packet
        // for a WLED row — a WLED packet must NOT overwrite a projectMM device's name (a
        // projectMM peer's packet without the marker is the lower-authority case). Always
        // fill an empty/placeholder name regardless of authority.
        const bool authoritative =
            (found.type == DevType::ProjectMM && d->type == DevType::ProjectMM) ||
            (found.type == DevType::Wled);
        if (found.name[0] && (!d->name[0] || isIpPlaceholder(d->name, ip) || authoritative)
            && std::strcmp(d->name, found.name) != 0) {
            std::snprintf(d->name, sizeof(d->name), "%s", found.name);
            persistChanged = true;
        }
        if (!d->name[0]) { formatDottedQuad(d->name, ip); persistChanged = true; }
        if (persistChanged) {                  // only a saved-field change touches disk + sort
            sortByName();
            refreshStatus();
        }
    }

    // Guarantee the self row exists at the current local IP (called every tick — idempotent).
    // Self never ages out (the row at the current IP is restamped each tick). Re-sorts +
    // refreshes status ONLY when the row actually changed (a fresh insert or an IP migration),
    // not every tick — a no-op tick must not arm persistence (same rule as upsertDevice).
    void upsertSelf(const uint8_t ip[4]) {
        bool changed = false;
        // Demote any prior self row at a DIFFERENT address — our IP moved (DHCP / interface
        // switch). It loses the self mark, so ageOut treats it as an ordinary peer and lets
        // it expire, instead of staying immortal at the old address.
        for (uint8_t i = 0; i < deviceCount_; i++)
            if (devices_[i].self && !ipEq(devices_[i].ip, ip)) { devices_[i].self = false; changed = true; }

        Device* d = findByIp(ip);
        if (!d) {
            if (deviceCount_ >= kMaxDevices) return;
            d = &devices_[deviceCount_++];
            std::memcpy(d->ip, ip, 4);
            changed = true;            // a new row changes the saved list
        }
        if (d->type != DevType::ProjectMM) { d->type = DevType::ProjectMM; changed = true; }
        if (!d->self) { d->self = true; changed = true; }
        d->cached = false;
        d->lastSeenMs = platform::millis();   // transient — not persisted
        if (!d->name[0]) {
            const char* n = (selfName_ && selfName_[0]) ? selfName_ : "this device";
            std::snprintf(d->name, sizeof(d->name), "%s", n);
            changed = true;
        }
        if (changed) {                 // only a real self-row change re-sorts + arms persistence
            sortByName();
            refreshStatus();
        }
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
            const bool isUs = ipEq(d.ip, local);
            // The row at the CURRENT local IP is us — keep it fresh, never age it out. Guard
            // on the ADDRESS, not the self flag: a stale self row at an old IP (after an IP
            // change) is demoted by upsertSelf, so it falls through to the normal age-out.
            if (isUs) d.lastSeenMs = now;
            // A cached (restored, never re-heard live) device is on a SHORT probation —
            // it's the fast-boot list, kept only long enough for a live packet to re-confirm
            // it; otherwise it's a ghost. A live-confirmed device gets the full 24 h.
            const uint32_t window = d.cached ? kCachedGraceMs : kStaleMs;
            if (!isUs && (now - d.lastSeenMs) > window) continue;   // drop, stale
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
