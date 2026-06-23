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
