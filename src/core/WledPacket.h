#pragma once

// WLED presence packet — the 44-byte header a WLED device broadcasts to 255.255.255.255
// on UDP 65506 for discovery (NOT sync: a WLED that receives one lists the sender, it does
// not mirror its state — sync/control is a separate protocol on a port WLED never shares).
//
// projectMM uses this in two directions, both discovery-only:
//   - PARSE inbound packets to discover real WLED devices on the LAN (WledPlugin).
//   - BUILD an outbound packet so projectMM devices discover each other (MmPlugin presence)
//     AND so a WLED / WLED app browsing 65506 can also list us.
//
// Wire layout (little-endian fields, packed, exactly 44 bytes) — observed from a live WLED
// and cross-checked against the field names in MoonLight's ModuleDevices.h; re-implemented
// fresh here, not copied. WLED validates token==255 && id==1 && ip0==its-own-subnet.
//
// Prior art / credit: the WLED native-app discovery contract this interoperates with was
// reverse-engineered from Christophe Gagnier's (@Moustachauve) WLED-Android client
// (github.com/Moustachauve/WLED-Android) — its `DeviceDiscovery`/`Info`/`State` models
// told us exactly which fields the app requires. The 65506 packet shape cross-references
// MoonLight's `ModuleDevices.h`. We carry those ideas forward in our own code.
//
//   off  size  field
//   0    1     token   = 255   (magic)
//   1    1     id      = 1     (magic / protocol)
//   2-5  4     ip0..ip3       sender IPv4 octets
//   6-37 32    name           null-padded hostname
//   38   1     type           low 7 bits = board kind; bit 7 (0x80) = lights on
//   39   1     insId          last IP octet (WLED uses it as an instance index)
//   40-43 4    version        numeric build date (informational)

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace mm {

struct WledPacket {
    static constexpr uint16_t kPort = 65506;
    static constexpr size_t   kSize = 44;
    static constexpr uint8_t  kToken = 255;
    static constexpr uint8_t  kId = 1;
    static constexpr size_t   kNameOff = 6;
    static constexpr size_t   kNameMax = 32;
    static constexpr size_t   kTypeOff = 38;

    // True if `data`/`len` is a valid WLED presence header (magic bytes + full length).
    // Defensive: any short or non-matching datagram returns false, never reads OOB.
    static bool isValid(const uint8_t* data, size_t len) {
        // Exactly kSize: a presence packet is a fixed 44-byte header. A longer datagram on
        // this port is something else (e.g. a WLED realtime/sync packet), not presence.
        return data && len == kSize && data[0] == kToken && data[1] == kId;
    }

    // Extract the null-padded name (bytes 6..37) into `out` (NUL-terminated). Caller-sized
    // buffer; truncates to outCap-1. Assumes isValid() already passed.
    static void readName(const uint8_t* data, char* out, size_t outCap) {
        if (!out || outCap == 0) return;
        size_t n = 0;
        for (; n < kNameMax && n < outCap - 1; n++) {
            uint8_t c = data[kNameOff + n];
            if (c == 0) break;
            out[n] = static_cast<char>(c);
        }
        out[n] = 0;
    }

    // Build a 44-byte presence packet into `out` (must be >= kSize). `ip` = our IPv4
    // octets (ip0 must be our real first octet or WLED's subnet check rejects us),
    // `name` = deviceName, `boardType` = the low-7-bit board kind, `lightsOn` sets bit 7.
    // Pure presence — carries no command, so a receiving WLED only lists us.
    static void build(uint8_t* out, const uint8_t ip[4], const char* name,
                      uint8_t boardType, bool lightsOn) {
        std::memset(out, 0, kSize);
        out[0] = kToken;
        out[1] = kId;
        out[2] = ip[0]; out[3] = ip[1]; out[4] = ip[2]; out[5] = ip[3];
        if (name) {
            size_t n = std::strlen(name);
            if (n > kNameMax) n = kNameMax;   // bytes 6..37, null-padded by the memset
            std::memcpy(out + kNameOff, name, n);
        }
        out[kTypeOff] = static_cast<uint8_t>((boardType & 0x7f) | (lightsOn ? 0x80 : 0));
        out[39] = ip[3];   // insId = last IP octet, matching WLED's convention
        // version (40..43) left 0 — informational only, no validator reads it.
    }

    // projectMM marker. A projectMM device broadcasts a WLED-VALID packet (so WLED apps
    // list it too), but a peer projectMM device must tell "this is a projectMM peer" from
    // "a generic WLED". We stamp a sentinel into the version field (bytes 40–43) — a field
    // no WLED validator reads, so it stays WLED-valid while uniquely marking us. ASCII "MM"
    // + a small protocol version, little-endian.
    static constexpr size_t  kMarkerOff = 40;
    static constexpr uint32_t kMmMarker = 0x014d4d00u;   // 0x00 'M' 'M' 0x01 → bytes: 00 4D 4D 01

    static void stampMmMarker(uint8_t* out) {
        out[kMarkerOff + 0] = (kMmMarker >> 0) & 0xff;
        out[kMarkerOff + 1] = (kMmMarker >> 8) & 0xff;
        out[kMarkerOff + 2] = (kMmMarker >> 16) & 0xff;
        out[kMarkerOff + 3] = (kMmMarker >> 24) & 0xff;
    }

    static bool hasMmMarker(const uint8_t* data, size_t len) {
        if (len < kSize) return false;
        uint32_t v = uint32_t(data[kMarkerOff]) | (uint32_t(data[kMarkerOff + 1]) << 8)
                   | (uint32_t(data[kMarkerOff + 2]) << 16) | (uint32_t(data[kMarkerOff + 3]) << 24);
        return v == kMmMarker;
    }
};

}  // namespace mm
