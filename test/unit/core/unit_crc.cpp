// @module crc

#include "doctest.h"
#include "core/crc.h"

#include <cstring>

using namespace mm;

// CRC-16/CCITT-FALSE has a well-known check value: "123456789" → 0x29B1. Pinning it proves the
// polynomial/init/reflection match the standard variant (so a fingerprint computed here matches
// any other CCITT-FALSE implementation).
TEST_CASE("crc: CCITT-FALSE check vector") {
    const char* s = "123456789";
    CHECK(crc16(reinterpret_cast<const uint8_t*>(s), std::strlen(s)) == 0x29B1u);
}

// A change-detector: different content → (almost always) different CRC; identical content → same.
TEST_CASE("crc: same bytes hash equal, a one-bit change differs") {
    const uint8_t a[] = {0, 1, 2, 3, 4, 5};
    const uint8_t b[] = {0, 1, 2, 3, 4, 5};
    uint8_t c[] = {0, 1, 2, 3, 4, 5};
    CHECK(crc16(a, sizeof(a)) == crc16(b, sizeof(b)));   // identical → equal
    c[3] ^= 0x01;                                        // flip one bit
    CHECK(crc16(a, sizeof(a)) != crc16(c, sizeof(c)));   // changed → differs
}

// Empty span returns the init value (no bytes processed).
TEST_CASE("crc: empty input is the init value") {
    CHECK(crc16(nullptr, 0) == 0xFFFFu);
}
