#pragma once

#include "core/MoonModule.h"
#include "core/MqttPacket.h"
#include "core/SystemModule.h"

namespace mm { class ControlModule; }   // presets published as the HA effect list; .cpp includes it
#include "platform/platform.h"

#include <cstdint>

namespace mm {

/// MQTT client service: bridges the device's light controls (on / brightness / palette) to an MQTT
/// broker so home-automation hubs can drive it. The headline consumer is **Homebridge** (via the
/// `homebridge-mqttthing` "lightbulb" accessory), which publishes to `set` topics and reads `get`
/// topics — a bare on/off + brightness + color surface. It is a network sub-service, a code-wired
/// child of NetworkModule alongside Improv/Devices, and it drives the shared apply-core exactly as
/// IR and the WLED bridge do: every command routes through `Scheduler::setControl("Drivers", …)`,
/// so MQTT adds a transport, not new control plumbing.
///
/// **The client is our own.** MQTT 3.1.1 is a small, standard protocol (the same framing mosquitto
/// and mqttthing speak), so the wire format lives in a dependency-free, golden-vector-tested header
/// (MqttPacket.h) and this module owns only the socket lifecycle over `platform::TcpConnection`
/// (`connect` + the non-blocking `read`/`writeSome`). No library — the framing is pinned by tests
/// the way the Improv frames are.
///
/// **Topics** (prefix `projectMM/<last6-of-MAC>` — a STABLE id, so a rename never repoints topics;
/// the friendly deviceName rides the separate retained `<prefix>/name` topic):
///   `<prefix>/on/set`         ← "true"/"false"        → Drivers.on
///   `<prefix>/on/get`         → publish current on
///   `<prefix>/brightness/set` ← 0..100 (mqttthing)    → *255/100 → Drivers.brightness
///   `<prefix>/brightness/get` → publish brightness*100/255
///   `<prefix>/hsv/set`        ← "h,s,v"               → hue+sat → nearest palette → Drivers.palette
///   `<prefix>/hsv/get`        → publish the chosen palette's representative "h,s,v"
///
/// **Home Assistant MQTT Discovery** (the `haDiscovery` control, default off / opt-in). When on, the
/// device publishes a RETAINED JSON-schema light config to `homeassistant/light/projectMM_<mac6>/config`,
/// so HA (and any Discovery-aware hub — the Tasmota/ESPHome/Zigbee2MQTT convention) **auto-creates a
/// wired light entity** — no hand-matching topics. It defaults off because the WLED-compat surface (the
/// HttpServerModule `/json` shim) already gives HA a richer light — color, palette, sensors — over
/// mDNS with no broker, so a device on defaults appears in HA once, not twice; MQTT discovery is the
/// opt-in for broker-only / cross-subnet setups where mDNS doesn't reach. When on it speaks HA's own
/// schema alongside the mqttthing topics above:
///   `<prefix>/ha/set`         ← `{"state":"ON"|"OFF"[,"brightness":0-255]}` → Drivers.on/brightness
///   `<prefix>/ha/state`       → retained `{"state":…,"brightness":0-255}` on change (HA-scale, no rescale)
///   `<prefix>/status`         → retained "online"; the CONNECT **Last-Will** publishes "offline" here
///                               on an ungraceful drop, so HA greys the entity out (`avty_t`)
/// Toggling `haDiscovery` re-announces / retracts live (an empty retained config removes the entity),
/// no reconnect. `uniq_id` is the MAC-stable `projectMM_<mac6>`, never the editable name. The schema is
/// JSON (not the default schema): a control maps to a config key rather than a topic, so HA's native
/// `effect`/`effect_list` renders a picker on the one command topic — the reason JSON is chosen here.
///
/// The same discovery-gate also publishes a second HA component — an **update entity** — on
/// `homeassistant/update/projectMM_<mac6>/config`. HA renders it as a *"Firmware: <version>"* card
/// in the device panel with an Install button; state comes from `<prefix>/update/state`, install
/// commands from `<prefix>/update/set`. `installed_version` = build-time `MM_VERSION`; `latest_version`
/// equals it today (no on-device release check yet, so HA shows "up-to-date"), and the install path
/// routes to `platform::http_fetch_to_ota`. Prevents HA's WLED integration from mis-flagging a WLED
/// firmware update against a projectMM device — the `/json` `info.ver` reports our semver so WLED's
/// comparison is always-newer, and this update entity is where a real projectMM update surfaces once
/// the release-check component lands.
///
/// **Lifecycle** (all on tick1s(), off the render hot path — MQTT is slow control): connect lazily
/// once `networkReady() && enabled`, CONNECT → CONNACK → SUBSCRIBE to the `set` topics, PINGREQ every
/// keepalive/2, drain inbound bytes through MqttInboundParser and route PUBLISHes to Drivers, and
/// publish the `get` topics whenever the local value changes (and on connect, so mqttthing never
/// reads "No Response"). A dropped socket reconnects with a backoff.
///
/// **Prior art:** the OASIS MQTT 3.1.1 standard, homebridge-mqttthing's topic conventions, and Home
/// Assistant's MQTT-discovery format (the same retained-`homeassistant/…/config` announce Tasmota /
/// ESPHome / Zigbee2MQTT use). projectMM writes its own lean client over the platform socket
/// primitive rather than a framework MQTT library. See docs/moonmodules/core/system.md#mqtt for the
/// Homebridge accessory config; docs/usecases/home-automation.md for the HA setup.
/// @card MqttModule.png
class MqttModule : public MoonModule {
public:
    void setSystemModule(SystemModule* s) { systemModule_ = s; }
    /// Optional: presets are published to Home Assistant as the light's effect list when set. Only
    /// look-only presets are exposed (ControlModule::isLookOnly), so an automation cannot rewire
    /// hardware through the effect dropdown.
    void setControlModule(ControlModule* c) { controlModule_ = c; }

