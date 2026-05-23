#pragma once

#include "light/drivers/DriverGroup.h"
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

        // Send all universes in one burst — receiver expects a complete frame
        const uint8_t* data = sourceBuffer_->data();
        size_t totalBytes = sourceBuffer_->bytes();
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

private:
    platform::UdpSocket socket_;
    Buffer* sourceBuffer_ = nullptr;
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
};

} // namespace mm
