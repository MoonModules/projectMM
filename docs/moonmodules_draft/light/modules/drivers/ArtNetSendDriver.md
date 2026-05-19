# ArtNet Send Driver

Output driver. Reads from the DriverGroup's output buffer and sends ArtNet DMX packets over UDP.

## Controls

- `ip` (text, default "255.255.255.255") — destination IP address (broadcast or unicast)
- `universe_start` (slider, default 0, range 0-255) — first ArtNet universe

## ArtNet Packet Format

Art-Net DMX (OpDmx = 0x5000):
- Header: "Art-Net\0" (8 bytes), OpCode (2), ProtVer (2), Sequence (1), Physical (1), Universe (2), Length (2)
- Data: RGB bytes (max 512 = 170 lights per universe)

## Universe Splitting

For >170 lights, data is split across consecutive universes starting from `universe_start`.

## Packet Pacing (critical)

Without pacing, blasting all universe packets in a tight loop causes receivers to drop packets. The output appears broken (missing lights, random output). Two mechanisms required:
- **FPS limiting**: skip sending if called faster than target fps (control: fps slider, default 50)
- **Inter-packet delay**: microsecond pause between universe packets within a frame

## Edge cases

- Broadcast (255.255.255.255) vs unicast — broadcast works on local network but floods all devices
- Sequence field should increment per frame for receivers to detect reordering
- Socket opened in setup(), closed in teardown(). Not a hot-path allocation.

## Prior art

### MoonLight — D_NetworkOut ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Drivers/D_NetworkOut.h))
Supports ArtNet, E1.31, and DDP output. Multi-protocol in one driver.

### projectMM v1 — ArtNetOutModule ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/modules/drivers/ArtNetOutModule.h))
Controls: universe_start (slider 0-255), ip (text). Platform UDP abstraction.
v1 ArtNetOutModule (commit 54b50bc). Controls: `universe_start` (slider 0-255), `ip` (text). Used platform UDP abstraction.

### projectMM v2 — ArtnetOutModule ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/ArtnetOutModule.h))
Uses PalUdp abstraction ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/pal/PalUdp.h)). ADR 0005 teardown safety via DataBuffer invalidation.
