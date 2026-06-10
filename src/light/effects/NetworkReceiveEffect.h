#pragma once

#include "light/ArtNetPacket.h"   // shared ArtNet wire formats (build + parse)
#include "light/DdpPacket.h"      // shared DDP wire format
#include "light/E131Packet.h"     // shared E1.31/sACN wire format
#include "light/layers/Layer.h"
#include "platform/platform.h"

#include <cstring>

namespace mm {

// Lights-over-UDP receiver as an EFFECT: external light data is just another
// module that writes into the layer buffer, composable with modifiers and
// blending like any generated effect. The end-to-end pair with
// NetworkSendDriver — and the receive side for industry senders (Resolume,
// Madrix, xLights, LedFx, …).
//
// All three protocols are received AT ONCE: the effect binds the three
// well-known ports (ArtNet 6454, E1.31 5568, DDP 4048) and validates each
// packet against its port's wire format — WLED's multi-port pattern. There is
// deliberately no protocol control: whatever a sender speaks just works, and
// the status field shows what is being received.
//
// ArtNet discovery: controllers (Resolume's Advanced Output, Madrix, xLights)
// find nodes by broadcasting ArtPoll; this effect answers with ArtPollReply so
// the device appears in their node lists instead of needing manual IP entry.
//
// The layer clears its buffer at the start of every tick, so packets are
// drained into an owned STAGING buffer and staging is copied to the layer
// buffer each tick — hold-last-frame semantics; without it the lights would
// strobe black between frames. The drain is non-blocking and bounded per tick
// (network input is synchronous at the frame boundary — the architecture.md
// rule), so a packet flood can't wedge the render loop. Sequence fields (and
// DDP's push flag) are ignored: last write wins into staging.
//
// Prior art: MoonLight's D_NetworkIn (single node, three protocols), WLED's
// realtime UDP input (multi-port + per-packet validation, ArtPollReply), and
// projectMM v1's ArtNetInModule.
class NetworkReceiveEffect : public EffectBase {
public:
    const char* tags() const override { return "📡🌙"; }  // network input · MoonLight / v1 lineage

    uint16_t universeStart = 0;        // mirrors the sender's universe_start (ArtNet/E1.31)
    // Bytes each universe maps to in the buffer. 510 = whole RGB lights per
    // universe (the xLights/Falcon convention and our sender's split); set 512
    // for senders that pack pixels across universe boundaries (Madrix-style).
    // Also clamps the copied payload, so a 512-channel frame from a 510-packed
    // source can't bleed its 2 padding bytes into the next universe's data.
    uint16_t channelsPerUniverse = static_cast<uint16_t>(MAX_CHANNELS_PER_UNIVERSE);

    void onBuildControls() override {
        controls_.addUint16("universe_start", universeStart);
        controls_.addUint16("channels_per_universe", channelsPerUniverse);
    }

    void setup() override {
        // One socket per protocol port, all listening at once. Each bind is
        // attempted independently so one taken port can't stop the others from
        // draining; any failure is reported once.
        const bool artnetOk = artnetSocket_.open() && artnetSocket_.bind(ARTNET_PORT);
        const bool e131Ok = e131Socket_.open() && e131Socket_.bind(E131_PORT);
        const bool ddpOk = ddpSocket_.open() && ddpSocket_.bind(DDP_PORT);
        if (artnetOk && e131Ok && ddpOk) {
            if (status() == kBindFailMsg) clearStatus();
        } else {
            setStatus(kBindFailMsg, Severity::Error);
        }
    }

    void teardown() override {
        artnetSocket_.close();
        e131Socket_.close();
        ddpSocket_.close();
        releaseStaging();
        setDynamicBytes(0);
        clearStatus();
    }

    ~NetworkReceiveEffect() override { releaseStaging(); }

    // Size staging to the layer buffer (one byte per channel byte), off the
    // hot path — the GameOfLifeEffect resource shape. Zeroed on (re)alloc so a
    // fresh grid starts dark, not with stale bytes.
    void onBuildState() override {
        const size_t need = static_cast<size_t>(nrOfLights()) * channelsPerLight();
        if (enabled() && need > 0) {
            if (need != stagingBytes_) {
                releaseStaging();
                staging_ = static_cast<uint8_t*>(platform::alloc(need));
                if (staging_) {
                    stagingBytes_ = need;
                    std::memset(staging_, 0, need);
                }
            }
        } else {
            releaseStaging();
        }
        setDynamicBytes(stagingBytes_);
    }

