#pragma once

#include "light/ArtNetPacket.h"   // shared ArtNet wire formats (build + parse)
#include "light/DdpPacket.h"      // shared DDP wire format
#include "light/E131Packet.h"     // shared E1.31/sACN wire format
#include "light/drivers/DriverBase.h"
#include "platform/platform.h"

#include <algorithm>  // std::min in the chunk loop
#include <cstdint>
#include <cstring>

namespace mm {

/// Output driver: streams the buffer over UDP — one driver, three industry protocols selected by a
/// control. The single-node-multiple-protocols shape follows MoonLight's D_NetworkOut (architecture
/// studied, not copied). Byte layouts live in ArtNetPacket.h / E131Packet.h / DdpPacket.h, shared
/// with the receiver so the two sides cannot drift.
///
/// **Interop:** unicast or limited-broadcast `255.255.255.255` (default, `SO_BROADCAST`) — NOT
/// multicast (no IGMP join; MoonLight ships without it too). E1.31 framing: CID stable per device
/// (from the MAC), source name `projectMM`, priority 100, one frame-level sequence per frame.
///
/// **Synchronous send:** the whole frame goes out inline in loop() (~35 ms Ethernet / ~90 ms WiFi at
/// 128×128 ArtNet; DDP less). A decoupling send task is a PSRAM-gated backlog item. Added per board
/// via the catalog like the LED drivers; applies the same shared Correction, so network and wired
/// outputs show identical colours.
/// @card NetworkSendDriver.png
class NetworkSendDriver : public DriverBase {
public:
    /// Protocol names, index-aligned with the constants used in loop()'s switch (0 = ArtNet,
    /// 1 = E1.31, 2 = DDP). The destination port follows the protocol (6454 / 5568 / 4048) — see
    /// connectIfDestChanged().
    static constexpr const char* kProtocolOptions[] = {"ArtNet", "E1.31", "DDP"};
    static constexpr uint8_t kProtocolCount = 3;

    /// Destination address as 4 octets — defaults to limited-broadcast so a fresh sender reaches
    /// every receiver on the LAN with no IP to type; set a unicast IP in the UI to target one
    /// device. Broadcast needs SO_BROADCAST, which platform UdpSocket::open sets. Stored as 4 bytes
    /// (not a dotted-quad string), per docs/coding-standards.md § store values in their native
    /// shape; UdpSocket::connect() takes a string, so connectIfDestChanged() formats on a stack
    /// buffer at the boundary.
    uint8_t ip[4] = {255, 255, 255, 255};
    /// Wire protocol (index into kProtocolOptions). Selects both the packet layout and the chunking:
    /// ArtNet / E1.31 split at 510 channels per universe (whole RGB lights, the xLights/Falcon
    /// convention; 170 lights/packet), consecutive universes from `universeStart`; DDP uses
    /// 1440-byte byte-offset chunks (480 lights/packet) and is the fast path — per-packet cost
    /// dominates wire time, so a 128×128 WiFi frame drops from ~110 ms (ArtNet) to ~40 ms.
    uint8_t protocol = 0;
    /// First universe the slice maps onto (ArtNet / E1.31; DDP is byte-addressed). Emitted verbatim,
    /// no hidden 1-based adjust: buffer offset = `(universe − universeStart) × 510`. Strict sACN
    /// reserves universe 0, so set ≥ 1 on BOTH ends for it; our own receiver defaults to 0 so
    /// device↔device pairs align out of the box. Orthogonal to the DriverBase window (start/count),
    /// which picks WHICH buffer slice is sent — this picks which universe it lands on.
    uint16_t universeStart = 0;
    /// Send-rate ceiling (Hz); loop() rate-limits to this so a fast render tick doesn't flood the LAN.
    uint8_t fps = 50;

    /// Register the controls in UI order: protocol, destination IP, universe offset, the shared
    /// window (start/count), then the rate cap.
    void onBuildControls() override {
        controls_.addSelect("protocol", protocol, kProtocolOptions, kProtocolCount);
        controls_.addIPv4("ip", ip);
        controls_.addUint16("universe_start", universeStart);
        addWindowControls();   // start / count — the slice of the shared buffer this sink sends
        controls_.addUint8("fps", fps, 1, 120);
    }

