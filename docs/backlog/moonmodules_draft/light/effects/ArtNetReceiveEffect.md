# ArtNet Receive Effect

Receives ArtNet UDP packets and writes light data into the layer buffer, behaving like any other effect. This means it can be combined with modifiers, participates in layer blending, and is selectable/configurable through the same UI as other effects.

## Controls

- `universe_start` (slider, default 0, range 0-255) — first universe to listen on
- `port` (slider, default 6454, range 1-65535) — UDP listen port

## Rendering

Opens a UDP socket in setup(). In loop(), polls for pending ArtNet packets (non-blocking, synchronous). Parses OpDmx packets and copies RGB data into the layer buffer. Handles multiple universes for >170 lights.

## Design notes

- ArtNet receive as an effect (not a separate input mechanism) is a key architectural choice. It means any external light source is just another MoonModule that writes into a layer buffer.
- Platform UDP receive abstraction needed (complement to the existing UDP send).
- Processing is synchronous at the frame boundary — check for pending packets, don't block.

## Prior art

### MoonLight — D_NetworkIn ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Drivers/D_NetworkIn.h))

ArtNet/E1.31/DDP receive as a driver node. Supports multiple protocols.

### projectMM v1 — ArtNetInModule ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/modules/effects/ArtNetInModule.h))

v1 ArtNetInModule (commit 54b50bc). Control: `universe_start` (slider 0-255). Treated as an effect within a layer.
ArtNet receive as an effect. Control: universe_start.