    void setup() override;
    void release() override;                          // free the lazily-allocated discovery buffers
    void defineControls() override;
    void onControlChanged(const char* controlName) override;   // a broker/port/cred change re-homes the socket
    void onEnabled(bool enabled) override;             // enable/disable → connect / clean DISCONNECT
    void tick1s() MM_NONBLOCKING override;

    /// Feed inbound bytes as if they arrived from the broker socket — the entry the host unit tests
    /// drive (there's no live broker in ctest). Mirrors InfraredService::injectCodeForTest.
    void feedForTest(const uint8_t* bytes, size_t len);

    /// Test seam: capture every outbound packet sendPacket() writes, so a unit test can assert what
    /// the module emits (e.g. the retained discovery config on CONNACK) — there's no live socket in
    /// ctest. Enable before the exercise; read back the concatenated bytes. Off in production (the
    /// capture buffer is null).
    void enableSendCaptureForTest(uint8_t* buf, size_t cap);
    size_t sentCaptureLenForTest() const { return sendCaptureLen_; }

    /// The heap footprint dynamicBytes() reports while HA discovery is announcing — the sum of the two
    /// discovery scratch regions. Exposed so a test asserts against this instead of a magic literal.
    /// The no-presets footprint. With looks published the buffers grow by the measured effect-list
    /// size, so the live figure is dynamicBytes(); this constant is the floor.
    static constexpr size_t kDiscoveryDynamicBytes = 320 + 448;

private:
    // Connection state machine — advanced by tick1s(). ConnectingTcp = a non-blocking TCP connect is
    // in flight (polled, never blocks the tick); Connecting = TCP up, CONNECT sent, awaiting CONNACK.
    enum class Conn : uint8_t { Idle, ConnectingTcp, Connecting, Connected };

