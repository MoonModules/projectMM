#pragma once

#include <cstdint>
#include <cstdlib>  // std::strtol
#include <cstring>  // std::memcpy

namespace mm {

/// @defgroup IpList Parsing a destination list of IPv4 addresses
/// @{
/// One human-typed text control turned into the list of addresses a driver sends to.
///
/// @moreinfo
///
/// ## Why a list rather than one address per driver
///
/// This sits beside `parsePinList`, the GPIO CSV parser, as the same shape of thing: a human-typed list of hardware endpoints.
/// A driver that fans one buffer out to several receivers therefore reads its destinations the way a driver fanning out to several GPIO lanes reads its pins.
///
/// Art-Net 4 requires ArtDmx to be unicast to the node that owns each universe, stating that there are no conditions in which broadcast is allowed.
/// Driving several receivers therefore means several destinations.
/// Expressing that as one control keeps the common case, a row of identical tubes on consecutive addresses, to a single field instead of one driver instance each.
///
/// ## The two syntaxes, which mix freely
///
/// | Written | What it means |
/// |---------|---------------|
/// | `192.168.1.60-70` | a range over the last octet alone, giving `.60` through `.70`, the first three octets having to match |
/// | `192.168.1.60,61,62,65` | a list where, after one full dotted quad, a bare number is the next host on the same `/24`, and a further full quad switches subnet |
/// | `192.168.1.60, 10.0.0.5` | explicit full quads, across any subnets |
///
/// The parser returns null on success, or a static error literal the caller hands straight to `setStatus`.
/// `unit_IpList.cpp` pins it on the host.

/// Parse one quad, or a bare last-octet shorthand continuing `prev`, advancing `p` past the token.
namespace detail {
inline bool parseOneIp(const char*& p, const uint8_t prev[4], bool havePrev, uint8_t out[4]) {
    long o[4];
    char* end = nullptr;
    o[0] = std::strtol(p, &end, 10);
    if (end == p || o[0] < 0 || o[0] > 255) return false;
    const char* q = end;
    uint8_t n = 1;
    while (n < 4 && *q == '.') {
        const char* after = q + 1;
        char* e2 = nullptr;
        o[n] = std::strtol(after, &e2, 10);
        if (e2 == after || o[n] < 0 || o[n] > 255) return false;
        q = e2;
        n++;
    }
    if (n == 4) {                       // a full dotted quad
        for (uint8_t i = 0; i < 4; i++) out[i] = static_cast<uint8_t>(o[i]);
    } else if (n == 1 && havePrev) {    // a bare host number → same /24 as the previous address
        std::memcpy(out, prev, 3);
        out[3] = static_cast<uint8_t>(o[0]);
    } else {
        return false;                   // a bare number with no previous address to extend
    }
    p = q;
    return true;
}
}  // namespace detail

/// Parse `s` into `out` as 4-byte quads, `nOut` receiving the count, a blank string yielding none.
inline const char* parseIpList(const char* s, uint8_t (*out)[4], uint8_t maxIps, uint8_t& nOut) {
    nOut = 0;
    if (!s) return nullptr;
    while (*s == ' ') s++;
    if (!*s) return nullptr;            // blank → no destinations, caller idles

    const char* p = s;
    uint8_t prev[4] = {};
    bool havePrev = false;

    while (true) {
        uint8_t ip[4];
        if (!detail::parseOneIp(p, prev, havePrev, ip)) return "invalid ip list";

        while (*p == ' ') p++;
        if (*p == '-') {                // a RANGE: expand low..high over the last octet
            p++;
            while (*p == ' ') p++;
            char* end = nullptr;
            const long hi = std::strtol(p, &end, 10);
            if (end == p || hi < 0 || hi > 255) return "invalid ip range";
            if (hi < ip[3]) return "ip range runs backwards";
            for (long last = ip[3]; last <= hi; last++) {
                if (nOut >= maxIps) return "too many destinations";
                out[nOut][0] = ip[0]; out[nOut][1] = ip[1];
                out[nOut][2] = ip[2]; out[nOut][3] = static_cast<uint8_t>(last);
                nOut++;
            }
            std::memcpy(prev, ip, 4);
            havePrev = true;
            p = end;
        } else {
            if (nOut >= maxIps) return "too many destinations";
            std::memcpy(out[nOut], ip, 4);
            nOut++;
            std::memcpy(prev, ip, 4);
            havePrev = true;
        }

        while (*p == ' ') p++;
        if (*p == '\0') return nullptr;
        if (*p != ',') return "invalid ip list";
        p++;
        while (*p == ' ') p++;
    }
}

/// @}
}  // namespace mm
