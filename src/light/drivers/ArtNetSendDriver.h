#pragma once

#include "light/drivers/Drivers.h"
#include "platform/platform.h"

#include <cstring>
#include <cstdint>

namespace mm {

class ArtNetSendDriver : public DriverBase {
public:
    char ip[16] = "192.168.1.70";
    uint16_t universeStart = 0;
    uint8_t fps = 50;

    void onBuildControls() override {
        controls_.addText("ip", ip);
        controls_.addUint16("universe_start", universeStart);
        controls_.addUint8("fps", fps, 1, 120);
    }

    void setup() override {
        socket_.open();
        // Bind the destination so each per-universe send skips the per-packet
        // address parse + route lookup. Re-bound in loop() if the ip control
        // changes (see connectIfIpChanged).
        connectIfIpChanged();
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

        // Re-bind the socket if the ip control was changed from the UI.
        connectIfIpChanged();

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
        if (correction_ && corrected_.data() && corrected_.count() >= nLights) {
            const uint8_t outCh = correction_->outChannels;
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

        // Send all universes in one burst — receiver expects a complete frame
        uint16_t universe = universeStart;

        size_t sent = 0;
        while (sent < totalBytes) {
            size_t chunkLen = totalBytes - sent;
            if (chunkLen > MAX_CHANNELS_PER_UNIVERSE) chunkLen = MAX_CHANNELS_PER_UNIVERSE;

            sendUniverse(universe, data + sent, static_cast<uint16_t>(chunkLen));

            sent += chunkLen;
            universe++;
        }

        sequence_++;
    }

    // Public for testability: builds an ArtNet OpDmx packet into outBuf.
    // Returns total packet size. outBuf must be at least ARTNET_HEADER_SIZE + dataLen.
    static size_t buildPacket(uint8_t* outBuf, uint16_t universe, uint8_t sequence,
                              const uint8_t* data, uint16_t dataLen) {
        // "Art-Net\0" header
        std::memcpy(outBuf, "Art-Net", 8); // includes null terminator

        // OpCode: OpDmx = 0x5000 (little-endian)
        outBuf[8] = 0x00;
        outBuf[9] = 0x50;

        // Protocol version: 14 (big-endian)
        outBuf[10] = 0x00;
        outBuf[11] = 0x0e;

        // Sequence
        outBuf[12] = sequence;

        // Physical port
        outBuf[13] = 0;

        // Universe (little-endian)
        outBuf[14] = static_cast<uint8_t>(universe & 0xFF);
        outBuf[15] = static_cast<uint8_t>(universe >> 8);

        // Length (big-endian)
        outBuf[16] = static_cast<uint8_t>(dataLen >> 8);
        outBuf[17] = static_cast<uint8_t>(dataLen & 0xFF);

        // DMX data
        std::memcpy(outBuf + ARTNET_HEADER_SIZE, data, dataLen);

        return ARTNET_HEADER_SIZE + dataLen;
    }

    static constexpr uint16_t ARTNET_PORT = 6454;
    static constexpr size_t MAX_CHANNELS_PER_UNIVERSE = 510; // 170 RGB lights
    static constexpr size_t ARTNET_HEADER_SIZE = 18;

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
    char lastConnectedIp_[16] = {};  // destination the socket is currently bound to

    // Re-bind the connected socket when the ip control differs from what it was
    // last bound to. UDP connect() only sets the destination (no handshake), so
    // this is cheap; it runs only on an actual change.
    void connectIfIpChanged() {
        if (std::strcmp(ip, lastConnectedIp_) == 0) return;
        socket_.connect(ip, ARTNET_PORT);
        std::strncpy(lastConnectedIp_, ip, sizeof(lastConnectedIp_) - 1);
        lastConnectedIp_[sizeof(lastConnectedIp_) - 1] = '\0';
    }

    void sendUniverse(uint16_t universe, const uint8_t* data, uint16_t dataLen) {
        uint8_t packet[ARTNET_HEADER_SIZE + MAX_CHANNELS_PER_UNIVERSE];
        size_t packetLen = buildPacket(packet, universe, sequence_, data, dataLen);
        socket_.sendTo(packet, packetLen);
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
