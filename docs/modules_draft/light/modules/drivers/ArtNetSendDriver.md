# ArtNet Send Driver

Output driver. Reads from the DriverGroup's output buffer and sends
ArtNet DMX packets over UDP.

## Controls

- `destIP` (Text, default "192.168.1.70") — destination IP address
- `startUniverse` (Uint16, default 0) — first ArtNet universe
- `fps` (Uint16, default 50, range 1-120) — send frame rate limit

## ArtNet Packet Format

Art-Net DMX (OpDmx = 0x5000):
- Header: "Art-Net\0" (8 bytes)
- OpCode: 0x5000 (little-endian)
- Protocol version: 14 (big-endian)
- Sequence, Physical, Universe, Length
- Data: RGB bytes (max 512 = 170 lights per universe)

## Universe Splitting

For >170 lights, data is split across consecutive universes. 128x128 =
16384 lights = 97 universes.

## Critical lesson: packet pacing

Without pacing, the UDP sendto loop blasts all 97 packets per frame
with zero delay. The ArtNet receiver drops packets and the output
shows missing data.

Two pacing mechanisms are needed:
1. **FPS limiter**: skip sending if called faster than the configured
   fps. Prevents sending more frames than the receiver can handle.
2. **Inter-packet delay**: 50us sleep between universe packets within
   a frame. Prevents the kernel UDP send buffer from overflowing.

## What worked

- ArtNet packet format is correct (verified with real receiver).
- Universe splitting works.
- FPS control prevents flooding.
- Platform UDP abstraction keeps socket code out of the driver.

## What needs improvement

- `buildPacket` doesn't set the sequence field. ArtNet receivers use
  sequence to detect packet reordering. Should increment per frame.
- No ArtNet poll/reply (device discovery). Currently requires manual
  IP configuration.
- `packet_` buffer (530 bytes) is on the stack. With multiple drivers,
  this adds up. Consider allocating once in setup.
- The 50us inter-packet delay is a `std::this_thread::sleep_for` which
  is not available on ESP32 FreeRTOS. Need a platform timing
  abstraction for microsecond delays.
- No support for ArtNet sequence numbering across universes.
