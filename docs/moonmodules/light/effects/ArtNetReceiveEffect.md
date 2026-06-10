# ArtNet Receive Effect

Receives ArtNet OpDmx UDP packets and writes the channel data into the layer buffer, behaving like any other effect: it can be combined with modifiers, participates in layer blending, and is selectable/configurable through the same UI. The end-to-end pair with [ArtNetSendDriver](../drivers/ArtNetSendDriver.md) — a desktop build on a PC can drive an ESP32's LEDs over the network.

## Controls

- `universe_start` (uint16_t, default 0) — first universe to listen for; mirrors the sender's `universe_start`. A packet for universe `u` lands at byte offset `(u − universe_start) × 510` in the buffer (the 510-channel-per-universe split the sender uses); universes below `universe_start` or beyond the buffer are ignored, payloads overrunning the buffer are clamped.
- `port` (uint16_t, default 6454) — UDP listen port (6454 is the Art-Net standard). Changing it rebinds the socket live.

## Rendering

Opens and binds a UDP socket in `setup()` (a bind failure — port already in use — lands in the status field). `loop()` polls non-blocking at the frame boundary: it drains pending packets (bounded per tick, so a packet flood can't wedge the render loop), validates each with the shared OpDmx parser, and copies payloads into a **staging buffer**; the staging buffer is then copied to the layer buffer every tick.

The staging buffer is load-bearing, not an optimisation: the Layer clears its buffer at the start of every tick, so writing packets straight into it would strobe black between ArtNet frames. Staging gives hold-last-frame semantics — the lights keep showing the last received frame until the next one arrives. The ArtNet sequence field is ignored: out-of-order packets within a frame are last-write-wins into staging.

## Wire contract

The OpDmx packet layout (header bytes, opcodes, endianness, the 510-channel universe split) lives in `src/light/ArtNetPacket.h`, shared with the sender — see [ArtNetSendDriver](../drivers/ArtNetSendDriver.md) for the transport-level details. The receiver accepts any protocol version and rejects non-OpDmx opcodes and packets whose declared length exceeds the datagram.

## Tests

[Unit tests: ArtNetReceiveEffect](../../../tests/unit-tests.md#artnetreceiveeffect) — build→parse round-trip + reject cases, universe placement/clamping into the staging buffer, staging lifecycle (sized off the hot path, freed on teardown), and a desktop localhost UDP round-trip that exercises the platform receive path end-to-end.

Live tier: `uv run scripts/scenario/run_artnet_live.py` ([MoonDeck.md § run_artnet_live](../../../../scripts/MoonDeck.md#run_artnet_live)) proves the path on real hardware — the PC seeds each online board over ArtNet UDP and every board relays to every other, asserting the received colour through each device's `/ws` preview stream.

## Design notes

- ArtNet receive as an effect (not a separate input mechanism) is a key architectural choice: any external light source is just another MoonModule that writes into a layer buffer.
- Processing is synchronous at the frame boundary — check for pending packets, never block ([architecture.md](../../../architecture.md) network-input rule).

## Prior art

### MoonLight — D_NetworkIn ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Drivers/D_NetworkIn.h))

ArtNet/E1.31/DDP receive as a driver node. Supports multiple protocols.

### projectMM v1 — ArtNetInModule ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/modules/effects/ArtNetInModule.h))

v1 ArtNetInModule (commit 54b50bc). Treated as an effect within a layer, with the same `universe_start` control.

## Source

[ArtNetReceiveEffect.h](../../../../src/light/effects/ArtNetReceiveEffect.h)
