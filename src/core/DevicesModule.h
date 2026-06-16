#pragma once

#include "core/MoonModule.h"
#include "core/Control.h"
#include "core/JsonSink.h"
#include "core/DeviceIdentify.h"   // DevType, classifyDevice, extractDeviceName (pure, unit-tested)
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

// Discovers other devices on the LAN, identifies what each is, and presents them
// as a browsable list. Core + domain-neutral: it finds "a projectMM / a WLED / a
// generic HTTP device", and light modules (Art-Net sync, future SuperSync) consume
// the list rather than living here. Submodule of NetworkModule — discovery depends
// on the network being up. See docs/moonmodules/core/DevicesModule.md.
//
// v1: a throttled subnet sweep (a few IPs per loop1s tick, never blocking the
// render loop) HTTP-probes each host and classifies the response; results render
// in the generic List control (this module is its ListSource). Read-only.
class DevicesModule : public MoonModule, public ListSource {
public:
    // ListSource — rows are produced straight from devices_ (no copy, no alloc).
    uint8_t listRowCount() const override { return deviceCount_; }

    void writeListRow(JsonSink& sink, uint8_t row) const override {
        if (row >= deviceCount_) { sink.append("{}"); return; }
        const Device& d = devices_[row];
        char ip[16];
        formatDottedQuad(ip, d.ip);
        sink.append("{\"name\":");
        sink.writeJsonString(d.name[0] ? d.name : ip);
        sink.appendf(",\"ip\":\"%s\",\"type\":\"%s\"", ip, devTypeStr(d.type));
        if (d.self) sink.append(",\"self\":true");
        sink.append("}");
    }

    void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
        if (row >= deviceCount_) { sink.append("{}"); return; }
        const Device& d = devices_[row];
        char ip[16];
        formatDottedQuad(ip, d.ip);
        sink.append("{\"name\":");
        sink.writeJsonString(d.name[0] ? d.name : ip);
        sink.appendf(",\"ip\":\"%s\",\"url\":\"http://%s/\",\"type\":\"%s\"",
                     ip, ip, devTypeStr(d.type));
        writeSpeaks(sink, d.speaks);
        if (d.self) sink.append(",\"self\":true");
        sink.append("}");
    }

    void onBuildControls() override {
        MoonModule::onBuildControls();
        // `scan` is a momentary ACTION (rescan now), not an on/off state — a Button,
        // not a Bool toggle (a toggle next to the "scanning…" status reads as two
        // unrelated states). onUpdate runs the rescan.
        controls_.addButton("scan");
        // No "status" control — sweep state goes through MoonModule::setStatus(), the
        // standard status channel the UI renders generically (mod.status/severity).
        // Sweep progress (host 0..254, plain count not KB). Always present: the WS
        // state push patches values but not structure, so a show-only-while-scanning
        // hide flag wouldn't update live. At rest the value is 0 (empty bar); pressing
        // `scan` mid-sweep just restarts the sweep (harmless), so no need to gate it.
        controls_.addProgress("progress", scanProgress_, 254, /*bytes=*/false);
        controls_.addList("devices", *this);   // this module is the ListSource
    }

    void onUpdate(const char* controlName) override {
        if (std::strcmp(controlName, "scan") == 0) restartScan();
    }

    void setup() override {
        MoonModule::setup();
        setStatus(statusBuf_);   // "idle" until the boot sweep starts
        // Don't scan here — the network isn't up yet (DHCP lands a few seconds after
        // boot). loop1s() kicks the ONE boot sweep once a local IP appears.
    }

    // The sweep advances one IP per tick. It runs on the render task and each probe
    // BLOCKS up to kProbeTimeoutMs on a dead host — a hot-path stall that would
    // flicker the LEDs. So the sweep runs ONCE at boot (LEDs not yet critical) and
    // otherwise only on an explicit `scan` press; there is NO periodic background
    // scan. Moving the blocking probe to its own FreeRTOS task is the enabler for
    // safe periodic scanning (and a UDP presence beacon) — see backlog.
    void loop1s() override {
        MoonModule::loop1s();
        if (hostCursor_ >= 0) {
            stepScan();
        } else if (!sweptOnce_) {
            // One-time boot sweep, started as soon as the network is up.
            uint8_t local[4] = {};
            localIp(local);
            if (local[0] || local[1] || local[2] || local[3]) restartScan();
        }
    }

    ModuleRole role() const override { return ModuleRole::Generic; }

