#pragma once

#include <cstddef>
#include <cstdint>

namespace mm {

// Base64 encode (RFC 4648). Standard alphabet, `=` padding. Writes a
// null-terminated string into `out`; truncates rather than overflowing if
// the encoded form would exceed outMax.
//
// Used in two places: the WebSocket handshake response (encoding the
// SHA-1 of `client_key + magic_GUID`), and `/api/state`'s Password
// serialization (XOR-then-base64 obfuscation, see HttpServerModule).
// Both are short payloads; the encoder is straightforward not optimised.
inline void base64Encode(const uint8_t* in, size_t inLen, char* out, size_t outMax) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t oi = 0;
    for (size_t i = 0; i < inLen && oi + 4 < outMax; i += 3) {
        uint32_t n = static_cast<uint32_t>(in[i]) << 16;
        if (i + 1 < inLen) n |= static_cast<uint32_t>(in[i + 1]) << 8;
        if (i + 2 < inLen) n |= static_cast<uint32_t>(in[i + 2]);
        out[oi++] = table[(n >> 18) & 0x3F];
        out[oi++] = table[(n >> 12) & 0x3F];
        out[oi++] = (i + 1 < inLen) ? table[(n >> 6) & 0x3F] : '=';
        out[oi++] = (i + 2 < inLen) ? table[n & 0x3F] : '=';
    }
    out[oi] = 0;
}

} // namespace mm
