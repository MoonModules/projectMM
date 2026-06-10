#pragma once

#include <cstdint>
#include <cstring>

namespace mm {

// ArtNet OpDmx wire format — the one place the packet layout lives. The sender
// (drivers/ArtNetSendDriver.h) builds packets with it, the receiver
// (effects/ArtNetReceiveEffect.h) parses them with it; a unit test round-trips
// build→parse so the two can never drift apart. Sits at the top of src/light/
// (beside light_types.h) because the protocol is neutral between the drivers
// and effects subfolders.
//
// Layout (18-byte header + DMX data):
//   0-7   "Art-Net\0"
//   8-9   OpCode, little-endian — OpDmx = 0x5000
//   10-11 protocol version, big-endian — 14
//   12    sequence
//   13    physical port
//   14-15 universe, little-endian
//   16-17 data length, big-endian
//   18+   DMX channel data

constexpr uint16_t ARTNET_PORT = 6454;
constexpr size_t MAX_CHANNELS_PER_UNIVERSE = 510; // 170 RGB lights
constexpr size_t ARTNET_HEADER_SIZE = 18;

// Build an ArtNet OpDmx packet into outBuf. Returns the total packet size.
// outBuf must be at least ARTNET_HEADER_SIZE + dataLen.
inline size_t buildArtDmxPacket(uint8_t* outBuf, uint16_t universe, uint8_t sequence,
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

// Parse + validate an ArtNet OpDmx packet. Returns true and sets the out
// params when pkt is a well-formed OpDmx datagram: "Art-Net\0" magic, OpDmx
// opcode, and a declared data length that fits inside the received bytes
// (dataOut points into pkt — zero copy). Anything else (other opcodes, short
// headers, lying length fields) returns false and the caller drops the packet.
// The protocol-version field is deliberately not checked — be liberal in what
// we accept; the sequence field is the caller's concern (the receive effect
// ignores it: last write wins).
inline bool parseArtDmxPacket(const uint8_t* pkt, size_t len, uint16_t& universeOut,
                              const uint8_t*& dataOut, uint16_t& dataLenOut) {
    if (!pkt || len < ARTNET_HEADER_SIZE) return false;
    if (std::memcmp(pkt, "Art-Net", 8) != 0) return false;
    if (pkt[8] != 0x00 || pkt[9] != 0x50) return false;   // OpDmx only
    const uint16_t dataLen = static_cast<uint16_t>((pkt[16] << 8) | pkt[17]);
    if (dataLen == 0 || dataLen > len - ARTNET_HEADER_SIZE) return false;
    universeOut = static_cast<uint16_t>(pkt[14] | (pkt[15] << 8));
    dataOut = pkt + ARTNET_HEADER_SIZE;
    dataLenOut = dataLen;
    return true;
}

} // namespace mm
