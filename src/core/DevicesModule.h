#pragma once

#include "core/MoonModule.h"
#include "core/ActiveInstance.h"   // the boot-registry seat election (the seat + its RAII vacate)
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

/// Discovers other devices on the LAN by UDP presence broadcast and presents them
/// as a browsable list, focusing on *all* devices on the network (including this
/// one, marked as self) rather than the host's own state
///
/// Core + domain-neutral: it finds "a projectMM / a WLED device" and light modules
/// (Art-Net sync, device groups) consume the list rather than living here, so its
/// card looks the same on every projectMM instance, ESP32 or desktop. Submodule of
/// NetworkModule — discovery depends on the network being up (the same placement
/// reasoning as the Improv provisioning module), wired in `main.cpp` and marked
/// wired-by-code so persistence preserves it.
///
/// **Discovery is passive UDP.** Each device BROADCASTS a small presence packet on
/// a well-known port (WLED + projectMM both use UDP 65506 with the 44-byte
/// WLED-compatible header — see `WledPacket`), and this module LISTENS (a bound
/// `UdpSocket` per port its `DevicePlugin`s claim, drained non-blocking each tick).
/// No subnet sweep, no per-host probe, no mDNS query — a device appears when its
/// broadcast arrives and ages out when it stops. projectMM also broadcasts its OWN
/// presence on a slow cadence (~10 s) so peers discover it, and a WLED app browsing
/// 65506 lists it too (discovery-only: a receiving WLED shows us in its instances
/// list, it does not sync to it). This replaces the former mDNS *query* path, which
/// destabilized our own mDNS advertise (a PTR query for a service we also host
/// exhausts the IDF mDNS pool). mDNS is
/// advertise-ONLY (announcing `_http._tcp`+`mm=1` and `_wled._tcp`+`mac=` so the
/// WLED native app + Home Assistant, which only browse mDNS, discover us); discovery
/// never queries.
///
/// **Plugins are the interop seam.** Foreign ecosystems hook in as plugins, not
/// hardcoded branches (the adapter pattern, cf. `ListSource`, `ModuleFactory`): a
/// `DevicePlugin` declares its UDP port and turns a datagram into a `Device` kind.
/// `MmPlugin` claims a marked WLED-valid packet as projectMM (offered first, so a
/// projectMM peer isn't double-claimed as WLED); `WledPlugin` claims an unmarked one
/// as WLED. A new system is one new plugin file — no core edit. Out-of-band devices
/// (a Philips Hue bridge found over HTTP by a light-domain driver) register through
/// `upsertHueBridge()` via the `active()` boot-instance seam, keeping the Hue
/// pairing entirely in the driver.
///
/// **Age-out + persistence.** Each sighting stamps `lastSeenMs`; a live-confirmed
/// device is kept `kStaleMs` (24 h) as a durable "devices I've seen" history, while
/// a cached row (restored from persistence, not yet re-heard) gets only a short
/// `kCachedGraceMs` (60 s) probation so a long-gone persisted device can't survive
/// forever across reboots. The `devices` List control is persistable, so the
/// last-known list is restored on boot (shown as "N devices (cached)") before the
/// first announcement arrives; the self row is re-added live with the current IP.
/// Storage is a fixed `devices_[kMaxDevices]` array — bounded, no heap.
///
/// **Transport boundary.** This module does *discovery* only (lossy-OK presence,
/// never device-to-device commands). Consumers reach a found device over the right
/// transport: must-arrive config rides REST; latency-critical lossy-OK traffic
/// (time sync, live pixels) rides its own UDP stream.
///
/// **WLED interop.** Because the presence broadcast and the mDNS advertise are
/// WLED-shaped, a projectMM device appears in the WLED ecosystem with no projectMM
/// software on the other side: it shows in a real WLED's own instances list (heard on
/// UDP 65506), and in the native WLED iOS/Android app (discovered via the `_wled._tcp`
/// mDNS advertise, validated via a `/json/info` shim — see HttpServerModule's
/// WLED-compatibility shim).
///
/// **Wire shape.** The `devices` List serializes each row's `value` as
/// `{"name","ip","type",["self"]}`, with a parallel `detail` object carrying `url` and
/// `ageSec` (seconds since last heard, `now − lastSeenMs`; omitted on the self row,
/// always current). A row restored from persistence but not yet re-heard live this
/// session carries `cached:true` instead of `ageSec`; the UI shows "last seen: cached"
/// until an announcement re-confirms it (clearing `cached` and emitting a real `ageSec`,
/// rendered as "last seen 2m ago").
///
/// **Prior art:** the industry-standard mDNS-SD / DNS-SD (Bonjour, Avahi)
/// announce-and-browse pattern, plus MoonLight's UDP presence broadcast carried
/// forward as the 44-byte WLED-compatible packet on UDP 65506.
/// @card DevicesModule.png
class DevicesModule : public MoonModule, public ListSource {
public:
    /// Wire this device's own name (deviceName) before setup so the self row matches the
    /// status page / router / mDNS. Borrowed pointer — caller owns stable storage (SystemModule).
    void setSelfName(const char* name) { selfName_ = name; }

