# Network Receive Effect

Receives lights-over-UDP data — **ArtNet, E1.31/sACN, and DDP, all at once** — and writes it into the layer buffer, behaving like any other effect: composable with modifiers, part of layer blending, selectable through the same UI. The receive side for industry senders (Resolume Arena, Madrix, xLights, LedFx, …) and the end-to-end pair with [NetworkSendDriver](../drivers/NetworkSendDriver.md).

There is deliberately **no protocol control**: the effect binds the three well-known ports (6454 ArtNet, 5568 E1.31, 4048 DDP) simultaneously and validates each packet against its port's wire format — WLED's multi-port pattern. Whatever a sender speaks just works; the status field shows what is being received (`receiving DDP`, …).

## Controls

- `universe_start` (uint16_t, default 0) — first universe to accept (ArtNet/E1.31); a packet for universe `u` lands at byte offset `(u − universe_start) × channels_per_universe`. Universes below the start or beyond the buffer are ignored. E1.31 senders conventionally start at universe 1 — set both ends accordingly (see the sender's universe rule).
- `channels_per_universe` (uint16_t, default 510) — bytes each universe maps to. 510 = whole RGB lights per universe (the xLights/Falcon convention and our own sender's split); set **512** for senders that pack pixels across universe boundaries (Madrix-style). Also clamps each universe's payload to its slot, so a 512-channel frame from a 510-packed source can't bleed its 2 padding bytes into the next universe.

DDP skips the universe math entirely: its packets carry a byte offset and land directly (clamped to the buffer).

## ArtNet discovery (Resolume node lists)

Controllers find output nodes by broadcasting **ArtPoll**; this effect answers with **ArtPollReply** (our IP, MAC, names, bound universe), so the device appears automatically in Resolume's Advanced Output, Madrix and xLights node lists instead of needing manual IP entry. The reply goes unicast to the poller via the platform's `sendToAddr`.

## Rendering

Opens and binds the three sockets in `setup()` (a taken port is reported in the status field; the other sockets still drain). `loop()` polls non-blocking at the frame boundary: it drains each socket (bounded per tick, so a packet flood can't wedge the render loop), validates each packet with its protocol's shared parser, and copies payloads into a **staging buffer**; staging is copied to the layer buffer every tick.

The staging buffer is load-bearing: the Layer clears its buffer at the start of every tick, so writing packets straight into it would strobe black between frames. Staging gives hold-last-frame semantics. Sequence fields (and DDP's push flag) are ignored: out-of-order packets are last-write-wins.

## Wire contracts

The byte layouts live in [ArtNetPacket.h](../../../../src/light/ArtNetPacket.h), [E131Packet.h](../../../../src/light/E131Packet.h) and [DdpPacket.h](../../../../src/light/DdpPacket.h), shared with the sender. The receiver is liberal: any ArtNet protocol version, any E1.31 priority/sequence (no multi-source arbitration), any DDP data type. E1.31 **multicast is not joined** (unicast only — platform IGMP support is a backlog item); point sACN senders at the device's IP.

## Tests

[Unit tests: NetworkReceiveEffect](../../../tests/unit-tests.md#networkreceiveeffect) — per-protocol build→parse round-trips and reject cases, cross-protocol rejects, universe placement with `channels_per_universe` 510 and 512, DDP byte placement with hostile-offset clamping, ArtPoll/ArtPollReply layout, staging lifecycle, and a localhost round-trip driving all three protocol sockets at once.

Live tier: `uv run scripts/scenario/run_network_live.py` ([MoonDeck.md § run_network_live](../../../../scripts/MoonDeck.md#run_network_live)) seeds real boards via all three protocols per round.

## Design notes

- Receive as an effect (not a separate input mechanism): any external light source is just another MoonModule that writes into a layer buffer.
- Processing is synchronous at the frame boundary — check for pending packets, never block ([architecture.md](../../../architecture.md) network-input rule).

## Prior art

### MoonLight — D_NetworkIn ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Drivers/D_NetworkIn.h))

ArtNet/E1.31/DDP receive in one driver node (protocol selected by control; we autodetect by port instead).

### WLED — realtime UDP input

Multi-port listening with per-packet header validation, plus ArtPollReply for controller discovery — the pattern this effect follows.

### projectMM v1 — ArtNetInModule ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/modules/effects/ArtNetInModule.h))

v1 treated ArtNet receive as an effect within a layer, the same architectural choice.

## Source

[NetworkReceiveEffect.h](../../../../src/light/effects/NetworkReceiveEffect.h)
