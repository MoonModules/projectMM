#pragma once

#include "platform/platform.h"  // platform::WriteChunk

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

protected:
    ~BinaryBroadcaster() = default;  // not owned through this interface
};

} // namespace mm
