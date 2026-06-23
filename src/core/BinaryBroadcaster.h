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
    // Stage one binary WS frame (the implementation prepends the WS header) for non-blocking
    // fan-out to all clients. Returns true if the frame was accepted, false if DROPPED because
    // a previous frame is still draining (backpressure) — the producer reads that as "the link
    // can't keep up at this rate" and can adapt (e.g. PreviewDriver downscales the preview).
    virtual bool broadcastBinary(const platform::WriteChunk* payload, int chunkCount) = 0;

    // How many transport-poll ticks the LAST fully-sent frame took to drain to all clients
    // (1 = went out immediately; higher = the link is backpressured). This is the real
    // "can the link keep up" signal — unlike a dropped-frame count, which a producer running
    // faster than the per-frame drain trips even on a healthy link. PreviewDriver reads this
    // to adapt its resolution: high latency → downscale, low → refine back to full.
    virtual uint16_t lastDrainTicks() const = 0;

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
