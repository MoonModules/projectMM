#pragma once

// Reassembles a chunked APPLY_OP payload ("Improv = REST over serial") into one
// NUL-terminated op-JSON buffer. This is the pure state machine behind the device's
// 0xFC handler: the platform layer (platform_esp32_improv.cpp) owns the serial I/O
// (reading frames, sending acks/errors, the single-buffer opReady atomic), and hands
// each chunk's [seq][last][bytes] here. Splitting it out follows the same core/platform
// line as ImprovFrame.h — the algorithm is core (and desktop-unit-testable), the UART
// is platform — so the reassembly + sequence guard is proven without hardware.
//
// Frame chunk: [seq][last][chunk bytes…]. seq 0 starts a fresh op (resets the buffer);
// every later chunk must be the next seq in order. A duplicate (an installer retry on a
// misread timeout) or an out-of-order chunk would splice garbage into the buffer, so it
// is rejected and the buffer reset. USB serial is in-order, but the installer's open-loop
// send could re-emit a chunk, so the guard is real, not theoretical.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mm {

class ImprovOpReassembler {
public:
    enum class Result : uint8_t {
        Continue,  // chunk accepted, more expected (not the last)
        Ready,     // last chunk accepted; out() is a complete NUL-terminated op
        Error,     // bad chunk (out-of-order / duplicate / overflow); buffer reset
    };

    // buf/cap is the caller-owned reassembly buffer (one byte is reserved for the NUL,
    // so the largest op JSON that fits is cap-1 bytes).
    ImprovOpReassembler(char* buf, size_t cap) : buf_(buf), cap_(cap) {}

    // Feed one chunk. `seq` is its 0-based index, `last` true on the final chunk.
    // On Ready, out() holds the reassembled, NUL-terminated JSON and len() its length;
    // the reassembler is reset for the next op. On Error the buffer is reset and the
    // caller should error the frame. chunk may be null iff chunkLen is 0.
    Result feed(uint8_t seq, bool last, const uint8_t* chunk, size_t chunkLen) {
        // Sequence guard. seq 0 always starts fresh (so a new op recovers cleanly even
        // after a previous one errored mid-stream); any other seq must be the next one.
        if (seq == 0) {
            len_ = 0;
            nextSeq_ = 1;
        } else if (seq != nextSeq_) {
            reset();
            return Result::Error;
        } else {
            nextSeq_++;
        }

        // Overflow guard: keep one byte for the NUL. Drop + error rather than truncate.
        if (len_ + chunkLen >= cap_) {
            reset();
            return Result::Error;
        }
        if (chunkLen) std::memcpy(buf_ + len_, chunk, chunkLen);
        len_ += chunkLen;

        if (last) {
            buf_[len_] = 0;
            size_t complete = len_;
            reset();          // ready for the next op
            len_ = complete;  // ...but keep the length readable until the next feed()
            return Result::Ready;
        }
        return Result::Continue;
    }

    const char* out() const { return buf_; }
    size_t len() const { return len_; }

    // Drop any partial op (e.g. when the single-buffer consumer wants a clean slate).
    void reset() {
        len_ = 0;
        nextSeq_ = 0;
    }

private:
    char* buf_;
    size_t cap_;
    size_t len_ = 0;       // bytes reassembled so far
    uint8_t nextSeq_ = 0;  // next chunk index expected (0 = awaiting a fresh op)
};

}  // namespace mm
