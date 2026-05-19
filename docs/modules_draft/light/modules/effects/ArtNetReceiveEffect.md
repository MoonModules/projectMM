# ArtNet Receive Effect

Receives ArtNet UDP packets and writes light data into the layer buffer,
behaving like any other effect.

## Controls

- `universe` (Uint16, default 0, range 0-32767) — listen universe
- `port` (Uint16, default 6454, range 1-65535) — listen port

## Status

Stub implementation only. Currently fills buffer with a test pattern
based on universe number. No actual UDP receiving implemented — requires
platform networking.

## Design

ArtNet receive as an effect (not a separate input mechanism) means:
- It writes into a layer buffer, same as any effect
- It can be combined with modifiers (mirror, rotate)
- It participates in layer blending
- The UI treats it like any other effect (selectable, has controls)

## What needs implementation

- Open UDP socket in setup(), poll in loop() (non-blocking, synchronous)
- Parse ArtNet DMX packets (OpDmx = 0x5000)
- Map received RGB data to buffer lights
- Handle multiple universes for >170 lights
- Platform UDP receive abstraction (complement to UdpSocket send)
