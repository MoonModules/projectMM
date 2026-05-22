# ArtNet Send Driver

Output driver. Reads from the DriverGroup's output buffer and sends ArtNet DMX packets over UDP. The driver doesn't care whether the buffer is a separate driver buffer or a shared layer buffer — it reads from whatever DriverGroup provides.

The UDP socket is `connect()`-bound to the destination in `setup()`, so each per-universe `sendTo()` skips the address parse + route lookup — a measurable saving when a frame spans dozens of universes (16,384 lights = 97 universes). See [performance.md](../../../performance.md) "ArtNet UDP send cost".

## Controls

- `ip` (text, default "192.168.1.70") — destination IP address
- `universe_start` (uint16_t, default 0, range 0-32767) — first ArtNet universe
- `fps` (uint8_t, default 50, range 1-120) — send frame rate limit. Critical: without FPS limiting, receivers drop packets.

## ArtNet Packet Format

Art-Net DMX (OpDmx = 0x5000):
- Header: "Art-Net\0" (8 bytes), OpCode (2), ProtVer (2), Sequence (1), Physical (1), Universe (2), Length (2)
- Data: channel bytes (max 512 = 170 RGB lights per universe)

Sequence field increments per frame (0-255, wrapping) so receivers can detect packet reordering.

## Universe Splitting

For >170 RGB lights, data is split across consecutive universes starting from `universe_start`.

## Socket

Opened in setup(), closed in teardown(). Uses platform UDP abstraction. Not a hot-path allocation.

## Tests

[Module test: ArtNet Packet](../../../testing.md#artnet) — header format, byte order, universe splitting.

[Scenario: base-pipeline](../../../testing.md#scenario-pipeline) — full pipeline with ArtNet output, performance bounds.

## Prior art

### MoonLight — D_NetworkOut ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Drivers/D_NetworkOut.h))
Supports ArtNet, E1.31, and DDP output. Multi-protocol in one driver.

### projectMM v1 — ArtNetOutModule ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/modules/drivers/ArtNetOutModule.h))
Controls: universe_start (slider 0-255), ip (text). Platform UDP abstraction.

### projectMM v2 — ArtnetOutModule ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/ArtnetOutModule.h))
Uses PalUdp abstraction. ADR 0005 teardown safety via DataBuffer invalidation.
