#pragma once

#include "light/ArtNetPacket.h"   // shared ArtNet wire formats (build + parse)
#include "light/DdpPacket.h"      // shared DDP wire format
#include "light/E131Packet.h"     // shared E1.31/sACN wire format
#include "light/drivers/Drivers.h"
#include "platform/platform.h"

#include <algorithm>  // std::min in the chunk loop
#include <cstdint>
#include <cstring>

namespace mm {

// Lights-over-UDP output: one driver, three industry protocols selected by a
// control — ArtNet (510-channel universes), E1.31/sACN (the same universe
// split, ACN framing), and DDP (1440-byte byte-offset packets — the fast path:
// 480 RGB lights per packet vs 170, and per-packet cost is what dominates the
// wire time). The single-node-multiple-protocols shape follows MoonLight's
// D_NetworkOut (architecture studied, not copied).
class NetworkSendDriver : public DriverBase {
public:
    // Index-aligned with the protocol constants used in loop()'s switch:
    // 0 = ArtNet, 1 = E1.31, 2 = DDP. The destination port follows the
    // protocol (6454 / 5568 / 4048) — see connectIfDestChanged().
    static constexpr const char* kProtocolOptions[] = {"ArtNet", "E1.31", "DDP"};
    static constexpr uint8_t kProtocolCount = 3;

    // Destination address as 4 octets (not a dotted-quad string) — 4 bytes
    // vs char[16], per docs/coding-standards.md § Prefer integers, store
    // values in their native shape. The platform UdpSocket::connect() takes
    // a string, so connectIfDestChanged() formats on a stack buffer at the
    // boundary — the long-lived storage stays integer.
    // Default to the limited-broadcast address so a fresh sender reaches every
    // receiver on the LAN with no IP to type — set a unicast IP in the UI to target
    // one device. Broadcast needs SO_BROADCAST, which platform UdpSocket::open sets.
    uint8_t ip[4] = {255, 255, 255, 255};
    uint8_t protocol = 0;        // index into kProtocolOptions
    uint16_t universeStart = 0;  // first universe (ArtNet/E1.31; DDP is byte-addressed)
    uint8_t fps = 50;

    void onBuildControls() override {
        controls_.addSelect("protocol", protocol, kProtocolOptions, kProtocolCount);
        controls_.addIPv4("ip", ip);
        controls_.addUint16("universe_start", universeStart);
        controls_.addUint8("fps", fps, 1, 120);
    }

    void setup() override {
        socket_.open();
        // E1.31 wants a stable per-device component id; derive it from the MAC
        // once — no UUID machinery needed for a deterministic, unique-enough CID.
        std::memcpy(cid_, "projectMM\0", 10);
        platform::getMacAddress(cid_ + 10);
        // Bind the destination so each per-packet send skips the per-packet
        // address parse + route lookup. Re-bound in loop() if the ip or
        // protocol control changes (see connectIfDestChanged).
        connectIfDestChanged();
    }

    void teardown() override {
        socket_.close();
    }

    void setSourceBuffer(Buffer* buf) override {
        sourceBuffer_ = buf;
        // setSourceBuffer / setCorrection / setLayer are all called from
        // Drivers::passBufferToDrivers, which runs inside Drivers::onBuildState
        // (and once at setup). resizeCorrected() is a no-op while correction_
        // is still null on the first call; the second call (after setCorrection)
        // lands the actual allocation. All off the hot path.
        resizeCorrected();
    }

    void setCorrection(const Correction* c) override {
        correction_ = c;
        resizeCorrected();
    }

    // Topology change (light count, channels per light, or LUT path swap) — the
    // framework calls onBuildState after Layer/Drivers reshape. Resize off the
    // hot path so loop() never allocates.
    void onBuildState() override {
        resizeCorrected();
        MoonModule::onBuildState();
    }

    // Preset toggle (RGB↔RGBW) changes correction_->outChannels without
    // triggering a structural rebuild. Drivers::onUpdate forwards this hook.
    void onCorrectionChanged() override {
        resizeCorrected();
    }

