// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once


/// @file MoonCloudModule.h
/// MoonCloud: the container for everything projectMM does with a server we run.
///
/// It holds no controls of its own and does no work. It exists so that each thing MoonCloud does
/// is a CHILD with its own consent, rather than one module whose meaning widens:
///
/// - **Stats** (`MoonStatsModule`): one report per install or upgrade, and the aggregates back.
/// - **Sync** (planned): device to device over the internet, a joint show across houses.
///
/// **One module per promise.** A user who wants a joint lightshow has not agreed to usage
/// reporting, and a user who shares their chip model has not agreed to accept connections from
/// other people's devices. Separate children keep those separate questions, separately answered
/// and separately revocable, which a single "MoonCloud: on/off" switch could not express.
///
/// Sync is NOT an extension of Stats and will not be built on it: Stats is one fire-and-forget POST
/// per firmware install, where Sync needs persistent connections, time alignment, presence and
/// pairing. They share this container and the installation id, and nothing else. See
/// [the MoonCloud plan](../../docs/history/plans/Plan-20260910%20-%20MoonCloud.md).

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/MoonModule.h"
#include "core/sha256.h"
#include "platform/platform.h"

namespace mm {

class MoonCloudModule : public MoonModule {
public:
    /// A container with nothing to disable: turning MoonCloud "off" would be ambiguous when its
    /// children carry the real consent, so each child owns its own answer and this one always runs.
    bool respectsEnabled() const MM_NONBLOCKING override { return false; }

    /// How long any member waits for the server. One bounded call, off the render path.
    static constexpr uint32_t kTimeoutMs = 4000;

    /// POST `body` to `path`. The one outbound call in MoonCloud, so every member sends through
    /// here rather than repeating the client API.
    ///
    /// HTTPS only, because the address is a constant and that constant is public. An earlier shape
    /// also carried a plain-HTTP branch for a local server, reached by editing a control; with the
    /// address compiled in that branch could never run, and an unreachable transport is a second
    /// way to send that nobody tests.
    bool post(const char* path, const char* body) const {
        char url[192];
        std::snprintf(url, sizeof(url), "https://%s%s", kHost, path);
        return platform::httpsPost(url, body, kTimeoutMs);
    }

    void defineControls() override {
        controls_.clear();
        // On the CONTAINER, so one setting serves every member. A child registering its own would
        // put the same field on two cards and let them drift apart.
        // Chain FIRST so children register before any control of this module's own, per the
        // override-and-chain convention (docs/coding-standards.md). Without this a child of a
        // container shows an empty card, which is exactly what happened when MoonStats was
        // briefly parented to Firmware, whose override does not chain.
        MoonModule::defineControls();
    }

private:
    // THE address, compiled in rather than configurable. There is one MoonCloud, and a device that
    // could be pointed elsewhere is a device that can be pointed at nothing: a typo means reports
    // vanish silently, since a failed report is never retried. Moving the server is a release.
    static constexpr const char* kHost = "stats.moonmodules.org";
    static constexpr uint16_t kPort = 443;
};


// ---------------------------------------------------------------------------------------------
// The installation id: MoonCloud's shared identity, used by every member.
//
// The MoonStats installation id: 32 hex characters identifying this install, and nothing else.
//
// `SHA-256(salt || platform::getMacAddress())`, truncated to 16 bytes, exactly as
// [the MoonCloud plan](../../docs/history/plans/Plan-20260910 - MoonCloud.md) specifies. ONE
// scheme on every target, because the platform layer already answers "what is this installation"
// per platform: an eFuse MAC on ESP32, and since PR #98 a stored random address on desktop and in
// Docker. This code does not care which it got.
//
// **It lives in core, not behind the platform seam.** There is nothing platform-specific left once
// the seed is in hand: hashing six bytes is the same arithmetic everywhere, and putting it here
// means a new target inherits a correct id by implementing `getMacAddress` alone.
//
// **The salt is MoonStats-specific and deliberately differs from every other salt in the project.**
// The same MAC produces the MQTT topic prefix and the Home Assistant `unique_id`
// ([ADR-0010](../../docs/adr/0010-integration-identity-stable-hardware-id.md)), both visible on the
// user's own network. Hashing with a distinct salt is what stops a report being tied to a device
// somebody can observe locally.
//
// What this is NOT: anonymous. It is stable, so two reports carrying it came from one install,
// which is the entire point and also why [privacy-policy.md](../../docs/privacy-policy.md) calls it
// pseudonymous and says an ESP32 id is reversible in principle under a published salt.

/// Characters written by `installationId`, excluding the terminator.
inline constexpr size_t kInstallationIdChars = 32;

/// Write the installation id into `out`, which must hold `kInstallationIdChars + 1` characters.
///
/// Computed on every call rather than cached: it is wanted once per install, and a cache would be
/// state that has to be invalidated when `fsSetRoot` moves the identity (which tests do between
/// cases).
// `inline` because this lives in a header: one definition shared by every translation unit that
// includes it, rather than one per unit.
//
// The salt is MoonCloud-wide and changing it re-identifies every installation in the world exactly
// once, so it is a fixed constant. It differs from every other salt in the project, which is what
// stops a report being correlated with the MQTT identity built from the same MAC.
inline constexpr char kMoonCloudSalt[] = "projectMM/MoonStats/v1";

inline void installationId(char* out) {
    if (!out) return;

    uint8_t mac[6] = {};
    platform::getMacAddress(mac);

    // salt || mac, hashed as one buffer. Concatenation rather than HMAC: the salt is public in this
    // source either way, so it is a domain separator rather than a key, and HMAC would imply a
    // secret that does not exist.
    uint8_t buf[sizeof(kMoonCloudSalt) - 1 + sizeof(mac)];
    std::memcpy(buf, kMoonCloudSalt, sizeof(kMoonCloudSalt) - 1);
    std::memcpy(buf + sizeof(kMoonCloudSalt) - 1, mac, sizeof(mac));

    sha256Hex(buf, sizeof(buf), out, kInstallationIdChars / 2);
}

}  // namespace mm
