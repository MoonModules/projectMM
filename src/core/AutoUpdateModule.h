#pragma once

#include "core/MoonModule.h"
#include "core/JsonUtil.h"
#include "core/build_info.h"
#include "core/OtaUpdateState.h"

#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mm {

struct AutoUpdateRuntime {
    uint32_t (*millis)() = nullptr;
    bool (*networkReady)() = nullptr;
    int (*httpGet)(const char* url, char* body, size_t bodyLen, uint32_t timeoutMs) = nullptr;
    const char* (*chipModel)() = nullptr;
    size_t (*firmwarePartition)() = nullptr;
    bool (*httpFetchToOtaChecked)(const char* url, const char* expectedSha256,
                                  uint32_t expectedSize,
                                  char* statusBuf, size_t statusBufLen,
                                  uint32_t* bytesReadOut, uint32_t* bytesTotalOut,
                                  std::atomic<bool>* inFlightFlag) = nullptr;
};

/// Automatic manifest-driven firmware update. Designed for firmware channels
/// where an update URL may be baked into the image so it survives a settings
/// wipe. The module fetches a small JSON manifest, validates that it
/// names a newer app image for this chip at the OTA app offset, then starts the
/// existing ESP-IDF OTA task with SHA-256 verification.
/// @card AutoUpdateModule.png
class AutoUpdateModule : public MoonModule {
public:
    static constexpr uint32_t kAppOffset = 0x10000;

    enum class DecisionCode : uint8_t {
        Install,
        UpToDate,
        InvalidManifest,
        MissingCandidate,
        ChipMismatch,
        BadOffset,
        MissingVersion,
        MissingHash,
        MissingSize,
        TooLarge,
    };

    struct Candidate {
        char url[512] = {};
        char version[32] = {};
        char sha256[65] = {};
        char chipFamily[24] = {};
        uint32_t size = 0;
        uint32_t offset = kAppOffset;
    };

    struct Decision {
        DecisionCode code = DecisionCode::InvalidManifest;
        const char* reason = "invalid manifest";
        Candidate candidate;
        bool shouldInstall() const { return code == DecisionCode::Install; }
    };

    AutoUpdateModule() {
        copyString(manifestUrl_, sizeof(manifestUrl_), kAutoUpdateManifestUrl);
        autoInstall_ = manifestUrl_[0] != 0;
    }

    void setRuntime(const AutoUpdateRuntime* runtime) { runtime_ = runtime; }

    void setup() override {
        MoonModule::setup();
        scheduleAfter(static_cast<uint32_t>(bootDelaySec_) * 1000u);
        if (manifestUrl_[0] && autoInstall_) setStatusf(Severity::Status, "waiting for network");
        else clearStatus();
    }

    void defineControls() override {
        controls_.addText("manifestUrl", manifestUrl_, sizeof(manifestUrl_), validateUrl);
        controls_.addBool("autoInstall", autoInstall_);
        controls_.addUint16("checkIntervalMin", checkIntervalMin_, 15, 1440);
        controls_.addUint16("retryMin", retryMin_, 1, 240);
        controls_.addUint16("bootDelaySec", bootDelaySec_, 10, 3600);
        controls_.addReadOnly("latest", latestVersion_, sizeof(latestVersion_));
        controls_.addReadOnly("lastCheck", lastCheckStr_, sizeof(lastCheckStr_));
        MoonModule::defineControls();
    }

    void onControlChanged(const char* name) override {
        if (!name) return;
        if (std::strcmp(name, "manifestUrl") == 0 ||
            std::strcmp(name, "autoInstall") == 0 ||
            std::strcmp(name, "checkIntervalMin") == 0 ||
            std::strcmp(name, "retryMin") == 0 ||
            std::strcmp(name, "bootDelaySec") == 0) {
            scheduleAfter(1000);
        }
    }

    void tick1s() override {
        MoonModule::tick1s();
        if (!manifestUrl_[0]) {
            clearStatus();
            return;
        }
        if (!autoInstall_) { clearStatus(); return; }
        if (otaInFlight()) return;
        if (!runtimeReady()) {
            setStatusf(Severity::Status, "waiting for network");
            return;
        }
        const uint32_t now = runtime_->millis();
        if (!timeReached(now, nextCheckMs_)) return;
        checkNow();
    }

