# Auto-update upstream rebase

## Goal

Rebase the manifest-driven automatic updater onto current `main` while preserving the reviewed security and OTA-lifecycle fixes.

## Plan

1. Keep the current System/Services ownership split and place automatic updates beside firmware updates under System.
2. Adapt the module to the current `defineControls`, `onControlChanged`, and `tick1s` lifecycle without changing its manifest or OTA behavior.
3. Run the focused unit tests, the desktop build, and the applicable repository checks before updating the pull request branch.

## Subtraction

The rebase removes the obsolete string-derived OTA-in-flight check. `OtaUpdateState` remains the single atomic lifecycle gate shared by every OTA entry point.
