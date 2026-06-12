#pragma once

#include <cstdint>
#include <cstring>

namespace mm {

// E1.31 (streaming ACN / sACN) data-packet wire format — the one place the
// layout lives, shared by NetworkSendDriver (build) and NetworkReceiveEffect
// (parse); a unit test round-trips build→parse so the two can never drift.
// Same shape as ArtNetPacket.h: constants + two inline free functions.
//
// Layout (126-byte header + DMX data; every multi-byte field BIG-endian):
//   Root layer 0–37:
//     0-1   preamble size 0x0010      2-3   postamble size 0x0000
//     4-15  packet identifier "ASC-E1.17\0\0\0"
//     16-17 flags+length 0x7000 | (totalLen − 16)
//     18-21 vector 0x00000004 (E131 data)
//     22-37 CID — sender's stable 16-byte component id
//   Framing layer 38–114:
//     38-39 flags+length 0x7000 | (totalLen − 38)
//     40-43 vector 0x00000002
//     44-107 source name (64 bytes, NUL-padded)
//     108   priority (default 100)    109-110 sync address (0 = none)
//     111   sequence                  112     options (0)
//     113-114 universe (1-based per spec; we transmit whatever the caller says
//             — see the universe rule in NetworkSendDriver.md)
//   DMP layer 115–125:
//     115-116 flags+length 0x7000 | (totalLen − 115)
//     117   vector 0x02              118     address & data type 0xA1
//     119-120 first property address 0x0000
//     121-122 address increment 0x0001
//     123-124 property value count = 1 + dataLen
//     125   DMX start code 0x00
//   126+  channel data

constexpr uint16_t E131_PORT = 5568;
constexpr size_t E131_HEADER_SIZE = 126;
constexpr size_t E131_CID_LENGTH = 16;

namespace detail {
inline void putU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}
inline uint16_t getU16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
} // namespace detail

// Build an E1.31 data packet. outBuf must be at least E131_HEADER_SIZE +
// dataLen. Priority is fixed at 100 (the spec default) and sync address at 0 —
// neither has a use here until a consumer appears.
inline size_t buildE131Packet(uint8_t* outBuf, uint16_t universe, uint8_t sequence,
                              const uint8_t cid[E131_CID_LENGTH],
                              const uint8_t* data, uint16_t dataLen) {
    const size_t totalLen = E131_HEADER_SIZE + dataLen;
    std::memset(outBuf, 0, E131_HEADER_SIZE);

    // Root layer
    detail::putU16(outBuf + 0, 0x0010);                       // preamble size
    // postamble size stays 0
    std::memcpy(outBuf + 4, "ASC-E1.17\0\0\0", 12);
    detail::putU16(outBuf + 16, static_cast<uint16_t>(0x7000 | (totalLen - 16)));
    outBuf[21] = 0x04;                                        // vector 0x00000004
    std::memcpy(outBuf + 22, cid, E131_CID_LENGTH);

    // Framing layer
    detail::putU16(outBuf + 38, static_cast<uint16_t>(0x7000 | (totalLen - 38)));
    outBuf[43] = 0x02;                                        // vector 0x00000002
    std::memcpy(outBuf + 44, "projectMM", 9);                 // source name (NUL-padded)
    outBuf[108] = 100;                                        // priority (spec default)
    // sync address stays 0
    outBuf[111] = sequence;
    // options stay 0
    detail::putU16(outBuf + 113, universe);

    // DMP layer
    detail::putU16(outBuf + 115, static_cast<uint16_t>(0x7000 | (totalLen - 115)));
    outBuf[117] = 0x02;                                       // vector: set property
    outBuf[118] = 0xA1;                                       // address & data type
    // first property address stays 0x0000
    outBuf[122] = 0x01;                                       // address increment
    detail::putU16(outBuf + 123, static_cast<uint16_t>(1 + dataLen));
    // start code stays 0x00

    std::memcpy(outBuf + E131_HEADER_SIZE, data, dataLen);
    return totalLen;
}

// Parse + validate an E1.31 data packet. Liberal like parseArtDmxPacket —
// checks the ACN identifier, the three layer vectors, the DMX start code, and
// that the declared property count fits the datagram; priority/sequence/sync
// are deliberately ignored (last write wins, same stance as ArtNet's sequence).
// dataOut points into pkt (zero copy).
inline bool parseE131Packet(const uint8_t* pkt, size_t len, uint16_t& universeOut,
                            const uint8_t*& dataOut, uint16_t& dataLenOut) {
    if (!pkt || len < E131_HEADER_SIZE) return false;
    if (std::memcmp(pkt + 4, "ASC-E1.17\0\0\0", 12) != 0) return false;
    if (pkt[18] != 0 || pkt[19] != 0 || pkt[20] != 0 || pkt[21] != 0x04) return false;
    if (pkt[40] != 0 || pkt[41] != 0 || pkt[42] != 0 || pkt[43] != 0x02) return false;
    if (pkt[117] != 0x02) return false;
    if (pkt[125] != 0x00) return false;   // only DMX start code 0 carries light data
    const uint16_t propCount = detail::getU16(pkt + 123);
    if (propCount < 1) return false;
    const uint16_t dataLen = static_cast<uint16_t>(propCount - 1);
    if (dataLen == 0 || dataLen > len - E131_HEADER_SIZE) return false;
    universeOut = detail::getU16(pkt + 113);
    dataOut = pkt + E131_HEADER_SIZE;
    dataLenOut = dataLen;
    return true;
}

} // namespace mm