private:
    // DevType / classifyDevice / extractDeviceName live in DeviceIdentify.h (pure +
    // unit-tested). devTypeStr() there replaces the former local typeStr().

    // Protocols a device is known to speak, as a bitmask. v1 discovery only proves
    // HTTP (the scan probes it), so that's all that gets set today — but the field
    // exists so additional discovery strategies (mDNS browse, UDP/ArtPoll/DDP/OSC,
    // RTP-MIDI; see DevicesModule.md "Discovery is per-protocol") fill in more bits
    // without reshaping the device record or the wire format. A consumer (Art-Net
    // sync, fleet OTA) reads `speaks` to know how it can talk to a device.
    enum Proto : uint8_t {
        ProtoHttp   = 1 << 0,   // an HTTP API (REST) — the only one v1 discovers
        ProtoArtnet = 1 << 1,   // Art-Net / sACN (future: ArtPoll discovery)
        ProtoDdp    = 1 << 2,   // DDP (future)
        // … mDNS-advertised services, OSC, RTP-MIDI, etc. as strategies are added.
    };

    struct Device {
        uint8_t ip[4] = {};
        char    name[24] = {};
        DevType type = DevType::Generic;
        uint8_t speaks = 0;     // Proto bitmask — protocols this device is known to speak
        bool    self = false;
        uint8_t missed = 0;     // consecutive full sweeps not seen — age-out at kMaxMissed
    };

    // Append a `speaks` JSON array (e.g. ["http"]) for a device's protocol bitmask.
    static void writeSpeaks(JsonSink& sink, uint8_t speaks) {
        sink.append(",\"speaks\":[");
        bool first = true;
        auto emit = [&](uint8_t bit, const char* tag) {
            if (!(speaks & bit)) return;
            if (!first) sink.append(",");
            sink.appendf("\"%s\"", tag);
            first = false;
        };
        emit(ProtoHttp, "http");
        emit(ProtoArtnet, "artnet");
        emit(ProtoDdp, "ddp");
        sink.append("]");
    }

    static constexpr uint8_t  kMaxDevices    = 32;  // a LAN's worth; bounded, no heap
    // One IP per tick: a probe blocks up to kProbeTimeoutMs on a dead host, and
    // loop1s must not stall the render loop. 1 IP/tick → a /24 sweep takes ~254 s
    // worst case (all-dead subnet), but each tick blocks at most one timeout. The
    // probe short-circuits after the FIRST GET times out (a dead host answers no
    // URL), so a sparse subnet costs ~1×timeout per empty IP, not 3×.
    static constexpr uint8_t  kProbesPerTick = 1;
    static constexpr uint32_t kProbeTimeoutMs = 150;
    static constexpr uint8_t  kMaxMissed     = 2;   // drop after this many empty sweeps
    Device  devices_[kMaxDevices];
    uint8_t deviceCount_ = 0;
    bool    sweptOnce_ = false;     // the one boot sweep has completed
    uint32_t scanProgress_ = 0;     // current host index 1..254 (0 = idle), for the Progress bar
    char    statusBuf_[40] = "idle";
    // Probe response buffer — a member, not a per-call stack local: /api/state's
    // deviceName can sit past 512 B on a multi-module device, so this needs ~1 KB,
    // too large for a stack frame in the scheduler task. One probe runs per tick, so
    // a single reused buffer suffices (no concurrency). Part of the module's fixed
    // footprint (~1 KB), allocated once.
    char    probeBuf_[1024];

    // Sweep cursor: hostLow_ walks 1..254 across the local /24. -1 = no scan running
    // (no network yet, or sweep finished). The subnet's first three octets come from
    // the local IP, captured at restartScan().
    int16_t hostCursor_ = -1;
    uint8_t subnet_[3] = {};   // first three octets of the /24 being swept

    // True when a control or first-run kicks off a fresh full sweep. Captures the
    // local IP (and so the subnet); marks every known device unseen-this-sweep.
    void restartScan() {
        uint8_t local[4] = {};
        localIp(local);
        if (local[0] == 0 && local[1] == 0 && local[2] == 0 && local[3] == 0) {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "no network");
            setStatus(statusBuf_, Severity::Warning);
            hostCursor_ = -1;
            return;
        }
        subnet_[0] = local[0]; subnet_[1] = local[1]; subnet_[2] = local[2];
        hostCursor_ = 1;       // sweep .1 .. .254
        scanProgress_ = 1;
        std::snprintf(statusBuf_, sizeof(statusBuf_), "scanning %u.%u.%u.0/24",
                      subnet_[0], subnet_[1], subnet_[2]);
        setStatus(statusBuf_);
        // Ensure self is in the list even before its own IP is probed.
        upsertSelf(local);
    }

    // Probe up to kProbesPerTick hosts this tick; advance the cursor. When the sweep
    // completes, age out devices not seen and go idle until the next restartScan.
    void stepScan() {
        if (hostCursor_ < 0) return;
        uint8_t local[4] = {};
        localIp(local);
        for (uint8_t i = 0; i < kProbesPerTick && hostCursor_ <= 254; i++, hostCursor_++) {
            uint8_t ip[4] = {subnet_[0], subnet_[1], subnet_[2],
                             static_cast<uint8_t>(hostCursor_)};
            // Don't probe our own IP: upsertSelf already gave it the right identity
            // (projectMM, deviceName), and an HTTP request to ourselves mid-tick can
            // race the server / loopback and misclassify us as generic. Just keep it
            // fresh so age-out doesn't drop it.
            if (ipEq(ip, local)) { if (Device* d = findByIp(ip)) d->missed = 0; continue; }
            probe(ip);
        }
        // Progress bar tracks the cursor (clamped to the 254 total for the last tick).
        scanProgress_ = (hostCursor_ > 254) ? 254 : static_cast<uint32_t>(hostCursor_);
        if (hostCursor_ > 254) {
            hostCursor_ = -1;
            sweptOnce_ = true;
            ageOut();
            std::snprintf(statusBuf_, sizeof(statusBuf_), "%u device%s",
                          deviceCount_, deviceCount_ == 1 ? "" : "s");
            setStatus(statusBuf_);
        }
    }

    void localIp(uint8_t out[4]) const {
        platform::ethGetIPv4(out);
        if (!out[0] && !out[1] && !out[2] && !out[3]) platform::wifiStaGetIPv4(out);
    }

    // HTTP-probe one IP and classify. Tries port 80 first (ESP32 devices, WLED,
    // generic web UIs); if nothing answers there, tries port 8080 (a projectMM
    // DESKTOP instance serves its API on 8080, not 80 — see main_desktop.cpp).
    // A live :80 host stops after :80, so the extra :8080 attempt only costs a
    // second timeout on otherwise-empty IPs, keeping the per-IP budget bounded.
    void probe(const uint8_t ip[4]) {
        if (probePort(ip, 80)) return;
        probePort(ip, 8080);
    }

    // Probe one ip:port. Returns true if a host answered (so the caller can stop).
    bool probePort(const uint8_t ip[4], uint16_t port) {
        char url[48], ipStr[16];
        formatDottedQuad(ipStr, ip);

        // First GET doubles as the liveness check: status 0 == no host answered
        // (timeout / connection refused). The response goes in probeBuf_, a member
        // (NOT a stack local): /api/state's deviceName can sit past 512 B on a
        // multi-module device, so the buffer must be ~1 KB — too large for this
        // call's stack frame in the scheduler task, so it lives in the module's
        // fixed footprint and is reused each probe (one probe per tick).
        std::snprintf(url, sizeof(url), "http://%s:%u/api/state", ipStr, port);
        int status = platform::httpGet(url, kProbeTimeoutMs, probeBuf_, sizeof(probeBuf_));
        if (status == 0) return false;   // nothing on this port
        DevType t = classifyDevice(probeBuf_, nullptr);
        if (t == DevType::ProjectMM) { upsert(ip, t, probeBuf_); return true; }

        // Not a projectMM — try the WLED info endpoint on this port.
        std::snprintf(url, sizeof(url), "http://%s:%u/json/info", ipStr, port);
        if (platform::httpGet(url, kProbeTimeoutMs, probeBuf_, sizeof(probeBuf_)) == 200) {
            t = classifyDevice(nullptr, probeBuf_);
            if (t == DevType::Wled) { upsert(ip, t, probeBuf_); return true; }
        }
        // Live host, not projectMM/WLED → generic HTTP device.
        upsert(ip, DevType::Generic, nullptr);
        return true;
    }

    // Find-or-insert a device by IP; refresh its type/name and mark it seen.
    void upsert(const uint8_t ip[4], DevType type, const char* body) {
        uint8_t local[4] = {};
        localIp(local);
        const bool isSelf = ipEq(ip, local);
        Device* d = findByIp(ip);
        if (!d) {
            if (deviceCount_ >= kMaxDevices) return;   // bounded; silently cap
            d = &devices_[deviceCount_++];
            std::memcpy(d->ip, ip, 4);
        }
        d->type = type;
        d->self = isSelf;
        d->missed = 0;
        d->speaks |= ProtoHttp;   // found via the HTTP scan → it speaks HTTP
        extractDeviceName(type, body, d->name, sizeof(d->name));
        if (!d->name[0]) formatDottedQuad(d->name, ip);   // fall back to the IP
    }

    // Guarantee this device is listed (marked self) even before its IP is swept.
    void upsertSelf(const uint8_t ip[4]) {
        Device* d = findByIp(ip);
        if (!d) {
            if (deviceCount_ >= kMaxDevices) return;
            d = &devices_[deviceCount_++];
            std::memcpy(d->ip, ip, 4);
            d->type = DevType::ProjectMM;   // we are a projectMM
        }
        d->self = true;
        d->missed = 0;
        if (!d->name[0]) std::snprintf(d->name, sizeof(d->name), "this device");
    }


    Device* findByIp(const uint8_t ip[4]) {
        for (uint8_t i = 0; i < deviceCount_; i++)
            if (ipEq(devices_[i].ip, ip)) return &devices_[i];
        return nullptr;
    }

    static bool ipEq(const uint8_t a[4], const uint8_t b[4]) {
        return std::memcmp(a, b, 4) == 0;
    }

    // After a full sweep, bump missed-count on unseen devices and compact out the
    // ones past kMaxMissed (a device that left the network). self never ages out.
    void ageOut() {
        uint8_t w = 0;
        for (uint8_t r = 0; r < deviceCount_; r++) {
            Device& d = devices_[r];
            if (!d.self) {
                if (d.missed >= kMaxMissed) continue;   // drop
            }
            if (w != r) devices_[w] = d;
            w++;
        }
        deviceCount_ = w;
        // Arm missed-count for the NEXT sweep: anything not refreshed by upsert()
        // (which resets missed to 0) gets counted as missed once this sweep ends.
        for (uint8_t i = 0; i < deviceCount_; i++)
            if (!devices_[i].self) devices_[i].missed++;
    }
};

}  // namespace mm
