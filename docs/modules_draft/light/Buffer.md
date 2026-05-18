# Buffer

Contiguous pixel array. The data structure effects write into and
drivers read from.

## API

- `allocate(count)` — allocate via `platform::alloc`, clears to black
- `free()` — safe to call multiple times
- `clear()` — memset to 0
- `fill(RGB)` — fill all pixels with one color
- `operator[]` — read/write access
- `pixels()` — `std::span<RGB>` view
- Move-constructible, not copyable.

## What worked

- Uses `platform::alloc`/`platform::free` — portable across platforms.
- Reallocating (calling allocate with different size) frees old buffer
  first and clears new buffer.
- Move semantics work correctly.

## What needs improvement

- `allocate()` is called on the hot path when layout/modifier changes
  trigger `rebuildLUT()`. This is cold-path by design but happens
  during frame processing if triggered by HTTP API. Could cause a
  frame stutter.
- No RGBW variant. Would need templating or a separate class.
