# Game of Life Effect

Conway's Game of Life on the XY plane.

## Controls

- `seed` (slider, default 42, range 0-255) — PRNG seed for initial state
- `wraparound` (toggle, default false) — whether edges wrap
- `hue` (slider, default 160, range 0-255) — base colour hue (spatial offset added per cell)

## Rendering

Two uint8_t grids (cur/nxt) allocated in setup (in PSRAM when available). Standard GoL rules (B3/S23). On extinction (alive==0) or stasis (changed==0), the grid re-seeds automatically. Living cells are colored with `hsvToRgb(hue + x*3 + y*5, 200, 255)`.

## Memory

Allocates 2 × width × height bytes for the cell grids. At 128x128 = 32KB. Freed in teardown.

## Edge cases

- Grid reallocation needed when layout dimensions change (control `onUpdate`).
- `srand(seed)` is not thread-safe. Consider a local PRNG state.

## Prior art

### projectMM v1 — GameOfLifeEffect ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/modules/effects/GameOfLifeEffect.h))
v1 GameOfLifeEffect (commit 54b50bc). Used `pal::psram_malloc` for grids. Had test helpers: `setPattern()`, `getCell()`, `liveCount()`, `stepGeneration()` for deterministic testing without rendering.
Full implementation with test helpers (setPattern, getCell, liveCount, stepGeneration).