    void startConnect();                    // begin a non-blocking TCP connect (ConnectingTcp)
    void sendConnectPacket();               // TCP up → send CONNECT, go to Connecting
    void serviceConnected();                // drain inbound, keepalive, publish-on-change
    bool sendPacket(const uint8_t* data, size_t len);   // non-blocking atomic send (true = fully sent)
    void resetConnection(const char* status);   // close socket + back to Idle with a status line
    void handleInboundByte(uint8_t byte);   // feed the parser, route a completed PUBLISH
    void routePublish(const char* topic, const uint8_t* payload, size_t payloadLen);
    void publishState(bool force);          // publish get topics when local values changed
    void publishName();                     // publish the friendly deviceName on the retained name topic
    void maybeRepublishName();              // re-publish the name if it changed (rename while connected)
    void setControlValue(const char* control, const char* valueJson);   // → Scheduler::setControl
    void setStatusLine(const char* msg);

    // Home Assistant MQTT Discovery (JSON schema). When haDiscovery_ is on, the device announces a
    // retained `homeassistant/light/<id>/config` so HA auto-creates a wired light entity; it then
    // speaks HA's own JSON schema on <prefix>/ha/{set,state} alongside the mqttthing on/set etc.
    void buildDiscoveryTopic(char* out, size_t cap) const;   // homeassistant/light/projectMM_<mac6>/config
    void buildStatusTopic(char* out, size_t cap) const;      // <prefix>/status — the LWT availability topic
    void publishDiscovery(bool announce);   // announce=false publishes an empty retained config (retract)
    void subscribeHaSet();                  // SUBSCRIBE to <prefix>/ha/set (at CONNACK + on live toggle-on)

    // HA update entity — a second HA-discovery component in the same shape as the light above, on
    // `homeassistant/update/projectMM_<mac6>/config`. Renders as a first-class *"Firmware: <version>"*
    // card in the device panel with an Install button; state comes from `<prefix>/update/state`,
    // install commands from `<prefix>/update/set` route to `platform::http_fetch_to_ota`. A companion
    // to the light discovery: same haDiscovery gate, same announce/retract pair, no per-tick state
    // (`installed_version` is a compile-time constant). `latest_version` equals `installed_version`
    // today (no on-device release check yet — see the "HA update entity via MQTT discovery" backlog
    // item), so HA renders "up-to-date" and the Install button stays disabled until a release-check
    // component fills in a newer `latest_version`. Wiring the install path now sets the shape for
    // that follow-up rather than leaving a half-built discovery config in the tree.
    void buildUpdateDiscoveryTopic(char* out, size_t cap) const;
    void publishUpdateDiscovery(bool announce);
    void publishUpdateState();              // retained JSON on <prefix>/update/state
    void subscribeUpdateSet();              // SUBSCRIBE to <prefix>/update/set (install command)
    void handleUpdateInstall(const char* payload, size_t payloadLen);

    SystemModule* systemModule_ = nullptr;
    ControlModule* controlModule_ = nullptr;
    uint32_t lastPresetsRev_ = 0;   ///< last preset revision announced to HA (tick1s re-announce)
    char lastLook_[32] = "";        ///< the look last published in ha/state (part of the change gate)

    // The topic prefix is DERIVED from a STABLE hardware id: projectMM/<last6-of-MAC>. Not stored (no
    // buffer). The MAC is fixed for the chip's life, so a device rename never changes the topics —
    // external integrations stay pinned (the WLED/Tasmota/HA convention). The friendly display name
    // is a separate concern, published on the retained `name` topic (publishName). buildTopic writes
    // <prefix>/<suffix>.
    // constexpr, not a `const char*` member: the compiler can then prove the topic-buffer bounds
    // downstream (GCC otherwise warns that "%s/%s" *may* truncate, because a runtime pointer could
    // point at anything). The root never varies at runtime, so saying so costs nothing.
    static constexpr const char* kPrefixRoot = "projectMM";
    // "projectMM" + '/' + 6 hex + NUL. Sized from the parts, so a longer root cannot silently truncate.
    static constexpr size_t kPrefixLen = 9 + 1 + 6 + 1;
    void topicPrefix(char* out, size_t cap) const;
    void buildTopic(char* out, size_t cap, const char* suffix) const;

