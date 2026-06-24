#pragma once

#include "platform/platform.h"  // platform::WriteChunk
#include <cstdint>
#include <cstddef>  // size_t

namespace mm {

// A sink that broadcasts a binary WebSocket message to all connected clients.
// HttpServerModule implements it; producers (e.g. PreviewDriver) hold a pointer
// to this interface rather than to the concrete server, so a light-domain
// producer depends only on "something I can send bytes to" — not on the HTTP
// server's full surface. Domain-neutral: the bytes' meaning is the caller's.
struct BinaryBroadcaster {
    // Stream ONE binary WS frame whose payload is PUSHED incrementally, so the caller never
    // holds the whole frame in a buffer. Begin/push/end trio, fitting a forward-only producer
    // like Layouts::forEachCoord (push from inside its callback):
    //   beginBinaryFrame(totalLen)  — build + send the WS header (totalLen = exact payload size)
    //   pushBinaryFrame(data, len)  — send the next payload slice (call as many times as needed)
    //   endBinaryFrame()            — finish; returns true if every client got the whole frame
    // The implementation streams straight to the clients with no frame-sized staging buffer, so a
    // large frame (e.g. PreviewDriver's coordinate table, tens of KB) goes out on a memory-tight
    // board where a contiguous staging block won't fit. The caller MUST push exactly `totalLen`
    // bytes between begin and end. Only one frame may be open at a time.
    virtual void beginBinaryFrame(size_t totalLen) = 0;
    virtual void pushBinaryFrame(const uint8_t* data, size_t len) = 0;
    virtual bool endBinaryFrame() = 0;

    // RESUMABLE one-frame send for a payload that lives in a STABLE caller-owned buffer (no copy):
    // one WS message = `header` (copied — small, may be a stack local) followed by `body` (a pointer
    // the caller guarantees stable until the send completes or is cancelled). The implementation
    // drains it across transport-poll ticks (a bounded chunk per tick, non-blocking), so a large
    // frame never spins the caller's loop. The frame is still ONE atomic WS message to the browser
    // — "resumable" means delivered over wall-clock, not split into multiple messages.
    //   sendBufferedFrame(...) — begin a send; while one is in flight a new call is DROPPED
    //                            (drop-new backpressure — the in-flight frame is kept, the new one
    //                            rejected), and the caller reads that as "link busy".
    //   bufferedSendIdle()     — true when no send is in flight (the previous frame fully drained
    //                            or was cancelled). The caller gates the next frame on this, so the
    //                            effective frame rate self-limits to what the link sustains.
    //   cancelBufferedSend()   — abandon the in-flight send NOW. The caller MUST call this before the
    //                            `body` buffer is freed/reallocated (e.g. a geometry rebuild) so a
    //                            cursor never reads freed memory. Today the only caller cancels on a
    //                            new-client connect, which also bumps clientGeneration() and re-sends
    //                            a fresh coordinate table — so a client that received a partial frame
    //                            is re-primed by the next full message rather than left mis-framed.
    // Only PreviewDriver uses this today (the full-res colour frame, whose payload is the producer
    // buffer). The coord table / downsampled frames keep the begin/push/end path.
    virtual bool sendBufferedFrame(const uint8_t* header, size_t headerLen,
                                   const uint8_t* body, size_t bodyLen) = 0;
    virtual bool bufferedSendIdle() const = 0;
    virtual void cancelBufferedSend() = 0;

    // A counter that increments each time a new client connects. A producer whose
    // first message is stateful (e.g. PreviewDriver's coordinate table, which colour
    // frames then reference) watches this: when it changes, a fresh client just joined
    // and needs that priming message re-sent NOW, rather than waiting for the producer's
    // periodic re-broadcast. Cheap, broadcast-only (no per-client send / inbound routing):
    // the producer re-broadcasts to everyone, idempotent on existing clients.
    virtual uint32_t clientGeneration() const = 0;

protected:
    ~BinaryBroadcaster() = default;  // not owned through this interface
};

} // namespace mm
