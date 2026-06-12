#pragma once

#include <cstdint>
#include <cstring>

namespace mm {

// DDP (Distributed Display Protocol, 3waylabs) wire format — the one place the
// layout lives, shared by NetworkSendDriver (build) and NetworkReceiveEffect
// (parse); a unit test round-trips build→parse. Same shape as ArtNetPacket.h.
//
// DDP is the high-throughput choice: a 10-byte header and 1440-byte payload
// carry 480 RGB lights per packet vs ArtNet's 170 — and per-packet cost is
// what dominates the wire time (~280 µs Ethernet / ~1140 µs WiFi per packet).
//
// Layout (10-byte header + data; multi-byte fields BIG-endian):
//   0    flags: VV=01 in the top bits (0x40), 0x01 = push (last packet of frame)
//   1    sequence (low 4 bits; 0 = unused)
//   2    data type (0x01 = RGB convention; receivers accept loosely)
//   3    destination id (1 = default display)
//   4-7  data offset — BYTE position in the display buffer
//   8-9  data length
//   10+  data
//
// Validation is deliberately thin: the 2-bit version field is the only magic,
// so a stray non-DDP datagram with byte0 ≈ 0x4x can parse "successfully" with
// a garbage offset — the receiver's offset bound check absorbs that, and the
// real protocol discriminator is the dedicated port (4048), not the header.

constexpr uint16_t DDP_PORT = 4048;
constexpr size_t DDP_HEADER_SIZE = 10;
constexpr size_t DDP_MAX_PAYLOAD = 1440;  // 480 RGB / 360 RGBW lights; divisible by 3 and 4

// Build a DDP data packet. outBuf must be at least DDP_HEADER_SIZE + dataLen.
// `push` marks the last packet of a frame (receivers that double-buffer show
// the frame on push; ours streams into staging and doesn't need it).
inline size_t buildDdpPacket(uint8_t* outBuf, uint32_t offset, bool push,
                             const uint8_t* data, uint16_t dataLen) {
    outBuf[0] = static_cast<uint8_t>(0x40 | (push ? 0x01 : 0x00));
    outBuf[1] = 0;                       // sequence unused
    outBuf[2] = 0x01;                    // RGB
    outBuf[3] = 0x01;                    // default display
    outBuf[4] = static_cast<uint8_t>(offset >> 24);
    outBuf[5] = static_cast<uint8_t>(offset >> 16);
    outBuf[6] = static_cast<uint8_t>(offset >> 8);
    outBuf[7] = static_cast<uint8_t>(offset & 0xFF);
    outBuf[8] = static_cast<uint8_t>(dataLen >> 8);
    outBuf[9] = static_cast<uint8_t>(dataLen & 0xFF);
    std::memcpy(outBuf + DDP_HEADER_SIZE, data, dataLen);
    return DDP_HEADER_SIZE + dataLen;
}

// Parse + validate a DDP data packet: version bits and a declared length that
// fits the datagram. Sequence, data type, destination and the push flag are
// deliberately ignored (hold-last-frame staging makes push moot; last write
// wins — the same stance as ArtNet's ignored sequence). dataOut points into
// pkt (zero copy).
inline bool parseDdpPacket(const uint8_t* pkt, size_t len, uint32_t& offsetOut,
                           const uint8_t*& dataOut, uint16_t& dataLenOut) {
    if (!pkt || len < DDP_HEADER_SIZE) return false;
    if ((pkt[0] & 0xC0) != 0x40) return false;   // version must be 01
    const uint16_t dataLen = static_cast<uint16_t>((pkt[8] << 8) | pkt[9]);
    if (dataLen == 0 || dataLen > len - DDP_HEADER_SIZE) return false;
    offsetOut = (static_cast<uint32_t>(pkt[4]) << 24) | (static_cast<uint32_t>(pkt[5]) << 16)
              | (static_cast<uint32_t>(pkt[6]) << 8) | pkt[7];
    dataOut = pkt + DDP_HEADER_SIZE;
    dataLenOut = dataLen;
    return true;
}

} // namespace mm
