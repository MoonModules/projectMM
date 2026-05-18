#pragma once

#include "light/DriverBase.h"
#include "light/Pixel.h"
#include "platform/UdpSocket.h"
#include <cstdint>
#include <cstring>
#include "platform/Timing.h"
#include <thread>

namespace mm::light {

class ArtNetSendDriver : public DriverBase {
public:
    const char* name() const override { return "ArtNet Send"; }

    void addControls() override {
        destIPIdx_        = MoonModule::addControl("destIP", "192.168.1.70");
        startUniverseIdx_ = MoonModule::addControl("startUniverse", uint16_t(0), uint16_t(0), uint16_t(32767));
        fpsIdx_           = MoonModule::addControl("fps", uint16_t(50), uint16_t(1), uint16_t(120));
    }

    void setup() override {
        sock_ = platform::udpOpen();
        if (sock_ != platform::INVALID_SOCKET_HANDLE) {
            platform::udpEnableBroadcast(sock_);
        }
    }

    void teardown() override {
        platform::udpClose(sock_);
        sock_ = platform::INVALID_SOCKET_HANDLE;
    }

    void loop() override {
        if (sock_ == platform::INVALID_SOCKET_HANDLE) return;
        auto pixels = outputBuffer();
        if (pixels.empty()) return;

        // FPS limiting: skip this frame if too soon
        uint16_t fps = control(fpsIdx_)->u16.value;
        uint32_t frameMs = 1000 / (fps > 0 ? fps : 1);
        uint32_t now = platform::millis();
        if (now - lastSendMs_ < frameMs) return;
        lastSendMs_ = now;

        const char* destIP = control(destIPIdx_)->text.value;
        uint16_t startUni = control(startUniverseIdx_)->u16.value;

        size_t pixelsSent = 0;
        uint16_t universe = startUni;

        while (pixelsSent < pixels.size()) {
            size_t remaining = pixels.size() - pixelsSent;
            size_t pixelsThisUni = (remaining > PIXELS_PER_UNIVERSE)
                ? PIXELS_PER_UNIVERSE : remaining;
            uint16_t dataLen = static_cast<uint16_t>(pixelsThisUni * 3);

            buildPacket(packet_, universe, dataLen,
                        &pixels[pixelsSent], pixelsThisUni);

            platform::udpSend(sock_, destIP, ARTNET_PORT,
                              packet_, HEADER_SIZE + dataLen);

            pixelsSent += pixelsThisUni;
            ++universe;

            // Pace packets to avoid overwhelming the receiver
            // ~50us per packet allows ~20K packets/sec
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        ++sequence_;
    }

    // Exposed for testing — builds an ArtNet DMX packet without sending
    static void buildPacket(uint8_t* packet, uint16_t universe,
                            uint16_t dataLen, const RGB* pixels,
                            size_t pixelCount) {
        std::memcpy(packet, "Art-Net", 8);
        packet[8] = 0x00;  // OpCode low (OpDmx = 0x5000)
        packet[9] = 0x50;  // OpCode high
        packet[10] = 0x00; // ProtVer high
        packet[11] = 14;   // ProtVer low
        packet[12] = 0;    // Sequence
        packet[13] = 0;    // Physical
        packet[14] = static_cast<uint8_t>(universe & 0xFF);
        packet[15] = static_cast<uint8_t>(universe >> 8);
        packet[16] = static_cast<uint8_t>(dataLen >> 8);   // Length high
        packet[17] = static_cast<uint8_t>(dataLen & 0xFF);  // Length low
        for (size_t i = 0; i < pixelCount; ++i) {
            packet[HEADER_SIZE + i * 3]     = pixels[i].r;
            packet[HEADER_SIZE + i * 3 + 1] = pixels[i].g;
            packet[HEADER_SIZE + i * 3 + 2] = pixels[i].b;
        }
    }

    static constexpr size_t PIXELS_PER_UNIVERSE = 170;
    static constexpr size_t HEADER_SIZE = 18;
    static constexpr uint16_t ARTNET_PORT = 6454;

private:
    platform::UdpSocketHandle sock_ = platform::INVALID_SOCKET_HANDLE;
    uint8_t sequence_ = 0;
    uint8_t packet_[HEADER_SIZE + 512] = {};
    uint8_t destIPIdx_ = 0;
    uint8_t startUniverseIdx_ = 0;
    uint8_t fpsIdx_ = 0;
    uint32_t lastSendMs_ = 0;
};

} // namespace mm::light
