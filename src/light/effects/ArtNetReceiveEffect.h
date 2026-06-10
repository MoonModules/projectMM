#pragma once

#include "light/ArtNetPacket.h"   // shared OpDmx wire format (build + parse)
#include "light/layers/Layer.h"
#include "platform/platform.h"

#include <cstring>

namespace mm {

// ArtNet OpDmx receiver as an EFFECT: external light data is just another
// module that writes into the layer buffer, composable with modifiers and
// blending like any generated effect. The end-to-end pair with
// ArtNetSendDriver — a desktop build on a PC drives this device's lights.
//
// The layer clears its buffer at the start of every tick, so packets are
// drained into an owned STAGING buffer and staging is copied to the layer
// buffer each tick — hold-last-frame semantics; without it the lights would
// strobe black between ArtNet frames. The drain is non-blocking and bounded
// per tick (network input is synchronous at the frame boundary — the
// architecture.md rule), so a packet flood can't wedge the render loop.
// The ArtNet sequence field is ignored: last write wins into staging.
//
// Prior art: MoonLight's D_NetworkIn and projectMM v1's ArtNetInModule.
class ArtNetReceiveEffect : public EffectBase {
public:
    const char* tags() const override { return "📡🌙"; }  // network input · MoonLight / v1 lineage

    uint16_t universeStart = 0;       // mirrors the sender's universe_start
    uint16_t port = ARTNET_PORT;      // 6454, the Art-Net standard port

    void onBuildControls() override {
        controls_.addUint16("universe_start", universeStart);
        controls_.addUint16("port", port);
    }

    void setup() override { openAndBind(); }

    void teardown() override {
        socket_.close();
        boundPort_ = 0;
        releaseStaging();
        setDynamicBytes(0);
        clearStatus();
    }

    ~ArtNetReceiveEffect() override { releaseStaging(); }

    // Rebind live on a port change (cheap — close + bind), the same tier as
    // ArtNet send's connectIfIpChanged. Runs off the render loop, in the
    // HTTP/API handler context.
    void onUpdate(const char* name) override {
        if (std::strcmp(name, "port") == 0 && port != boundPort_) {
            socket_.close();
            openAndBind();
        }
    }

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
        // Bounded non-blocking drain: 128 packets ≈ one full frame for ~21k RGB
        // lights; a flood costs at most 128 recvfrom calls per tick, then the
        // rest waits in the socket buffer for the next tick.
        for (int i = 0; i < kMaxPacketsPerTick; i++) {
            const int n = socket_.recvFrom(pkt_, sizeof(pkt_));
            if (n <= 0) break;
            uint16_t universe = 0, dataLen = 0;
            const uint8_t* data = nullptr;
            if (parseArtDmxPacket(pkt_, static_cast<size_t>(n), universe, data, dataLen)) {
                applyDmx(universe, data, dataLen);
            }
        }
        // Staging → layer buffer (the layer cleared it at tick start).
        uint8_t* buf = buffer();
        if (!buf) return;
        const size_t bufBytes = static_cast<size_t>(nrOfLights()) * channelsPerLight();
        std::memcpy(buf, staging_, stagingBytes_ < bufBytes ? stagingBytes_ : bufBytes);
    }

    // Place one universe's payload into staging at the same 510-byte split the
    // sender uses: byte offset (universe − universeStart) × 510, clamped to the
    // buffer; universes below the start or beyond the buffer are ignored.
    // Public for testability (the buildArtDmxPacket precedent) — the placement
    // and clamping are pinned without sockets.
    void applyDmx(uint16_t universe, const uint8_t* data, uint16_t len) {
        if (!staging_ || universe < universeStart) return;
        const size_t offset =
            static_cast<size_t>(universe - universeStart) * MAX_CHANNELS_PER_UNIVERSE;
        if (offset >= stagingBytes_) return;
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

    platform::UdpSocket socket_;
    uint16_t boundPort_ = 0;                     // 0 = not bound
    uint8_t pkt_[ARTNET_HEADER_SIZE + 512] = {}; // one datagram; DMX max 512 channels
    uint8_t* staging_ = nullptr;                 // owned; layer-buffer-sized
    size_t stagingBytes_ = 0;

    void openAndBind() {
        if (socket_.open() && socket_.bind(port)) {
            boundPort_ = port;
            if (status() == kBindFailMsg) clearStatus();
        } else {
            boundPort_ = 0;
            setStatus(kBindFailMsg, Severity::Error);
        }
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