    // Controls (persisted).
    char     broker_[64]   = "";          // hostname or IP of the broker
    uint16_t port_         = 1883;
    char     username_[48] = "";
    char     password_[48] = "";
    bool     haDiscovery_  = false;       // announce a HA MQTT-discovery light (opt-in); see publishDiscovery
    char     statusStr_[64] = "disabled";

    platform::TcpConnection conn_;
    MqttInboundParser parser_;
    Conn     state_ = Conn::Idle;
    uint32_t lastPingSent_   = 0;
    uint32_t lastActivity_   = 0;         // last inbound byte or successful send (for keepalive)
    uint32_t lastConnectTry_ = 0;         // backoff clock for reconnects
    uint32_t connectStartedMs_ = 0;       // when the current TCP-connect / CONNACK-wait began
    uint32_t nameSig_        = 0;          // signature of the last-published friendly name (rename detect)
    uint16_t nextPacketId_   = 1;

    // Last-published state, so publishState only emits on change.
    bool    lastOn_    = false;
    uint8_t lastBri_   = 0;
    uint8_t lastPalette_ = 0xFF;          // 0xFF = "never published" → force first publish
    bool    havePublished_ = false;

    bool     lastConnectFailed_ = false;  // widen the backoff after a failure (esp. a slow DNS lookup)

    // Discovery-config scratch — HEAP, allocated lazily only when discovery actually publishes
    // (connected + haDiscovery on), freed in release() and when discovery is turned off, per the
    // pay-for-what-you-use rule (architecture.md § Memory strategy): a device that has the MQTT module
    // but never enables HA discovery pays ZERO for it. Heap (not a fixed member) also keeps the ~360 B
    // config frame off the shared 8 KB main-task stack (the P4 registerType-stack lesson). Two regions
    // because buildMqttPublish needs payload + output separate: the JSON builds into discoveryPayload_,
    // the framed packet into discoveryBuf_.
    // The fixed part of the config; the effect_list adds to it and is measured, not capped (see
    // haEffectListBytes / ensureDiscoveryBuffers). A cap would either waste RAM on a device with
    // three presets or silently publish nothing on a device with many.
    static constexpr size_t kDiscoveryPayloadBase = 320;
    static constexpr size_t kDiscoveryBufBase     = 448;
    static_assert(kDiscoveryDynamicBytes == kDiscoveryPayloadBase + kDiscoveryBufBase,
                  "public test constant must track the actual buffer sizes");
    /// Bytes the effect_list needs for the CURRENT look-only presets, including the JSON key,
    /// quotes and separators. 0 when there is nothing to publish.
    size_t haEffectListBytes() const;
    /// Serialize the effect_list key into `out`; returns the bytes written (0 if there are no looks).
    size_t writeHaEffectList(char* out, size_t cap) const;
    size_t discoveryPayloadLen_ = 0;   ///< size the buffers were allocated at, so a change can resize
    size_t discoveryBufLen_     = 0;
    char*    discoveryPayload_ = nullptr;
    uint8_t* discoveryBuf_     = nullptr;
    bool ensureDiscoveryBuffers();   // lazily alloc both; false on OOM. Sets dynamicBytes.
    void freeDiscoveryBuffers();     // free both + dynamicBytes(0). Called on release / discovery-off.

    // Test-only outbound capture (null in production). sendPacket appends every emitted packet here.
    uint8_t* sendCapture_    = nullptr;
    size_t   sendCaptureCap_ = 0;
    size_t   sendCaptureLen_ = 0;

    static constexpr uint16_t kKeepaliveSec = 30;
    static constexpr uint32_t kReconnectBackoffMs = 5000;       // after a clean disconnect
    static constexpr uint32_t kFailedBackoffMs    = 30000;      // after a failed attempt — a bad
        // hostname re-runs a synchronous getaddrinfo each try (a brief tick stall), so back off harder
    static constexpr uint32_t kConnectTimeoutMs = 8000;   // overall TCP-connect + CONNACK-wait bound
};

} // namespace mm