    /// ListSource — rows are produced straight from devices_ (no copy, no alloc).
    uint8_t listRowCount() const override { return deviceCount_; }

    void writeListRow(JsonSink& sink, uint8_t row) const override {
        if (row >= deviceCount_) { sink.append("{}"); return; }
        const Device& d = devices_[row];
        char ip[16];
        formatDottedQuad(ip, d.ip);
        sink.append("{\"name\":");
        sink.writeJsonString(d.name[0] ? d.name : ip);
        sink.appendf(",\"ip\":\"%s\",\"type\":\"%s\"", ip, devTypeStr(d.type));
        if (d.type == DevType::Hue) sink.appendf(",\"color\":%u", d.colorCount);
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
        if (d.type == DevType::Hue) sink.appendf(",\"color\":%u", d.colorCount);
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

    /// ListSource restore (persistence load): parse the saved `devices` array with the
    /// recursive mm::json reader and rebuild devices_, so the last-known list shows on
    /// boot before any announcement arrives. Tolerant of a malformed/over-large file
    /// (parse fails → false → empty list). Self is dropped (re-added live via upsertSelf
    /// with the current IP). Tolerates an OLD persisted file with extra keys (such as the
    /// former `via`) — the keyed reader ignores them (robust to any input).
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
                // Clamp the persisted count to the valid range (0..127, the HueDriver's
                // colorCount_ range) so a corrupt or hand-edited entry can't wrap into a bogus
                // value when narrowed. 0 for non-bridge rows (the key is absent → readInt = 0).
                const long color = mm::json::readInt(mm::json::member(doc, el, "color"));
                d.colorCount = static_cast<uint8_t>(color < 0 ? 0 : (color > 127 ? 127 : color));
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

    /// Announce ourselves on WLED's broadcast address as well as the multicast group.
    ///
    /// OFF (the default) announces on the multicast group alone, which is the better neighbour:
    /// a broadcast wakes every phone, printer and laptop on the LAN to parse a packet none of them
    /// want, where multicast reaches only the devices that joined (and a switch with IGMP snooping
    /// does not even forward it to the other ports). ON adds the broadcast copy that WLED apps and
    /// devices need, since they browse this port on BROADCAST: turn it on to appear in them.
    ///
    /// Not automatic, for the same reason the sACN multicast option is opt-in: without IGMP
    /// snooping a switch floods multicast exactly like broadcast, and on WiFi it goes out at the
    /// lowest basic rate to every station. Firmware cannot tell which kind of network it is on.
    ///
    /// And flooding is the GOOD failure. Multicast also just fails to arrive on some consumer gear,
    /// most often where the path bridges physical media (a WiFi client to a wired one), so an empty
    /// device list can mean the group never got through rather than that nobody is there. A device
    /// cannot tell those apart by listening, since both are silence: see backlog-core.md,
    /// "Multicast discovery has no fallback when the group never arrives".
    ///
    /// **Devices need not agree on this.** Presence ALWAYS goes to the group and every device
    /// ALWAYS joins it, so projectMM peers find each other whatever each has chosen; the flag
    /// only adds the broadcast copy WLED needs. A fleet can therefore be mixed, and turning it
    /// off on one device never hides it from another.
    bool wledCompatible = false;

    void defineControls() override {
        MoonModule::defineControls();
        controls_.addControl("wledCompatible", wledCompatible);
        controls_.addList("devices", *this);   // this module is the ListSource
    }

    /// The boot DevicesModule (exactly one exists). A foreign-bridge driver in the light domain
    /// (a Hue driver) registers a discovered bridge through this without a compile-time dependency
    /// on DevicesModule's address — the same static-seam shape as `AudioService::latestFrame()`.
    static DevicesModule* active() { return ActiveInstance<DevicesModule>::active(); }

    /// Register a Hue bridge a light-domain driver has connected to. Unlike upsertDevice (driven by a UDP
    /// presence packet), a bridge is discovered out-of-band — the driver already holds its IP +
    /// app key — so this is the explicit entry point for that. Idempotent: updates the name +
    /// color count of the existing row, or inserts one. `color` is how many of the bridge's
    /// lights are color-capable, the figure for sizing a layout. Persisted like any device row.
    void upsertHueBridge(const uint8_t ip[4], const char* name, uint8_t color) {
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
        if (d->colorCount != color) { d->colorCount = color; persistChanged = true; }
        d->lastSeenMs = platform::millis();   // transient — keeps the bridge from ageing out
        // A cached row coming (back) online is a status change even with no persisted field edit —
        // refresh on that transition too, else a re-announced bridge stays greyed-out in the UI.
        const bool wasCached = d->cached;
        d->cached = false;
        if (persistChanged) sortByName();              // re-sort only on a real persisted change
        if (persistChanged || wasCached) refreshStatus();
    }

    /// One-time wiring, enabled-INDEPENDENT: show the cached device list on boot. The last-known
    /// list is restored before setup() by the persistence overlay (the `devices` List round-trips
    /// as JSON), so the UI shows it INSTANTLY — even for a disabled instance, which still displays
    /// what it last saw. The seat + socket (the actual resource) are handled in prepare.
    void setup() override {
        MoonModule::setup();
        if (deviceCount_) {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "%u device%s (cached)",
                          deviceCount_, deviceCount_ == 1 ? "" : "s");
        }
        setStatus(statusBuf_);
    }

