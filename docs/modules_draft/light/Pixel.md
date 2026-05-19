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
