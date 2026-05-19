# Light Value

NOT a fixed 3-byte RGB struct. The system must support RGB (3 bytes), RGBW (4 bytes), and multi-channel DMX fixtures (up to 32 channels per light).

## Design

Follow MoonLight's LightsHeader approach: the buffer is raw `uint8_t*` with configurable `channelsPerLight` and per-channel offsets. There is no `RGB` struct that all code passes around — instead, effects and drivers work with the buffer directly using the channel configuration.

Channel offsets define where each color/function channel lives within a light's data:
- `offsetRed`, `offsetGreen`, `offsetBlue` — primary RGB
- `offsetWhite` — white channel (RGBW, RGBCCT)
- `offsetBrightness` — separate brightness channel (PAR lights)
- `offsetPan`, `offsetTilt`, `offsetZoom`, `offsetRotate`, `offsetGobo` — moving heads

This means the same buffer and pipeline handles LEDs and DMX fixtures without separate code paths.

## Color math utilities

Pure functions (not tied to a struct):
- `hsvToRgb(h, s, v)` — integer HSV to RGB (6-sector, no floats)
- `scale8(val, scale)` — `(val * scale) >> 8`
- `blend(r1,g1,b1, r2,g2,b2, amount)` — per-channel lerp

These operate on individual channel values, not on a struct. They live in core (platform-independent).

## Prior art

### MoonLight — LightsHeader ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Layers/LightsHeader.h))
48-byte metadata struct: `channelsPerLight` (3-32), per-channel offsets for RGB, RGBW, RGBCCT, brightness, pan/tilt/zoom/rotate/gobo. One struct handles LEDs AND DMX fixtures. Sent to frontend for preview rendering.

### projectMM v1 — RGB ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/modules/layers/RGB.h))
Plain 3-byte RGB struct. No FastLED dependency. Limited to RGB only.

### projectMM v2 — RGB ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/RGB.h))
Same 3-byte struct. Still limited to RGB.
