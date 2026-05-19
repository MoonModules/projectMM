# Light Value (RGB)

3-byte RGB struct. The fundamental color type.

## API

- `RGB{r, g, b}` — construction
- `RGB::black()`, `RGB::white()` — constexpr constants
- `RGB::fromHSV(h, s, v)` — integer HSV to RGB (6-sector, no floats)
- `scale8(val, scale)` — `(val * scale) >> 8`
- `blend(a, b, amount)` — per-channel lerp via scale8

## What worked

- `static_assert(sizeof(RGB) == 3)` — guaranteed packed.
- All math is constexpr integer. No floats on hot path.
- `fromHSV` produces correct colors for all hue values at full
  saturation and value.

## What needs improvement

- `fromHSV` with value < 255 can produce very dark lights that appear
  black on LEDs. Effects should generally use full brightness (v=255)
  and vary hue/saturation.
- No RGBW support yet. Needed for RGBW LED strips and white-channel
  DMX fixtures.
- `blend()` has potential issues with uint8_t underflow in `b.r - a.r`
  when b < a. The cast to uint8_t wraps around, and scale8 on the
  wrapped value gives wrong results. Needs a proper signed lerp.

## Prior art

### MoonLight — RGB in LightsHeader ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Layers/LightsHeader.h))
Not just RGB — supports RGBW, RGBCCT, and multi-channel DMX fixtures via configurable `channelsPerLight` (3-32) and per-channel offsets (offsetRed, offsetGreen, offsetBlue, offsetWhite, offsetPan, offsetTilt, etc.). One struct handles LEDs AND DMX fixtures.

### projectMM v1 — RGB ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/modules/layers/RGB.h))
Plain RGB struct, no FastLED dependency. `hsvToRgb()` inline.

### projectMM v2 — RGB ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/RGB.h))
Same plain struct.
