# BLE provisioning upstream rebase

## Context

The BLE provisioning branch predates the current lifecycle names and System/Services split. Its core module also includes the platform API directly, which crosses the repository's platform boundary. Security0 remains necessary until hardware carries a real out-of-band secret, so the BLE service must be restricted to the device's AP fallback onboarding state.

## Alternatives

- Keep direct platform calls: removes adapter code, but violates the platform boundary and leaves core behavior hard to test.
- Inject a plain function table: adds one small data struct, follows the reviewed auto-update pattern, and enables desktop unit tests.
- Fold BLE into Improv provisioning: removes one module, but combines separate Espressif and Improv protocols and expands the change well beyond the review feedback.

## Choice

Inject a plain function table. Keep BLE as fixed Network infrastructure, advertise only while Network is in AP fallback, and preserve Network as the sole owner of credential persistence and reconnect state.

## Work

1. Port the module to `defineControls`, `tick1s`, and `release`.
2. Inject platform start/stop/time/chip functions from `main.cpp`.
3. Add desktop unit coverage for AP-gated startup, retry, credential publication, password clearing, and release.
4. Run spec, platform-boundary, desktop, unit, scenario, and ESP32 checks relevant to the branch.
5. Test provisioning over Windows BLE on hardware, then update the PR review threads with verified results.