    static bool versionIsNewer(const char* candidate, const char* current) {
        SemVer a = parseVersion(candidate);
        SemVer b = parseVersion(current);
        if (!a.valid || !b.valid) return false;
        if (a.major != b.major) return a.major > b.major;
        if (a.minor != b.minor) return a.minor > b.minor;
        if (a.patch != b.patch) return a.patch > b.patch;
        if (a.prerelease != b.prerelease) return !a.prerelease;
        if (!a.prerelease) return false;
        if (a.dev != b.dev) return a.dev > b.dev;
        return std::strcmp(a.pre, b.pre) > 0;
    }

    static Decision selectCandidate(const char* manifestJson,
                                    const char* manifestUrl,
                                    const char* currentVersion,
                                    const char* chipFamily,
                                    size_t firmwarePartition = 0) {
        static json::JsonDoc doc;
        if (!json::parse(manifestJson, doc) ||
            !doc.rootNode() || doc.rootNode()->type != json::JsonType::Object) {
            return fail(DecisionCode::InvalidManifest, "invalid manifest");
        }

        Candidate c;
        const json::JsonNode* root = doc.rootNode();
        bool sawMatchingBuild = false;
        bool found = readOtaObject(doc, root, c, chipFamily);
        if (!found) {
            found = readBuilds(doc, root, c, chipFamily, sawMatchingBuild);
            if (!found && sawMatchingBuild) {
                return fail(DecisionCode::MissingCandidate, "no OTA app image");
            }
            if (!found && json::member(doc, root, "builds")) {
                return fail(DecisionCode::ChipMismatch, "chip not in manifest");
            }
        }
        if (!found || !c.url[0]) return fail(DecisionCode::MissingCandidate, "no OTA image");
        if (c.offset != kAppOffset) return fail(DecisionCode::BadOffset, "bad OTA offset");
        if (!c.version[0]) return fail(DecisionCode::MissingVersion, "manifest version missing");
        if (!isHex(c.sha256, 64)) return fail(DecisionCode::MissingHash, "sha256 missing");
        if (!versionIsNewer(c.version, currentVersion)) {
            Decision d = fail(DecisionCode::UpToDate, "up to date");
            d.candidate = c;
            return d;
        }
        if (c.size == 0) return fail(DecisionCode::MissingSize, "size missing");
        if (firmwarePartition > 0 && c.size > firmwarePartition) {
            return fail(DecisionCode::TooLarge, "image too large");
        }
        char resolved[sizeof(c.url)] = {};
        resolveUrl(manifestUrl, c.url, resolved, sizeof(resolved));
        copyString(c.url, sizeof(c.url), resolved);
        Decision d;
        d.code = DecisionCode::Install;
        d.reason = "install";
        d.candidate = c;
        return d;
    }

private:
    struct SemVer {
        int major = 0, minor = 0, patch = 0, dev = -1;
        bool prerelease = false;
        bool valid = false;
        char pre[24] = {};
    };

    char manifestUrl_[512] = {};
    bool autoInstall_ = true;
    uint16_t checkIntervalMin_ = 360;
    uint16_t retryMin_ = 30;
    uint16_t bootDelaySec_ = 60;
    uint32_t nextCheckMs_ = 0;
    char latestVersion_[32] = "";
    char lastCheckStr_[24] = "never";
    char statusBuf_[96] = {};
    char manifestBuf_[json::kMaxJsonLen] = {};
    const AutoUpdateRuntime* runtime_ = nullptr;

    static bool validateUrl(const char* s) {
        return !s || !*s || std::strncmp(s, "https://", 8) == 0;
    }

    static void copyString(char* dst, size_t dstLen, const char* src) {
        if (!dst || dstLen == 0) return;
        std::snprintf(dst, dstLen, "%s", src ? src : "");
    }

    void setStatusf(Severity sev, const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(statusBuf_, sizeof(statusBuf_), fmt, args);
        va_end(args);
        setStatus(statusBuf_, sev);
    }

    static bool timeReached(uint32_t now, uint32_t deadline) {
        return static_cast<int32_t>(now - deadline) >= 0;
    }

    void scheduleAfter(uint32_t delayMs) {
        nextCheckMs_ = nowMs() + delayMs;
    }

    void scheduleRetry() {
        scheduleAfter(static_cast<uint32_t>(retryMin_) * 60000u);
    }

    void scheduleNormal() {
        scheduleAfter(static_cast<uint32_t>(checkIntervalMin_) * 60000u);
    }