    void loop() override {
        if (!sourceBuffer_ || !sourceBuffer_->data()) return;

        // FPS limiting
        if (fps == 0) return;
        uint32_t now = platform::millis();
        uint32_t interval = 1000 / fps;
        if (now - lastSendTime_ < interval) return;
        lastSendTime_ = now;

        // Re-bind the socket if the ip or protocol control changed from the UI.
        connectIfDestChanged();

        // Apply output correction (brightness / channel order / RGBW white) into the
        // pre-sized corrected_ buffer, then send that. Pure reader — sizing happens
        // in resizeCorrected() off the hot path (onBuildState / onCorrectionChanged
        // / setSourceBuffer / setCorrection). If correction isn't wired (e.g. a unit
        // test constructs the driver outside a Drivers parent) or its buffer doesn't
        // match the source size, fall back to passthrough — same degradation the
        // earlier in-loop allocate had if the allocation itself failed.
        const uint8_t* data;
        size_t totalBytes;
        const nrOfLightsType nLights = sourceBuffer_->count();
        // Three guards before applying correction: (a) correction wired,
        // (b) corrected_ has the row count we need, (c) corrected_'s
        // per-light stride is at least outChannels — otherwise dst + i *
        // outCh would overrun the allocation. Falls back to passthrough
        // when any guard fails (same degradation the old in-loop allocate
        // had on allocation failure). resizeCorrected() should keep
        // corrected_'s stride in sync with outChannels off the hot path,
        // but the hot-path check stays defensive — a stale corrected_
        // (e.g. correction_ swapped without onCorrectionChanged firing)
        // should miss the apply, not corrupt memory.
        const uint8_t outCh = correction_ ? correction_->outChannels : 0;
        if (correction_ && corrected_.data()
            && corrected_.count() >= nLights
            && corrected_.channelsPerLight() >= outCh) {
            const uint8_t* src = sourceBuffer_->data();
            const uint8_t srcCh = sourceBuffer_->channelsPerLight();
            uint8_t* dst = corrected_.data();
            for (nrOfLightsType i = 0; i < nLights; i++) {
                correction_->apply(src + i * srcCh, dst + i * outCh);
            }
            data = dst;
            totalBytes = static_cast<size_t>(nLights) * outCh;
        } else {
            data = sourceBuffer_->data();
            totalBytes = sourceBuffer_->bytes();
        }

        // Send the whole frame in one burst — receivers expect a complete
        // frame. The chunking is the only per-protocol difference: ArtNet and
        // E1.31 split into 510-channel universes (whole RGB lights, the
        // xLights/Falcon convention); DDP packs 1440-byte chunks addressed by
        // byte offset, push-flagged on the last packet of the frame.
        const size_t chunk = (protocol == 2) ? DDP_MAX_PAYLOAD : MAX_CHANNELS_PER_UNIVERSE;
        uint16_t universe = universeStart;
        uint8_t packet[DDP_HEADER_SIZE + DDP_MAX_PAYLOAD];  // 1450 B covers all three
        size_t sent = 0;
        while (sent < totalBytes) {
            const size_t n = std::min(totalBytes - sent, chunk);
            size_t packetLen;
            switch (protocol) {
                case 1:
                    packetLen = buildE131Packet(packet, universe, sequence_, cid_,
                                                data + sent, static_cast<uint16_t>(n));
                    break;
                case 2:
                    packetLen = buildDdpPacket(packet, static_cast<uint32_t>(sent),
                                               /*push=*/sent + n >= totalBytes,
                                               data + sent, static_cast<uint16_t>(n));
                    break;
                default:
                    packetLen = buildArtDmxPacket(packet, universe, sequence_,
                                                  data + sent, static_cast<uint16_t>(n));
                    break;
            }
            socket_.sendTo(packet, packetLen);
            sent += n;
            universe++;
        }

        sequence_++;
    }

    // The packet builds, the constants, and the inverse parses live in
    // light/ArtNetPacket.h, light/E131Packet.h and light/DdpPacket.h, shared
    // with NetworkReceiveEffect — each wire format exists in exactly one place.

    // Test-only accessor for the correction-applied buffer. Lets the unit
    // tests pin the no-allocation-in-loop contract (size set in onBuildState
    // / onCorrectionChanged, never in loop). Not part of any runtime API.
    const Buffer& correctedBuffer() const { return corrected_; }

private:
    platform::UdpSocket socket_;
    Buffer* sourceBuffer_ = nullptr;
    const Correction* correction_ = nullptr;
    Buffer corrected_;               // owned: source bytes after brightness/order/white
    uint8_t sequence_ = 0;
    uint32_t lastSendTime_ = 0;
    uint8_t cid_[E131_CID_LENGTH] = {};  // E1.31 component id, built once in setup()
    uint8_t lastConnectedIp_[4] = {};    // destination the socket is currently bound to
    uint8_t lastConnectedProtocol_ = 0xFF;  // 0xFF = never connected

    static uint16_t protocolPort(uint8_t p) {
        return p == 1 ? E131_PORT : p == 2 ? DDP_PORT : ARTNET_PORT;
    }

    // Re-bind the connected socket when the ip or protocol control differs
    // from what it was last bound to (the port follows the protocol). UDP
    // connect() only sets the destination (no handshake), so this is cheap; it
    // runs only on an actual change. The platform UdpSocket::connect() takes a
    // string IP, so we format the octets onto a stack buffer at the boundary
    // rather than holding a long-lived char[16] member.
    void connectIfDestChanged() {
        if (std::memcmp(ip, lastConnectedIp_, 4) == 0
            && protocol == lastConnectedProtocol_) return;
        char ipStr[16];
        formatDottedQuad(ipStr, ip);
        socket_.connect(ipStr, protocolPort(protocol));
        std::memcpy(lastConnectedIp_, ip, 4);
        lastConnectedProtocol_ = protocol;
    }

    // Called off the hot path (onBuildState, onCorrectionChanged, setters) to
    // make sure corrected_ is sized for the current source + correction. Skips
    // when nothing is wired yet, or when the existing allocation already fits.
    void resizeCorrected() {
        if (!correction_ || !sourceBuffer_) return;
        const nrOfLightsType n = sourceBuffer_->count();
        const uint8_t ch = correction_->outChannels;
        if (n == 0 || ch == 0) return;
        if (corrected_.count() >= n && corrected_.channelsPerLight() >= ch) return;
        corrected_.allocate(n, ch);
    }
};

} // namespace mm
