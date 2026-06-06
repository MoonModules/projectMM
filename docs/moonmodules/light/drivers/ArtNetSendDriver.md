# ArtNet Send Driver

![ArtNetSendDriver controls](../../../assets/screenshots/ArtNetSendDriver.png)

Output driver. Reads from the Drivers container's output buffer and sends ArtNet DMX packets over UDP. The driver doesn't care whether the buffer is a separate composed buffer or a shared Layer buffer — it reads from whatever the Drivers container provides.

The UDP socket is `connect()`-bound to the destination in `setup()`, so each per-universe `sendTo()` skips the address parse + route lookup — a measurable saving when a frame spans dozens of universes (16,384 lights = 97 universes). See [performance.md](../../../performance.md) "ArtNet UDP send cost".

**Synchronous send (throughput-bound at large grids).** The send is synchronous in the render loop — one UDP packet per universe. A full 128×128 frame is ~97 universes (~50 KB); pushing that through the ESP32 TX path takes real wall-clock time (measured ~35 ms over Ethernet, ~90 ms over WiFi) that is charged to the render tick, so ArtNet dominates the tick at large grids. This is a transport throughput limit, not a code path that a non-blocking socket can shed: lwIP blocks UDP TX in the netif/driver layer below the socket API, so neither `O_NONBLOCK` nor `MSG_DONTWAIT` makes a full-frame send return early (verified on hardware). For high frame rates at large grids, use Ethernet over WiFi, or a smaller grid. See [performance.md](../../../performance.md) for the measured per-transport send cost.

## Controls

- `ip` (ipv4, default "192.168.1.70") — destination IP address. Stored as 4 octets device-side (`uint8_t[4]`), formatted to a dotted-quad string only at the wire boundary. See [coding-standards.md § Prefer integers](../../../coding-standards.md#prefer-integers-store-values-in-their-native-shape).
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

[Unit tests: ArtNetSendDriver](../../../tests/unit-tests.md#artnetsenddriver) — header format, byte order, universe splitting.

[Scenario: scenario_Layer_base_pipeline](../../../tests/scenario-tests.md#scenario_layer_base_pipeline) — full pipeline with ArtNet output, performance bounds.

## Prior art

### MoonLight — D_NetworkOut ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Drivers/D_NetworkOut.h))

Supports ArtNet, E1.31, and DDP output. Multi-protocol in one driver.

### projectMM v1 — ArtNetOutModule ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/modules/drivers/ArtNetOutModule.h))

Controls: universe_start (slider 0-255), ip (text). Platform UDP abstraction.

### projectMM v2 — ArtnetOutModule ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/ArtnetOutModule.h))

Uses PalUdp abstraction. ADR 0005 teardown safety via DataBuffer invalidation.
