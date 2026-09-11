// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once


/// @file MoonCloudModule.h
/// MoonCloud: the container for everything projectMM does with a server we run.
///
/// Holds no controls and does no work. Each thing MoonCloud does is a CHILD with its own consent:
/// Stats (one report per install or upgrade), Talk (a public message board), Sync (planned).
/// A user who wants a joint lightshow has not agreed to usage reporting, and one switch could not
/// express that.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/MoonModule.h"
#include "core/sha256.h"
#include "platform/platform.h"

namespace mm {

class MoonCloudModule : public MoonModule {
public:
    /// Always runs: the children carry the real consent, so disabling the container is ambiguous.
    bool respectsEnabled() const MM_NONBLOCKING override { return false; }

    static constexpr uint32_t kTimeoutMs = 4000;

    /// The one outbound call in MoonCloud. HTTPS only: the address is a compiled-in constant, so a
    /// plain-HTTP branch could never run and would be a second transport nobody tests.
    bool post(const char* path, const char* body) const {
        char url[192];
        std::snprintf(url, sizeof(url), "https://%s%s", kHost, path);
        return platform::httpsPost(url, body, kTimeoutMs);
    }

    void defineControls() override {
        controls_.clear();
        // Chain FIRST so children register before this module's own controls. Without it a child of
        // a container shows an empty card.
        MoonModule::defineControls();
    }

private:
    // Compiled in rather than configurable: a device that can be pointed elsewhere can be pointed
    // at nothing, and a failed report is never retried. Moving the server is a release.
    //
    // The workers.dev name rather than a custom domain. Cloudflare picks the CA for a Custom Domain
    // certificate, and the one it picked is absent from IDF's default root bundle, so every ESP32
    // handshake failed while desktop's system trust store accepted it. The address is compiled in
    // and never shown, so a prettier one buys nothing a device can reach.
    static constexpr const char* kHost = "mooncloud-stats.moonmodules.workers.dev";
};


/// The installation id: `SHA-256(salt || MAC)` truncated to 16 bytes, 32 hex characters.
///
/// One scheme on every target, because the platform layer already answers "what is this
/// installation": an eFuse MAC on ESP32, a stored random address on desktop and Docker. It lives in
/// core rather than behind the platform seam, so a new target inherits a correct id by implementing
/// `getMacAddress` alone.
///
/// The salt differs from every other salt in the project on purpose. The same MAC produces the MQTT
/// topic prefix and the Home Assistant `unique_id`, both visible on the user's own network; a
/// distinct salt is what stops a report being tied to a device somebody can observe locally.
///
/// NOT anonymous: it is stable, so two reports carrying it came from one install. That is the point,
/// and why [privacy-policy.md](../../docs/privacy-policy.md) calls it pseudonymous.

/// Characters written by `installationId`, excluding the terminator.
inline constexpr size_t kInstallationIdChars = 32;

/// Changing this re-identifies every installation in the world exactly once, so it is fixed.
inline constexpr char kMoonCloudSalt[] = "projectMM/MoonStats/v1";

/// Write the installation id into `out`, which must hold `kInstallationIdChars + 1` characters.
///
/// Computed per call rather than cached: it is wanted once per install, and a cache would need
/// invalidating when `fsSetRoot` moves the identity (which tests do between cases).
inline void installationId(char* out) {
    if (!out) return;

    uint8_t mac[6] = {};
    platform::getMacAddress(mac);

    // Concatenation rather than HMAC: the salt is public in this source either way, so it is a
    // domain separator rather than a key.
    uint8_t buf[sizeof(kMoonCloudSalt) - 1 + sizeof(mac)];
    std::memcpy(buf, kMoonCloudSalt, sizeof(kMoonCloudSalt) - 1);
    std::memcpy(buf + sizeof(kMoonCloudSalt) - 1, mac, sizeof(mac));

    sha256Hex(buf, sizeof(buf), out, kInstallationIdChars / 2);
}

}  // namespace mm