    /// A start/count change resizes the window this sink sends; route it through the onBuildState
    /// sweep so resizeCorrected() re-sizes corrected_ for the new slice — otherwise growing the
    /// window past the old corrected_ silently drops to passthrough.
    bool controlChangeTriggersBuildState(const char* name) const override {
        return isWindowControl(name);
    }

    /// Open the socket and derive the stable E1.31 component id (CID) from the MAC once — no UUID
    /// machinery needed for a deterministic, unique-enough id — then bind the destination so each
    /// per-packet send skips the address parse + route lookup (re-bound in loop() on an ip/protocol
    /// change; see connectIfDestChanged).
    void setup() override {
        socket_.open();
        std::memcpy(cid_, "projectMM\0", 10);
        platform::getMacAddress(cid_ + 10);
        connectIfDestChanged();
    }

    /// Close the socket on teardown; DriverBase::teardown (via the base) clears any status.
    void teardown() override {
        socket_.close();
    }

    /// Take the shared source buffer and (re)size the corrected_ buffer for it. Called from
    /// Drivers::passBufferToDrivers inside onBuildState (and once at setup); resizeCorrected() is a
    /// no-op while correction_ is still null on the first call, and the second call (after
    /// setCorrection) lands the actual allocation. All off the hot path.
    void setSourceBuffer(Buffer* buf) override {
        sourceBuffer_ = buf;
        resizeCorrected();
    }

    /// Take the shared output correction and re-size corrected_ to its channel count.
    void setCorrection(const Correction* c) override {
        correction_ = c;
        resizeCorrected();
    }

    /// Topology change (light count, channels per light, or LUT path swap) — the framework calls
    /// this after Layer/Drivers reshape. Resize off the hot path so loop() never allocates.
    void onBuildState() override {
        resizeCorrected();
        MoonModule::onBuildState();
    }

    /// Preset toggle (RGB↔RGBW) changes correction_->outChannels without a structural rebuild;
    /// Drivers::onUpdate forwards this hook so corrected_ tracks the new channel count.
    void onCorrectionChanged() override {
        resizeCorrected();
    }

    /// Rate-limit to `fps`, apply the shared correction into corrected_ (passthrough if unwired),
    /// then chunk the window slice into protocol packets and send the whole frame inline.
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
        // Send this sink's window slice [start, start+count) only (count 0 = the
        // whole buffer from start), so it covers just its lights — and a frame
        // isn't packed/sent for lights it doesn't own. winStart is the first light.
        nrOfLightsType winStart, nLights;
        windowSlice(sourceBuffer_->count(), winStart, nLights);
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
                // Read the windowed light (slice starts at winStart); pack densely.
                correction_->apply(src + (winStart + i) * srcCh, dst + i * outCh, srcCh);
            }
            data = dst;
            totalBytes = static_cast<size_t>(nLights) * outCh;
        } else {
            // Passthrough (no correction): honour the same window as the corrected
            // path — point at the slice start so a sliced sink sends only its lights.
            const uint8_t srcCh = sourceBuffer_->channelsPerLight();
            data = sourceBuffer_->data() + static_cast<size_t>(winStart) * srcCh;
            totalBytes = static_cast<size_t>(nLights) * srcCh;
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
        // Size for the window slice this sender actually transmits, not the whole
        // frame — a sink covering 64 of a 16K-light buffer reserves 64. The same
        // windowSlice() the send loop uses, so the buffers stay in lock-step.
        nrOfLightsType winStart, n;
        windowSlice(sourceBuffer_->count(), winStart, n);
        const uint8_t ch = correction_->outChannels;
        if (n == 0 || ch == 0) return;
        if (corrected_.count() >= n && corrected_.channelsPerLight() >= ch) return;
        corrected_.allocate(n, ch);
    }
};

} // namespace mm
