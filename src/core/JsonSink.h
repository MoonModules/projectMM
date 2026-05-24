#pragma once

#include "platform/platform.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

// Writes JSON with no fixed-buffer size ceiling. Two modes:
//  - socket mode: a small staging buffer flushes to a TcpConnection as it fills,
//    so the whole response never lives in RAM at once (used by GET /api/state).
//  - buffer mode: bytes collect in a heap buffer that doubles on demand, for
//    callers that need the assembled JSON + its length (the WebSocket push,
//    whose frame header carries the length up front).
// Either way, a module tree of any size serializes correctly.
class JsonSink {
public:
    // Socket mode.
    explicit JsonSink(platform::TcpConnection& conn) : conn_(&conn) {}

    // Buffer mode — collects into a growable heap buffer.
    JsonSink() = default;

    ~JsonSink() { if (heap_) platform::free(heap_); }

    JsonSink(const JsonSink&) = delete;
    JsonSink& operator=(const JsonSink&) = delete;

    void append(const char* s) {
        if (!s) return;
        while (*s) {
            if (conn_) {
                if (pos_ == STAGE_SIZE) flushStage();
                stage_[pos_++] = *s++;
            } else {
                if (!ensureHeap(heapLen_ + 1)) return;  // out of memory: drop
                heap_[heapLen_++] = *s++;
            }
        }
        // Keep heap_ null-terminated after every append so data() is a valid
        // C-string even if the caller skips size(). ensureHeap already reserved
        // the +1 slot, so this write is in-bounds.
        if (!conn_ && heap_) heap_[heapLen_] = '\0';
    }

    // Append a printf-formatted fragment. The common case (one control, one
    // module header) fits the FRAG_MAX stack buffer; a fragment that would
    // exceed it — e.g. an unusually long text-control value — is re-formatted
    // into a heap buffer so the output is never silently truncated.
    void appendf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char frag[FRAG_MAX];
        va_list ap;
        va_start(ap, fmt);
        va_list ap2;
        va_copy(ap2, ap);
        int n = std::vsnprintf(frag, sizeof(frag), fmt, ap);
        va_end(ap);
        if (n < 0) { va_end(ap2); return; }
        if (static_cast<size_t>(n) < sizeof(frag)) {
            va_end(ap2);
            append(frag);
            return;
        }
        // Fragment longer than the stack buffer — format into an exact heap buffer.
        char* big = static_cast<char*>(platform::alloc(static_cast<size_t>(n) + 1));
        if (big) {
            std::vsnprintf(big, static_cast<size_t>(n) + 1, fmt, ap2);
            append(big);
            platform::free(big);
        }
        va_end(ap2);
    }

    // Socket mode: flush staged bytes to the socket. Call once at the end.
    void flush() { flushStage(); }

    // Buffer mode: the collected JSON and its length (null-terminated).
    const char* data() const { return heap_ ? heap_ : ""; }
    size_t size() const { return heapLen_; }

private:
    static constexpr size_t STAGE_SIZE = 1024;
    static constexpr size_t FRAG_MAX = 256;

    void flushStage() {
        if (conn_ && pos_ > 0) {
            conn_->write(reinterpret_cast<const uint8_t*>(stage_), pos_);
            pos_ = 0;
        }
    }

    // Grow the heap buffer to hold at least `need` bytes plus a null terminator.
    bool ensureHeap(size_t need) {
        if (need + 1 <= heapCap_) return true;
        size_t newCap = heapCap_ == 0 ? 2048 : heapCap_ * 2;
        while (newCap < need + 1) newCap *= 2;
        char* grown = static_cast<char*>(platform::alloc(newCap));
        if (!grown) return false;
        if (heap_) { std::memcpy(grown, heap_, heapLen_); platform::free(heap_); }
        heap_ = grown;
        heapCap_ = newCap;
        heap_[heapLen_] = 0;
        return true;
    }

    platform::TcpConnection* conn_ = nullptr;  // socket mode when non-null
    char stage_[STAGE_SIZE];
    size_t pos_ = 0;

    char* heap_ = nullptr;                     // buffer mode
    size_t heapLen_ = 0;
    size_t heapCap_ = 0;
};

// Escape a string for embedding inside a JSON string literal: " → \" and
// \ → \\. Writes into `out` (no surrounding quotes). Truncates rather than
// overflowing if the escaped form exceeds outMax. Distinct from
// FilesystemModule::writeJsonString, which appends to a (buf, pos) pair.
inline void jsonEscape(const char* in, char* out, size_t outMax) {
    if (outMax == 0) return;  // no room even for the terminator
    size_t oi = 0;
    for (; *in && oi + 2 < outMax; in++) {
        if (*in == '"' || *in == '\\') out[oi++] = '\\';
        out[oi++] = *in;
    }
    out[oi] = 0;
}

} // namespace mm
