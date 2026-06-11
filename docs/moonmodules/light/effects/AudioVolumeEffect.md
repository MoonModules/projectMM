# AudioVolumeEffect

A VU meter on the whole grid: every light pulses with the microphone's sound **level**, its colour ramping from calm green (quiet) toward hot red (loud). The simplest audio-reactive effect — one scalar drives one brightness.

Reads the live frame from [MicModule](../../core/MicModule.md)`::latestFrame()`; with no microphone (or in silence) the level is zero and the grid stays dark, so the effect is safe on any target and grid size.

## Controls

- **brightness** — overall ceiling (1–255). Default 255.

## Notes

The colour is a level-driven green→red ramp; modifiers and layouts give the flat VU surface its shape. On an RGBW grid the white channel is left off.

## Source

[AudioVolumeEffect.h](../../../../src/light/effects/AudioVolumeEffect.h)
