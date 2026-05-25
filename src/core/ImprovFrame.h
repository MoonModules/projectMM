#pragma once

// Improv-WiFi serial framing — pure C++, no ESP-IDF or stdlib-network deps.
//
// Wire format (https://www.improv-wifi.com/serial/):
//   [I][M][P][R][O][V][version=1][type][length][payload×length][checksum]
//
// The parser is a state machine fed one byte at a time. The framing layer
// here is intentionally separate from the Improv RPC semantics (the
// upstream `improv/improv` library — ESP Component Registry; source:
// improv-wifi/sdk-cpp on GitHub — handles RPC payload parsing via
// `improv::parse_improv_data`). Splitting at this boundary lets us:
//   - Unit-test the framing on the host (test/test_improv_frame.cpp).
//   - Keep the ESP32 task (platform_esp32.cpp) thin: feed bytes from the
//     UART driver into ImprovFrameParser, react to complete frames.
//   - Re-use the builder for both the ESP32 send path and the Python CLI
//     spec (which reimplements the same framing in scripts/build/
//     improv_provision.py — same wire format, two languages).

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace mm {

// Framing constants — match the spec verbatim. The library also defines
// these (improv.h: IMPROV_SERIAL_VERSION etc.) but we don't pull that in
// here to keep this header dependency-free for host-side testing.
inline constexpr uint8_t kImprovMagic[6] = {'I','M','P','R','O','V'};
inline constexpr uint8_t kImprovSerialVersion = 1;
inline constexpr size_t  kImprovMaxPayload    = 128;  // RPC bodies are well under this

// Frame types from the spec; named without the protocol prefix to avoid
// shadowing the library's `improv::ImprovSerialType` enum where both are
// in scope (the test code only includes this header; the ESP32 task
// includes both and dispatches at the boundary).
enum class ImprovFrameType : uint8_t {
    CurrentState = 0x01,
    ErrorState   = 0x02,
    Rpc          = 0x03,
    RpcResponse  = 0x04,
};

// Result of feeding a byte to the parser.
enum class ImprovFeedResult : uint8_t {
    NeedMore,        // mid-frame; keep feeding
    FrameReady,      // a complete, checksum-valid frame is in lastType()/lastPayload()
    BadChecksum,     // a complete frame arrived but the checksum is wrong; dropped
    OversizePayload, // length byte > kImprovMaxPayload; resync
};

// Byte-at-a-time framing parser. Resets to the magic-search state after
// every completed frame (or error). Caller owns the parser; one instance
// per UART channel.
class ImprovFrameParser {
public:
    // Returns NeedMore until a full frame has been read. On FrameReady the
    // caller can read lastType() + lastPayload()/lastPayloadLen(). The
    // buffers are valid until the next feed() call.
    ImprovFeedResult feed(uint8_t byte) {
        switch (state_) {
            case State::Magic0: case State::Magic1: case State::Magic2:
            case State::Magic3: case State::Magic4: case State::Magic5: {
                const size_t i = static_cast<size_t>(state_);
                if (byte == kImprovMagic[i]) {
                    headerBytes_[i] = byte;
                    state_ = static_cast<State>(i + 1);
                } else {
                    // Resync: if this byte starts the magic, take it.
                    state_ = State::Magic0;
                    if (byte == kImprovMagic[0]) {
                        headerBytes_[0] = byte;
                        state_ = State::Magic1;
                    }
                }
                return ImprovFeedResult::NeedMore;
            }
            case State::Version:
                if (byte == kImprovSerialVersion) {
                    headerBytes_[6] = byte;
                    state_ = State::Type;
                } else {
                    // Bad version — drop and resync. If the bad byte happens
                    // to be the magic start, re-enter the magic search at
                    // Magic1 (same handling as the Magic-state resync above)
                    // so we don't lose an 'I' that begins a new frame
                    // arriving right after a corrupted header.
                    state_ = State::Magic0;
                    if (byte == kImprovMagic[0]) {
                        headerBytes_[0] = byte;
                        state_ = State::Magic1;
                    }
                }
                return ImprovFeedResult::NeedMore;
            case State::Type:
                type_ = byte;
                headerBytes_[7] = byte;
                state_ = State::Length;
                return ImprovFeedResult::NeedMore;
            case State::Length:
                expectedLen_ = byte;
                headerBytes_[8] = byte;
                payloadPos_ = 0;
                if (byte > kImprovMaxPayload) {
                    state_ = State::Magic0;
                    return ImprovFeedResult::OversizePayload;
                }
                state_ = (byte > 0) ? State::Payload : State::Checksum;
                return ImprovFeedResult::NeedMore;
            case State::Payload:
                payload_[payloadPos_++] = byte;
                if (payloadPos_ >= expectedLen_) state_ = State::Checksum;
                return ImprovFeedResult::NeedMore;
            case State::Checksum: {
                uint32_t sum = 0;
                for (int i = 0; i < 9; i++) sum += headerBytes_[i];
                for (uint8_t i = 0; i < expectedLen_; i++) sum += payload_[i];
                const bool ok = (static_cast<uint8_t>(sum & 0xFF) == byte);
                state_ = State::Magic0;  // ready for next frame regardless
                return ok ? ImprovFeedResult::FrameReady : ImprovFeedResult::BadChecksum;
            }
        }
        return ImprovFeedResult::NeedMore;  // unreachable; quiets some compilers
    }

    uint8_t        lastType()       const { return type_; }
    const uint8_t* lastPayload()    const { return payload_; }
    uint8_t        lastPayloadLen() const { return expectedLen_; }

private:
    enum class State : uint8_t {
        Magic0, Magic1, Magic2, Magic3, Magic4, Magic5,
        Version, Type, Length, Payload, Checksum
    };
    State   state_       = State::Magic0;
    uint8_t headerBytes_[9] = {};   // magic(6) + version + type + length
    uint8_t type_        = 0;
    uint8_t expectedLen_ = 0;
    uint8_t payload_[kImprovMaxPayload] = {};
    uint8_t payloadPos_  = 0;
};

// XOR-style checksum (sum mod 256) — same as the spec's. Exposed so
// frame-builders + tests can reuse it.
inline uint8_t improvChecksum(const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += data[i];
    return static_cast<uint8_t>(sum & 0xFF);
}

// Build a complete frame: [magic][version][type][length][payload][checksum].
// Caller-owned output buffer; returns the total byte count written, or 0 on
// overflow (outLen too small) / oversize payload. No allocation; suitable
// for both ESP-IDF tasks and host-side tests.
inline size_t buildImprovFrame(ImprovFrameType type,
                               const uint8_t* payload, size_t payloadLen,
                               uint8_t* out, size_t outLen) {
    if (payloadLen > kImprovMaxPayload) return 0;
    if (out == nullptr) return 0;                       // no buffer to write to
    if (payloadLen > 0 && payload == nullptr) return 0; // can't memcpy from null
    const size_t need = 6 + 1 + 1 + 1 + payloadLen + 1;
    if (outLen < need) return 0;
    size_t p = 0;
    std::memcpy(out + p, kImprovMagic, 6); p += 6;
    out[p++] = kImprovSerialVersion;
    out[p++] = static_cast<uint8_t>(type);
    out[p++] = static_cast<uint8_t>(payloadLen);
    if (payloadLen > 0) {
        std::memcpy(out + p, payload, payloadLen);
        p += payloadLen;
    }
    out[p] = improvChecksum(out, p);
    return p + 1;
}

} // namespace mm
