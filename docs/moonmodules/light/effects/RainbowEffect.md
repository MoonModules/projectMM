# Rainbow 2D Effect

![RainbowEffect controls](../../../assets/screenshots/RainbowEffect.png)

![RainbowEffect preview](../../../assets/screenshots/RainbowEffect.gif)

Diagonal rainbow pattern across a 2D grid, animated over time. Good default/test effect — always produces visible, colorful output.

## Controls

- `speed` (uint8_t, default 60, range 1-255) — animation speed in BPM (beats per minute). 60 = 1 full cycle per second.

## Tests

[Unit tests: RainbowEffect](../../../tests/unit-tests.md#rainboweffect) — non-zero output, valid RGB, spatial variation.

[Scenario: scenario_Layer_base_pipeline](../../../tests/scenario-tests.md#scenario_layer_base_pipeline) — full pipeline with rainbow effect, performance bounds.

## Design notes

- Test effect. Dead simple — proves the pipeline works.
- No palette, no variants. Rainbow is visually recognizable, which makes it easy to spot in tests.

## Source

[RainbowEffect.h](../../../../src/light/effects/RainbowEffect.h)