    void checkNow() {
        if (!validateUrl(manifestUrl_)) {
            setStatusf(Severity::Warning, "manifest URL must be HTTPS");
            scheduleRetry();
            rebuildControls();
            return;
        }
        if (!runtime_) return;

        const uint32_t now = runtime_->millis();
        std::snprintf(lastCheckStr_, sizeof(lastCheckStr_), "%lus",
                      static_cast<unsigned long>(now / 1000u));
        setStatusf(Severity::Status, "checking");
        const int code = runtime_->httpGet(manifestUrl_, manifestBuf_, sizeof(manifestBuf_), 12000);
        if (code != 200) {
            setStatusf(Severity::Warning, "manifest HTTP %d", code);
            scheduleRetry();
            rebuildControls();
            return;
        }
        Decision d = selectCandidate(manifestBuf_, manifestUrl_, kVersion, runtime_->chipModel(),
                                     runtime_->firmwarePartition());
        if (d.candidate.version[0]) copyString(latestVersion_, sizeof(latestVersion_), d.candidate.version);
        if (!d.shouldInstall()) {
            setStatusf(d.code == DecisionCode::UpToDate ? Severity::Status : Severity::Warning,
                       "%s", d.reason);
            if (d.code == DecisionCode::UpToDate) scheduleNormal();
            else scheduleRetry();
            rebuildControls();
            return;
        }

        if (!otaTryStart()) return;
        copyString(g_otaStatus, sizeof(g_otaStatus), "starting");
        g_otaBytesRead = 0;
        g_otaBytesTotal = d.candidate.size;
        setStatusf(Severity::Status, "installing %s", d.candidate.version);
        const bool ok = runtime_->httpFetchToOtaChecked(
            d.candidate.url, d.candidate.sha256, d.candidate.size,
            g_otaStatus, sizeof(g_otaStatus), &g_otaBytesRead, &g_otaBytesTotal,
            &g_otaInFlight);
        if (!ok) {
            otaFinish();
            setStatusf(Severity::Error, "OTA start failed");
            scheduleRetry();
        } else {
            scheduleNormal();
        }
        rebuildControls();
    }

    bool runtimeReady() const {
        return runtime_ && runtime_->millis && runtime_->networkReady && runtime_->httpGet &&
               runtime_->chipModel && runtime_->firmwarePartition &&
               runtime_->httpFetchToOtaChecked && runtime_->networkReady();
    }

    uint32_t nowMs() const {
        return (runtime_ && runtime_->millis) ? runtime_->millis() : 0;
    }

    static Decision fail(DecisionCode code, const char* reason) {
        Decision d;
        d.code = code;
        d.reason = reason;
        return d;
    }

    static bool isHex(const char* s, size_t len) {
        if (!s) return false;
        for (size_t i = 0; i < len; i++) {
            if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
        }
        return s[len] == 0;
    }

    static SemVer parseVersion(const char* s) {
        SemVer v;
        if (!s || !*s) return v;
        if (*s == 'v' || *s == 'V') s++;
        char* end = nullptr;
        v.major = static_cast<int>(std::strtol(s, &end, 10));
        if (end == s || *end != '.') return v;
        s = end + 1;
        v.minor = static_cast<int>(std::strtol(s, &end, 10));
        if (end == s || *end != '.') return v;
        s = end + 1;
        v.patch = static_cast<int>(std::strtol(s, &end, 10));
        if (end == s) return v;
        v.valid = (*end == 0 || *end == '-');
        if (*end == '-') {
            v.prerelease = true;
            copyString(v.pre, sizeof(v.pre), end + 1);
            if (std::strncmp(v.pre, "dev", 3) == 0) {
                const char* p = v.pre + 3;
                if (*p == '.') p++;
                char* devEnd = nullptr;
                long d = std::strtol(p, &devEnd, 10);
                if (devEnd != p) v.dev = static_cast<int>(d);
            }
        }
        return v;
    }

    static bool readStringMember(const json::JsonDoc& doc, const json::JsonNode* obj,
                                 const char* key, char* out, size_t outLen) {
        return json::readString(json::member(doc, obj, key), out, outLen);
    }

    static bool readOffset(const json::JsonNode* n, uint32_t& out) {
        if (!n) return false;
        if (n->type == json::JsonType::Int) {
            long v = json::readInt(n, -1);
            if (v < 0) return false;
            out = static_cast<uint32_t>(v);
            return true;
        }
        char buf[24];
        if (!json::readString(n, buf, sizeof(buf))) return false;
        char* end = nullptr;
        unsigned long v = std::strtoul(buf, &end, 0);
        if (end == buf || *end != 0) return false;
        out = static_cast<uint32_t>(v);
        return true;
    }