    /// Pure build (see MoonModule::prepare): claim the singleton seat (the Hue-bridge routing target).
    /// No enabled() check — core's applyState() calls this only when effectively-enabled and routes to
    /// release() otherwise, which vacates the seat and closes the presence socket, so a disabled
    /// instance (or one under a disabled parent) frees the port. Exactly one DevicesModule exists, so
    /// claim-if-empty is equivalent to an unconditional claim here.
    void prepare() override {
        seat_.claim();
    }

    /// Close the presence socket so its port is released (the module holds it via the lazy
    /// ensureListener bind). Also runs on module removal so the fd doesn't leak.
    void release() override {
        seat_.vacate();
        listener_.close();
        listenerBound_ = false;
        MoonModule::release();
    }

    /// Every tick: ensure we're online, drain inbound presence packets through the plugins,
    /// broadcast our own presence on a slow cadence, and age out devices unheard for
    /// kStaleMs. The drain is non-blocking (recvFrom returns -1 when nothing pending), so it
    /// never stalls the tick — the hot-path-safe replacement for the old mDNS query.
    void tick1s() MM_NONBLOCKING override {
        MoonModule::tick1s();
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

    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Generic; }

    /// Test seam: feed a synthetic presence datagram through the real classify→upsert
    /// pipeline, exactly as the live recvFrom loop does. The desktop unit/scenario tests
    /// drive the full discovery path (plugin claim, type priority, name/IP merge) with
    /// hand-built packets — no network needed. Not used in production.
    void injectPacketForTest(const uint8_t* data, size_t len, const uint8_t srcIp[4]) {
        mergePacket(data, len, srcIp);
    }

private:
    struct Device {
        uint8_t  ip[4] = {};
        char     name[24] = {};
        DevType  type = DevType::Generic;
        bool     self = false;
        bool     cached = false; ///< restored from persistence, not yet re-heard live this
                                 ///< session. Cleared on the first live sighting.
        uint32_t lastSeenMs = 0; ///< platform::millis() at the most recent presence sighting.
                                 ///< Age-out drops a non-self device unheard for kStaleMs.
        uint8_t  colorCount = 0; ///< Hue bridge only: how many of its lights are color-capable
                                    ///< (the figure for sizing a layout). 0 for non-bridge rows.
    };

    // The boot-instance seat, for active() — the foreign-bridge static seam (mirrors AudioService).
    // Claimed on build, vacated on release + on destruction (the RAII member's dtor guards the static).
    ActiveInstance<DevicesModule> seat_{*this};

