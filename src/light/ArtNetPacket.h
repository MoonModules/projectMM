#pragma once

#include <cstdint>
#include <cstring>

namespace mm {

// ArtNet wire formats — the one place the packet layouts live. The sender
// (drivers/NetworkSendDriver.h) builds packets with it, the receiver
// (effects/NetworkReceiveEffect.h) parses them with it; a unit test round-trips
// build→parse so the two can never drift apart. Sits at the top of src/light/
// (beside light_types.h) because the protocol is neutral between the drivers
// and effects subfolders.
//
// OpDmx layout (18-byte header + DMX data):
//   0-7   "Art-Net\0"
//   8-9   OpCode, little-endian — OpDmx = 0x5000
//   10-11 protocol version, big-endian — 14
//   12    sequence
//   13    physical port
//   14-15 universe, little-endian
//   16-17 data length, big-endian
//   18+   DMX channel data
//
// Discovery (the Resolume/Madrix/xLights node-list handshake): controllers
// broadcast ArtPoll (OpCode 0x2000); every node answers with ArtPollReply
// (OpCode 0x2100, 239 bytes) carrying its IP, names, MAC, and bound universe —
// that reply is what makes the device appear in a controller's output list.

constexpr uint16_t ARTNET_PORT = 6454;
constexpr size_t MAX_CHANNELS_PER_UNIVERSE = 510; // 170 RGB lights
constexpr size_t ARTNET_HEADER_SIZE = 18;
constexpr size_t ARTNET_POLL_REPLY_SIZE = 239;

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

// True when pkt is an ArtPoll — a controller asking "which nodes are out
// there?". The minimal ArtPoll is 14 bytes (header + protVer + flags + prio).
inline bool isArtPoll(const uint8_t* pkt, size_t len) {
    return pkt && len >= 14 && std::memcmp(pkt, "Art-Net", 8) == 0
        && pkt[8] == 0x00 && pkt[9] == 0x20;   // OpPoll, little-endian
}

// Build the minimal ArtPollReply controllers actually read: our IP + port,
// short/long name, MAC, style "node", one output port bound to
// `universeStart`. Every other field stays zero — accepted by Resolume,
// Madrix and xLights, which key on the fields above. outBuf must be at least
// ARTNET_POLL_REPLY_SIZE bytes.
inline size_t buildArtPollReply(uint8_t* outBuf, const uint8_t ip[4],
                                const uint8_t mac[6], const char* shortName,
                                const char* longName, uint16_t universeStart) {
    std::memset(outBuf, 0, ARTNET_POLL_REPLY_SIZE);
    std::memcpy(outBuf, "Art-Net", 8);
    outBuf[8] = 0x00; outBuf[9] = 0x21;                   // OpPollReply, little-endian
    std::memcpy(outBuf + 10, ip, 4);
    outBuf[14] = 0x36; outBuf[15] = 0x19;                 // port 6454, little-endian
    // 15-bit port address: NetSwitch = bits 14-8, SubSwitch = bits 7-4,
    // SwOut[0] = bits 3-0 — together they re-assemble universeStart.
    outBuf[18] = static_cast<uint8_t>((universeStart >> 8) & 0x7F);
    outBuf[19] = static_cast<uint8_t>((universeStart >> 4) & 0x0F);
    outBuf[20] = 0x00; outBuf[21] = 0xFF;                 // OEM: unknown/generic
    // Short name (18 bytes incl NUL) and long name (64 bytes incl NUL).
    std::strncpy(reinterpret_cast<char*>(outBuf + 26), shortName, 17);
    std::strncpy(reinterpret_cast<char*>(outBuf + 44), longName, 63);
    outBuf[173] = 1;                                      // NumPorts (lo) = 1
    outBuf[174] = 0x80;                                   // PortTypes[0]: can output DMX
    outBuf[182] = 0x80;                                   // GoodOutput[0]: outputting
    outBuf[190] = static_cast<uint8_t>(universeStart & 0x0F);  // SwOut[0]
    // Style at 200 stays 0x00 = StNode.
    std::memcpy(outBuf + 201, mac, 6);
    return ARTNET_POLL_REPLY_SIZE;
}

} // namespace mm