    static void readCommon(const json::JsonDoc& doc, const json::JsonNode* root,
                           const json::JsonNode* obj, Candidate& c) {
        readStringMember(doc, obj, "url", c.url, sizeof(c.url));
        if (!c.url[0]) readStringMember(doc, obj, "path", c.url, sizeof(c.url));
        readStringMember(doc, obj, "version", c.version, sizeof(c.version));
        if (!c.version[0]) readStringMember(doc, root, "version", c.version, sizeof(c.version));
        readStringMember(doc, obj, "sha256", c.sha256, sizeof(c.sha256));
        readStringMember(doc, obj, "chipFamily", c.chipFamily, sizeof(c.chipFamily));
        c.size = static_cast<uint32_t>(json::readInt(json::member(doc, obj, "size"), c.size));
        readOffset(json::member(doc, obj, "offset"), c.offset);
    }

    static void normaliseChip(const char* in, char* out, size_t outLen) {
        if (!out || outLen == 0) return;
        size_t j = 0;
        for (size_t i = 0; in && in[i] && j + 1 < outLen; i++) {
            unsigned char ch = static_cast<unsigned char>(in[i]);
            if (std::isalnum(ch)) out[j++] = static_cast<char>(std::tolower(ch));
        }
        out[j] = 0;
    }

    static bool chipMatches(const char* manifestChip, const char* deviceChip) {
        if (!manifestChip || !manifestChip[0]) return true;
        char a[24], b[24];
        normaliseChip(manifestChip, a, sizeof(a));
        normaliseChip(deviceChip, b, sizeof(b));
        return a[0] && std::strcmp(a, b) == 0;
    }

    static bool readOtaObject(const json::JsonDoc& doc, const json::JsonNode* root,
                              Candidate& c, const char* chipFamily) {
        const json::JsonNode* ota = json::member(doc, root, "ota");
        if (!ota || ota->type != json::JsonType::Object) return false;
        readCommon(doc, root, ota, c);
        if (!chipMatches(c.chipFamily, chipFamily)) return false;
        return c.url[0] != 0;
    }

    static bool readBuilds(const json::JsonDoc& doc, const json::JsonNode* root,
                           Candidate& c, const char* chipFamily, bool& sawMatchingBuild) {
        const json::JsonNode* builds = json::member(doc, root, "builds");
        const int n = json::arraySize(doc, builds);
        for (int i = 0; i < n; i++) {
            const json::JsonNode* build = json::element(doc, builds, i);
            if (!build || build->type != json::JsonType::Object) continue;
            char buildChip[24] = {};
            readStringMember(doc, build, "chipFamily", buildChip, sizeof(buildChip));
            if (!chipMatches(buildChip, chipFamily)) continue;
            sawMatchingBuild = true;
            const json::JsonNode* parts = json::member(doc, build, "parts");
            const int partCount = json::arraySize(doc, parts);
            for (int j = 0; j < partCount; j++) {
                const json::JsonNode* part = json::element(doc, parts, j);
                if (!part || part->type != json::JsonType::Object) continue;
                uint32_t offset = 0;
                if (!readOffset(json::member(doc, part, "offset"), offset) || offset != kAppOffset) continue;
                readCommon(doc, root, part, c);
                c.offset = offset;
                copyString(c.chipFamily, sizeof(c.chipFamily), buildChip);
                return c.url[0] != 0;
            }
        }
        return false;
    }

    static void resolveUrl(const char* base, const char* path, char* out, size_t outLen) {
        if (!path || !path[0]) { copyString(out, outLen, ""); return; }
        if (std::strncmp(path, "https://", 8) == 0 || std::strncmp(path, "http://", 7) == 0) {
            copyString(out, outLen, path);
            return;
        }
        const char* scheme = base ? std::strstr(base, "://") : nullptr;
        if (!scheme) { copyString(out, outLen, path); return; }
        const char* host = scheme + 3;
        const char* firstSlash = std::strchr(host, '/');
        if (path[0] == '/') {
            const size_t prefix = firstSlash ? static_cast<size_t>(firstSlash - base) : std::strlen(base);
            std::snprintf(out, outLen, "%.*s%s", static_cast<int>(prefix), base, path);
            return;
        }
        const char* lastSlash = std::strrchr(base, '/');
        if (!lastSlash || lastSlash < host) {
            std::snprintf(out, outLen, "%s/%s", base, path);
        } else {
            const size_t prefix = static_cast<size_t>(lastSlash + 1 - base);
            std::snprintf(out, outLen, "%.*s%s", static_cast<int>(prefix), base, path);
        }
    }
};

} // namespace mm