    void loop() override {
        if (!staging_) return;
        // Bounded non-blocking drain per socket: 128 packets ≈ one full ArtNet
        // frame for ~21k RGB lights; a flood costs at most 3×128 recvfrom calls
        // per tick, then the rest waits in the socket buffers for the next tick.
        uint16_t universe = 0, dataLen = 0;
        uint32_t byteOffset = 0;
        const uint8_t* data = nullptr;
        uint8_t srcIp[4];
        for (int i = 0; i < kMaxPacketsPerTick; i++) {
            const int n = artnetSocket_.recvFrom(pkt_, sizeof(pkt_), srcIp);
            if (n <= 0) break;
            if (parseArtDmxPacket(pkt_, static_cast<size_t>(n), universe, data, dataLen)) {
                applyDmx(universe, data, dataLen);
                noteReceiving(kStatusArtnet);
            } else if (isArtPoll(pkt_, static_cast<size_t>(n))) {
                replyToPoll(srcIp);   // make the device show up in controller node lists
            }
        }
        for (int i = 0; i < kMaxPacketsPerTick; i++) {
            const int n = e131Socket_.recvFrom(pkt_, sizeof(pkt_));
            if (n <= 0) break;
            if (parseE131Packet(pkt_, static_cast<size_t>(n), universe, data, dataLen)) {
                applyDmx(universe, data, dataLen);
                noteReceiving(kStatusE131);
            }
        }
        for (int i = 0; i < kMaxPacketsPerTick; i++) {
            const int n = ddpSocket_.recvFrom(pkt_, sizeof(pkt_));
            if (n <= 0) break;
            if (parseDdpPacket(pkt_, static_cast<size_t>(n), byteOffset, data, dataLen)) {
                applyBytes(byteOffset, data, dataLen);
                noteReceiving(kStatusDdp);
            }
        }
        // Staging → layer buffer (the layer cleared it at tick start).
        uint8_t* buf = buffer();
        if (!buf) return;
        const size_t bufBytes = static_cast<size_t>(nrOfLights()) * channelsPerLight();
        std::memcpy(buf, staging_, stagingBytes_ < bufBytes ? stagingBytes_ : bufBytes);
    }

    // Place one universe's payload: byte offset (universe − universeStart) ×
    // channels_per_universe, payload clamped to one universe's stride so a
    // 512-channel frame can't bleed past its slot. Universes below the start or
    // beyond the buffer are ignored. Shared by ArtNet and E1.31; DDP skips the
    // universe math and calls applyBytes directly. Public for testability (the
    // buildArtDmxPacket precedent).
    void applyDmx(uint16_t universe, const uint8_t* data, uint16_t len) {
        if (universe < universeStart || channelsPerUniverse == 0) return;
        if (len > channelsPerUniverse) len = channelsPerUniverse;
        applyBytes(static_cast<size_t>(universe - universeStart) * channelsPerUniverse,
                   data, len);
    }

    // The one clamped write into staging (DDP's native addressing). The bound
    // check runs BEFORE any addition so a hostile 32-bit offset can't overflow
    // past it.
    void applyBytes(size_t offset, const uint8_t* data, uint16_t len) {
        if (!staging_ || offset >= stagingBytes_) return;
        size_t n = len;
        if (offset + n > stagingBytes_) n = stagingBytes_ - offset;
        std::memcpy(staging_ + offset, data, n);
    }

    // Test-only accessors — let the unit tests pin the staging lifecycle
    // (sized off the hot path, never reallocated by loop, freed on teardown).
    const uint8_t* stagingData() const { return staging_; }
    size_t stagingBytes() const { return stagingBytes_; }

private:
    static constexpr int kMaxPacketsPerTick = 128;
    static constexpr const char* kBindFailMsg = "UDP bind failed — port in use?";
    static constexpr const char* kStatusArtnet = "receiving Art-Net";
    static constexpr const char* kStatusE131 = "receiving E1.31";
    static constexpr const char* kStatusDdp = "receiving DDP";

    platform::UdpSocket artnetSocket_;
    platform::UdpSocket e131Socket_;
    platform::UdpSocket ddpSocket_;
    uint8_t pkt_[1500] = {};       // one datagram, any protocol (DDP max 1450)
    uint8_t* staging_ = nullptr;   // owned; layer-buffer-sized
    size_t stagingBytes_ = 0;

    // Swap the "receiving <protocol>" diagnostic — but never clobber a bind
    // error. Pointer compares only (all four strings are static literals).
    void noteReceiving(const char* lit) {
        const char* s = status();
        if (s == nullptr || s == kStatusArtnet || s == kStatusE131 || s == kStatusDdp) {
            if (s != lit) setStatus(lit, Severity::Status);
        }
    }

    // Answer an ArtPoll with our IP/MAC/name so controllers list the device.
    // Runs at most once per poll (controllers poll every few seconds) — the
    // 239-byte reply lives on the stack, no allocation.
    void replyToPoll(const uint8_t pollerIp[4]) {
        char ipStr[16] = {};
        platform::ethGetIP(ipStr, sizeof(ipStr));
        if (!ipStr[0]) platform::wifiStaGetIP(ipStr, sizeof(ipStr));
        if (!ipStr[0]) std::strncpy(ipStr, platform::hostIp(), sizeof(ipStr) - 1);
        uint8_t myIp[4];
        if (!parseDottedQuad(ipStr, myIp)) return;   // no usable IP — stay silent
        uint8_t mac[6];
        platform::getMacAddress(mac);
        uint8_t reply[ARTNET_POLL_REPLY_SIZE];
        buildArtPollReply(reply, myIp, mac, "projectMM", "projectMM NetworkReceive",
                          universeStart);
        artnetSocket_.sendToAddr(pollerIp, ARTNET_PORT, reply, sizeof(reply));
    }

    void releaseStaging() {
        if (staging_) {
            platform::free(staging_);
            staging_ = nullptr;
            stagingBytes_ = 0;
        }
    }
};

} // namespace mm
