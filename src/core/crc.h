#pragma once

#include <cstddef>
#include <cstdint>

// CRC-16/CCITT-FALSE: a 16-bit checksum over a byte span. Used as a cheap, well-distributed
// "fingerprint" of a block of state — e.g. Game of Life hashes its grid each generation to
// detect that the pattern has gone static or fallen into a short oscillation (the same CRC
// recurring) so it can respawn instead of looping forever. The textbook polynomial 0x1021,
// init 0xFFFF, no reflection — the recognisable CCITT-FALSE variant.
//
// Not a security hash (a CRC is trivially collidable); it's a fast change-detector, which is all
// the stasis check needs. Integer-only, no table (the bit-serial form is tiny and the call sites
// run off the hot path — once per generation, not per pixel).

namespace mm {

constexpr uint16_t crc16(const uint8_t* data, size_t len, uint16_t crc = 0xFFFFu) {
    for (size_t i = 0; i < len; i++) {
        crc = static_cast<uint16_t>(crc ^ (static_cast<uint16_t>(data[i]) << 8));
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000u) ? static_cast<uint16_t>((crc << 1) ^ 0x1021u)
                                  : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

}  // namespace mm
