#pragma once

#include "core/DeviceIdentify.h"   // DevType
#include "core/WledPacket.h"       // the 65506 presence packet projectMM + WLED both use

#include <cstdio>
#include <cstring>

// Device-interop plugin seam — how a foreign lighting/IoT system "hooks in" to
// projectMM's device discovery. DevicesModule owns the device model + the UDP discovery
// listener and stays domain-neutral; each *plugin* teaches it to recognise one ecosystem
// (our own projectMM, WLED, later ESPHome / Tasmota / Hue) from a UDP presence broadcast.
// This is the adapter pattern (cf. the ListSource data-source seam, ModuleFactory's
// register-by-name): the core is generic, the per-system knowledge lives with its plugin,
// so a new system is *one new plugin file*, never a core edit.
//
// Discovery is PASSIVE UDP: a plugin declares the broadcast port it listens on and
// classifies a received datagram into a device. This replaces the former mDNS *query*
// path, which destabilised our own mDNS advertise (a PTR query for a service we also host
// exhausts the IDF mDNS pool — see docs/history/decisions.md). mDNS is
// now advertise-only (so the WLED app + Home Assistant find us); discovery never queries.
//
// The seam covers the discovery half, with two concrete plugins (projectMM and WLED) that
// prove it isn't shaped to one system. It is sized to also carry a control half (a per-plugin
// `command()` that translates "set brightness" into a system's protocol) without reshaping —
// that's why `DiscoveredDevice` stays plain and the iteration is generic.

namespace mm {

// A device a plugin recognised from a presence packet. Plain data; the IP comes from the
// datagram source, the plugin fills the kind + name. (A future hub plugin adds a resource
// list here; the command half adds capability/auth.)
struct DiscoveredDevice {
    DevType type = DevType::Generic;
    char    name[24] = {};
};

// Copy a discovered name into a DiscoveredDevice, truncating to the display-name field.
// `%.*s` bounds the read so truncation is explicit and the format-truncation check passes.
inline void setDeviceName(DiscoveredDevice& d, const char* src) {
    std::snprintf(d.name, sizeof(d.name), "%.*s",
                  static_cast<int>(sizeof(d.name) - 1), src ? src : "");
}

// One interop plugin. Stateless const singleton — no per-device state (that lives in the
// module's list). A plugin declares the UDP port it listens on and turns a received
// presence datagram into a device.
class DevicePlugin {
public:
    virtual ~DevicePlugin() = default;

    // A short label for logs / the UI ("projectMM", "WLED"). Flash-literal lifetime.
    virtual const char* label() const = 0;

    // The UDP port this plugin's ecosystem broadcasts presence on. The bundled plugins
    // share one port (projectMM + WLED both use 65506), and DevicesModule enforces that
    // invariant: it binds a single listener to the plugins' common discoveryPort() and
    // offers every datagram to all of them.
    virtual uint16_t discoveryPort() const = 0;

    // Classify a received datagram (`data`/`len`) from `srcIp`. Returns true and fills
    // `out` when this plugin owns the packet; false to decline (let another plugin handle
    // it). Defensive per the robustness contract: a short/garbage datagram → decline,
    // never read out of bounds, never crash.
    virtual bool classifyPacket(const uint8_t* data, size_t len, const uint8_t srcIp[4],
                                DiscoveredDevice& out) const = 0;

    // (reserved) Translate a generic command (set brightness, …) into this system's
    // protocol and send it. Added when a control consumer exists; not built now.
    //   virtual bool command(const DiscoveredDevice& dev, const DeviceCommand& cmd) const;
};

// --- projectMM plugin: a peer's WLED-valid presence packet carrying our `MM` marker. ---
// projectMM broadcasts a WLED-VALID 65506 packet (so WLED apps list us too) stamped with a
// sentinel in the version field. This plugin claims a packet ONLY if that marker is present
// — so a peer projectMM device is typed projectMM, and the WledPlugin (below) doesn't also
// claim it as a generic WLED. Listed first for that priority.
class MmPlugin : public DevicePlugin {
public:
    const char* label() const override { return "projectMM"; }
    uint16_t discoveryPort() const override { return WledPacket::kPort; }

    bool classifyPacket(const uint8_t* data, size_t len, const uint8_t /*srcIp*/[4],
                        DiscoveredDevice& out) const override {
        if (!WledPacket::isValid(data, len) || !WledPacket::hasMmMarker(data, len)) return false;
        out.type = DevType::ProjectMM;
        char name[24];
        WledPacket::readName(data, name, sizeof(name));
        setDeviceName(out, name);
        return true;
    }
};

// --- WLED plugin: any valid WLED presence packet WITHOUT our marker. ---------------
class WledPlugin : public DevicePlugin {
public:
    const char* label() const override { return "WLED"; }
    uint16_t discoveryPort() const override { return WledPacket::kPort; }

    bool classifyPacket(const uint8_t* data, size_t len, const uint8_t /*srcIp*/[4],
                        DiscoveredDevice& out) const override {
        if (!WledPacket::isValid(data, len)) return false;
        if (WledPacket::hasMmMarker(data, len)) return false;   // that's a projectMM peer
        out.type = DevType::Wled;
        char name[24];
        WledPacket::readName(data, name, sizeof(name));
        setDeviceName(out, name);
        return true;
    }
};

}  // namespace mm
