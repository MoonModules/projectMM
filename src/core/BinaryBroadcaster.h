#pragma once

#include "platform/platform.h"  // platform::WriteChunk
#include <cstdint>

namespace mm {

// A sink that broadcasts a binary WebSocket message to all connected clients.
// HttpServerModule implements it; producers (e.g. PreviewDriver) hold a pointer
// to this interface rather than to the concrete server, so a light-domain
// producer depends only on "something I can send bytes to" — not on the HTTP
// server's full surface. Domain-neutral: the bytes' meaning is the caller's.
struct BinaryBroadcaster {
    // Send one binary WS frame whose payload is the given scatter-gather chunks
    // (the implementation prepends the WS frame header). Backpressured clients
    // skip the frame; corrupt / dead sockets are dropped.
    virtual void broadcastBinary(const platform::WriteChunk* payload, int chunkCount) = 0;

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
