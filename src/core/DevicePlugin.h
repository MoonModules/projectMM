#pragma once

#include "core/DeviceIdentify.h"   // DevType
#include "platform/platform.h"     // MdnsHost — the discovery hit a plugin classifies

#include <cstdio>
#include <cstring>

// Device-interop plugin seam — how a foreign lighting/IoT system "hooks in" to
// projectMM's device discovery. DevicesModule owns the device model + the mDNS
// listener and stays domain-neutral; each *plugin* teaches it to recognise one
// ecosystem (our own projectMM, WLED, later ESPHome / Tasmota / Hue) from an mDNS
// announcement. This is the adapter pattern (cf. the ListSource data-source seam,
// ModuleFactory's register-by-name): the core is generic, the per-system knowledge
// lives with its plugin, so a new system is *one new plugin file*, never a core edit.
//
// Built minimal-but-real now (the discovery half: claim a service + classify a hit,
// with two concrete plugins — projectMM and WLED — proving the seam isn't shaped to
// any one system). The control half (translate a "set brightness" command into a
// system's protocol) is a reserved extension: when a consumer for it exists,
// `command()` is added to this base and implemented per plugin — the discovery code
// and DevicesModule's iteration don't change. Concrete first, abstract later.
//
// Plugins are NOT all the same shape: a flat-device plugin (WLED, ESPHome) yields
// one device per hit; a hub plugin (Hue) would expand one bridge hit into several
// controllable resources + carry auth state. The seam keeps `DiscoveredDevice` plain
// so a future hub plugin can extend it (a resource list) without reshaping the flat case.

namespace mm {

// A device a plugin recognised from an mDNS hit. Plain data; the IP/name come from
// the hit, the plugin fills the kind. (A future hub plugin adds a resource list here;
// the command half adds capability/auth.)
struct DiscoveredDevice {
    DevType type = DevType::Generic;
    char    name[24] = {};
};

// Copy a discovered name into a DiscoveredDevice, truncating if the source (an mDNS
// hostname, up to 32 B) is longer than the device's display-name field — the `%.*s`
// precision bounds the read so the truncation is explicit and the compiler's
// format-truncation check is satisfied (a long advertised name just shows clipped).
inline void setDeviceName(DiscoveredDevice& d, const char* src) {
    std::snprintf(d.name, sizeof(d.name), "%.*s",
                  static_cast<int>(sizeof(d.name) - 1), src ? src : "");
}

// One interop plugin. Stateless const singleton — no per-device state (that lives in
// the module's list). A plugin declares the mDNS service it listens on and turns a
// resolved hit on that service into a device.
class DevicePlugin {
public:
    virtual ~DevicePlugin() = default;

    // A short label for logs / the UI ("projectMM", "WLED"). Flash-literal lifetime.
    virtual const char* label() const = 0;

    // The mDNS service + proto this plugin claims (e.g. "_wled", "_tcp"). DevicesModule
    // rotates its mDNS listener through the distinct services its plugins declare.
    virtual const char* service() const = 0;
    virtual const char* proto() const = 0;

    // Classify a resolved mDNS hit. Returns true and fills `out` when this plugin owns
    // the hit; false to decline (let another plugin / the generic fallback handle it).
    // Defensive against any input per the robustness contract (a hit on the claimed
    // service with unexpected fields → decline, never crash).
    virtual bool classify(const platform::MdnsHost& host, DiscoveredDevice& out) const = 0;

    // (reserved) Translate a generic command (set brightness, …) into this system's
    // protocol and send it. Added when a control consumer exists; not built now.
    //   virtual bool command(const DiscoveredDevice& dev, const DeviceCommand& cmd) const;
};

// --- projectMM plugin: a peer advertising `_http._tcp` with the `mm=1` TXT. --------
// Registered first so a projectMM device (which also matches the generic _http._tcp
// service) is claimed as projectMM, not left generic.
class MmPlugin : public DevicePlugin {
public:
    const char* label() const override { return "projectMM"; }
    const char* service() const override { return "_http"; }
    const char* proto() const override { return "_tcp"; }

    bool classify(const platform::MdnsHost& host, DiscoveredDevice& out) const override {
        if (!host.isProjectMM) return false;   // a generic _http._tcp box — not us
        out.type = DevType::ProjectMM;
        setDeviceName(out, host.hostname);
        return true;
    }
};

// --- WLED plugin: a device advertising `_wled._tcp`. ------------------------------
class WledPlugin : public DevicePlugin {
public:
    const char* label() const override { return "WLED"; }
    const char* service() const override { return "_wled"; }
    const char* proto() const override { return "_tcp"; }

    bool classify(const platform::MdnsHost& host, DiscoveredDevice& out) const override {
        // Anything answering on _wled._tcp is a WLED device (its hostname is its name).
        out.type = DevType::Wled;
        setDeviceName(out, host.hostname);
        return true;
    }
};

}  // namespace mm
