#pragma once

// I2cScanModule — a diagnostic that scans an I2C bus and reports which device
// addresses ACK (the standard `i2cdetect`). Domain-neutral: any I2C bring-up
// (an audio codec, a sensor, a port expander) uses it to confirm wiring and
// read off a device's address. Pressing `scan` probes the bus on the `sda` /
// `scl` pins and lists the 7-bit addresses found in `result`.
//
// Same shape as DevicesModule (a momentary `scan` button → results), one rung
// simpler: the bus is local (no persisted list, no live age-out), so a single
// read-only `result` string suffices instead of a ListSource.
//
// The pins are controls (defaulting unset) so each board sets its bus pins in
// docs/install/deviceModels.json — the same per-board pin-config pattern as the
// driver/audio modules. The actual probe is platform::i2cScan (platform.h), a
// self-contained seam that opens a temporary bus, scans, and tears it down, so
// the diagnostic never fights a bus another driver owns.

#include "core/MoonModule.h"
#include "platform/platform.h"  // i2cScan

#include <cstdint>
#include <cstdio>
#include <cstring>  // strcmp

namespace mm {

class I2cScanModule : public MoonModule {
public:
    // A diagnostic, like FirmwareUpdateModule / DevicesModule — keeps its
    // controls and the scan action available regardless of the `enabled` toggle.
    bool respectsEnabled() const override { return false; }

    ModuleRole role() const override { return ModuleRole::Peripheral; }

    void onBuildControls() override {
        controls_.addPin("sda", sda_);
        controls_.addPin("scl", scl_);
        controls_.addButton("scan");
        controls_.addReadOnly("result", resultStr_, sizeof(resultStr_));
        MoonModule::onBuildControls();
    }

    void onUpdate(const char* controlName) override {
        if (std::strcmp(controlName, "scan") == 0) scan();
    }

private:
    // Default to GPIO21/22 — the Arduino-ESP32 core's default I2C pair, the pins a
    // contributor expects to try first on a classic ESP32. They're a *convention*,
    // not fixed hardware (I2C routes through the GPIO matrix to any pins), so they
    // pre-fill the control as a sensible starting point the user edits. A board
    // with a FIXED bus (the S31 codec, the P4) overrides them via its catalog entry.
    int8_t sda_ = 21;
    int8_t scl_ = 22;
    char resultStr_[64] = "";    // space-separated hex addresses, e.g. "0x18 0x3c"
    char statusBuf_[40] = "idle";

    void scan() {
        if (sda_ < 0 || scl_ < 0) {
            resultStr_[0] = '\0';
            setStatus("set sda + scl pins first", Severity::Warning);
            return;
        }
        uint8_t found[kMaxAddrs];
        const size_t n = platform::i2cScan(static_cast<uint16_t>(sda_),
                                           static_cast<uint16_t>(scl_),
                                           found, kMaxAddrs);
        if (n == platform::kI2cBusUnavailable) {
            // The bus is held by another driver (e.g. the ES8311 codec while
            // AudioModule is active) — say so instead of a misleading "0 found".
            resultStr_[0] = '\0';
            setStatus("bus in use — free the I2C driver, then scan", Severity::Warning);
            markDirty();
            return;
        }
        // Build the "0x18 0x3c …" result string, truncating cleanly if the buffer
        // fills (more devices than fit is unusual on one bus, but stay bounded).
        int pos = 0;
        for (size_t i = 0; i < n; i++) {
            const int w = std::snprintf(resultStr_ + pos, sizeof(resultStr_) - pos,
                                        "%s0x%02x", i ? " " : "", found[i]);
            if (w <= 0 || pos + w >= static_cast<int>(sizeof(resultStr_))) break;
            pos += w;
        }
        if (n == 0) resultStr_[0] = '\0';

        std::snprintf(statusBuf_, sizeof(statusBuf_), "%u device%s found",
                      static_cast<unsigned>(n), n == 1 ? "" : "s");
        setStatus(statusBuf_);
        markDirty();   // push the updated result + status to the UI
    }

    static constexpr size_t kMaxAddrs = 16;  // plenty for one bus
};

} // namespace mm
