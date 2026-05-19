# Rainbow 2D Effect

Diagonal rainbow pattern across a 2D grid, animated over time. Good default/test effect — always produces visible, colorful output.

## Controls

- `speed` (slider, default 1, range 1-255) — animation speed

## Rendering

Uses `hsvToRgb(hue, 255, 255)` with hue derived from `x + y + elapsed_time * speed`. Always full brightness (v=255) and full saturation (s=255). Time-based animation via elapsed millis.

## Design notes

- No v1 equivalent (v1 used SineEffect as the primary test effect). Rainbow is simpler and more visually recognizable for testing.
- Pattern is simple (diagonal lines). Could be extended with palette support or radial/spiral variants.
- Works correctly with mirror modifier (renders into reduced logical space, LUT replicates to all quadrants).