    static constexpr uint8_t  kMaxDevices = 32;   ///< a LAN's worth; bounded, no heap
    /// Broadcast our presence every this-many tick1s ticks (≈ seconds). Slow + light, like
    /// WLED's ~30 s beacon; a new device appears within this window. A departed device
    /// clears within kStaleMs (sized to a few intervals so a present-but-quiet device isn't
    /// dropped between its broadcasts).
    static constexpr uint32_t kBroadcastEverySec = 10;
    /// Keep a device listed for 24 h after its last sighting, then drop. The list is a
    /// durable "devices I've seen" history (persisted to flash, restored on boot), not just
    /// "live right now": a device that goes offline survives a reboot and lingers a full day,
    /// its freshness dot ageing green → yellow (>1 min) → red (>1 h) so the UI shows it
    /// fading before it finally purges. A still-present device re-broadcasts every ~10 s, so
    /// 24 h is never hit by a live peer.
    static constexpr uint32_t kStaleMs = 24u * 60u * 60u * 1000u;
    /// Probation for a CACHED (restored-from-persistence, never-re-heard) device: keep it
    /// only this long for a live packet to re-confirm it, else drop it as a ghost. Short, so
    /// a stale persisted entry doesn't survive across reboots — the persisted file has no
    /// real last-seen time, so a cached device's clock is "boot", not "actually last seen".
    static constexpr uint32_t kCachedGraceMs = 60u * 1000u;
    static constexpr int      kMaxDrainPerTick = 16;   ///< cap packets processed per tick (bounded work)

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
    uint32_t broadcastTick_ = 0;         // counts tick1s ticks toward the next presence broadcast

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
        // No plugin claimed it — an unrecognized packet on a port we listen on; ignore.
    }

    // Bind the discovery listener once the network is up. Idempotent — a no-op once bound.
    // open() first (creates the fd AND enables SO_BROADCAST, which the presence broadcast
    // needs); then bind() to the plugins' discovery port. The port comes from the plugins'
    // discoveryPort() — the seam owns it, not a hardcoded constant — so adding a plugin on
    // the same port is free. Today both plugins share one port (projectMM + WLED on 65506),
    // so one socket receives + broadcasts; the assert pins that invariant. A future plugin
    // on a DIFFERENT port is the trigger to grow this to one socket per distinct port (the
    // shape is already a loop over plugins everywhere else).
    /// The projectMM discovery group. 239.255.x.x is the IPv4 organization-local scope (RFC 2365):
    /// site-local, never routed off the network, and the same block sACN uses. .77 is ours.
    static constexpr uint8_t kDiscoveryGroup[4] = {239, 255, 77, 77};

    void ensureListener() {
        if (listenerBound_) return;
        const uint16_t port = plugins_[0]->discoveryPort();
        for (const DevicePlugin* p : plugins_)
            if (p->discoveryPort() != port) return;   // divergent ports unsupported yet — see note
        if (!listener_.open()) return;
        if (listener_.bind(port)) {
            listenerBound_ = true;
            // Join the projectMM discovery group so a peer's multicast presence reaches us even
            // when it has WLED compatibility off. Best-effort: a stack without multicast still
            // hears the broadcast half, so discovery degrades rather than breaking.
            char grp[16];
            std::snprintf(grp, sizeof(grp), "%u.%u.%u.%u", kDiscoveryGroup[0], kDiscoveryGroup[1],
                          kDiscoveryGroup[2], kDiscoveryGroup[3]);
            listener_.joinMulticast(grp);
        } else {
            // bind failed (port busy this tick) — CLOSE the just-opened socket before
            // returning, or each retry would open() a fresh fd and leak one per tick1s
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
        // The multicast group always: peer projectMM devices listen there, and it costs the rest
        // of the LAN nothing. The broadcast additionally, unless the user turned WLED compatibility
        // off: WLED apps and devices browse this port on BROADCAST, so dropping it makes projectMM
        // invisible to them. The protocol's owner decides the transport, and this control is where
        // a projectMM-only network gets to stop paying for WLED's choice.
        listener_.sendToAddr(kDiscoveryGroup, WledPacket::kPort, pkt, sizeof(pkt));
        if (wledCompatible) {
            const uint8_t bcast[4] = {255, 255, 255, 255};
            listener_.sendToAddr(bcast, WledPacket::kPort, pkt, sizeof(pkt));
        }
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
