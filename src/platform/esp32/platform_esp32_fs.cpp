// LittleFS partition mount + the `fs*` / `filesystem*` API on ESP32.
//
// Cut out of platform_esp32.cpp (plan-23) for size + readability. The
// file owns its private state (FS_TAG, FS_PARTITION_LABEL, FS_MOUNT_POINT,
// fsMounted_) and the path-translation helper; the rest of the platform
// layer talks to it only through the public mm::platform::fs* and
// filesystem* symbols declared in platform.h. Move was a code-organisation
// change with no API delta — anything that compiled before still does.

#include "platform/platform.h"

#include "esp_littlefs.h"
#include "esp_log.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mm::platform {

// LittleFS state
static constexpr const char* FS_TAG = "mm_fs";
static constexpr const char* FS_PARTITION_LABEL = "spiffs";  // partition label kept for tooling compat; contents are LittleFS
static constexpr const char* FS_MOUNT_POINT = "/littlefs";    // VFS mount point; not exposed in API paths
static bool fsMounted_ = false;

// Translate API path "/foo/bar" or "foo/bar" → "/littlefs/foo/bar" into out.
// Returns false on null input, zero-sized output, or truncation; out[0] is set to 0
// on any failure so callers don't accidentally consume a partial path.
static bool fsTranslate(const char* apiPath, char* out, size_t outLen) {
    if (outLen == 0) return false;
    if (!apiPath) { out[0] = 0; return false; }
    const char* sep = (apiPath[0] == '/') ? "" : "/";
    int n = std::snprintf(out, outLen, "%s%s%s", FS_MOUNT_POINT, sep, apiPath);
    if (n < 0 || static_cast<size_t>(n) >= outLen) { out[0] = 0; return false; }
    return true;
}

void fsSetRoot(const char* /*path*/) {
    // No-op on ESP32 — LittleFS is mounted at a fixed partition; the FS_MOUNT_POINT
    // prefix is hard-coded. Provided only so test code can call it portably.
}

bool fsMount() {
    if (fsMounted_) return true;

    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = FS_MOUNT_POINT;
    conf.partition_label = FS_PARTITION_LABEL;
    conf.format_if_mount_failed = true;
    conf.dont_mount = false;

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(FS_TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return false;
    }
    fsMounted_ = true;
    ESP_LOGI(FS_TAG, "LittleFS mounted at %s (partition: %s)", FS_MOUNT_POINT, FS_PARTITION_LABEL);
    return true;
}

void fsUnmount() {
    if (!fsMounted_) return;
    esp_vfs_littlefs_unregister(FS_PARTITION_LABEL);
    fsMounted_ = false;
}

bool fsMkdir(const char* path) {
    if (!fsMounted_) return false;
    char full[128];
    if (!fsTranslate(path, full, sizeof(full))) return false;
    // mkdir -p: walk components, create each if missing
    char* p = full + std::strlen(FS_MOUNT_POINT) + 1; // skip "/littlefs/"
    while (*p) {
        if (*p == '/') {
            *p = 0;
            mkdir(full, 0775);  // ignore errors; could already exist
            *p = '/';
        }
        p++;
    }
    int rc = mkdir(full, 0775);
    return rc == 0 || errno == EEXIST;
}

bool fsExists(const char* path) {
    if (!fsMounted_) return false;
    char full[128];
    if (!fsTranslate(path, full, sizeof(full))) return false;
    struct stat st;
    return stat(full, &st) == 0;
}

bool fsRemove(const char* path) {
    if (!fsMounted_) return false;
    char full[128];
    if (!fsTranslate(path, full, sizeof(full))) return false;
    return ::remove(full) == 0;
}

int fsRead(const char* path, char* buf, size_t maxLen) {
    if (!fsMounted_ || !buf || maxLen == 0) return -1;
    char full[128];
    if (!fsTranslate(path, full, sizeof(full))) return -1;
    FILE* f = std::fopen(full, "rb");
    if (!f) return -1;
    size_t n = std::fread(buf, 1, maxLen - 1, f);
    std::fclose(f);
    buf[n] = 0;
    return static_cast<int>(n);
}

bool fsWriteAtomic(const char* path, const char* data, size_t len) {
    if (!fsMounted_) return false;
    // Guard the public API: a non-zero len with a null data pointer is UB
    // in std::fwrite. No current caller passes null, but the boundary is
    // exposed so the check is cheap insurance.
    if (len > 0 && !data) return false;
    char full[128];
    char tmp[136];
    if (!fsTranslate(path, full, sizeof(full))) return false;
    int n = std::snprintf(tmp, sizeof(tmp), "%s.tmp", full);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(tmp)) return false;

    FILE* f = std::fopen(tmp, "wb");
    if (!f) return false;
    size_t written = std::fwrite(data, 1, len, f);
    if (written != len) {
        std::fclose(f);
        ::remove(tmp);
        return false;
    }
    std::fflush(f);
    int fd = ::fileno(f);
    if (fd >= 0) ::fsync(fd);
    std::fclose(f);

    if (::rename(tmp, full) != 0) {
        ::remove(tmp);
        return false;
    }
    return true;
}

void fsList(const char* dir, FsListCb cb, void* user) {
    if (!fsMounted_ || !cb) return;
    char full[128];
    if (!fsTranslate(dir, full, sizeof(full))) return;
    DIR* d = ::opendir(full);
    if (!d) return;
    struct dirent* ent;
    // Sized to hold full ("/littlefs/..." up to 128) + '/' + max 255-byte d_name + null.
    char childPath[400];
    struct stat st;
    while ((ent = ::readdir(d)) != nullptr) {
        std::snprintf(childPath, sizeof(childPath), "%s/%s", full, ent->d_name);
        bool isDir = stat(childPath, &st) == 0 && S_ISDIR(st.st_mode);
        cb(ent->d_name, isDir, user);
    }
    ::closedir(d);
}

size_t filesystemUsed() {
    if (!fsMounted_) return 0;
    size_t total = 0, used = 0;
    if (esp_littlefs_info(FS_PARTITION_LABEL, &total, &used) != ESP_OK) return 0;
    return used;
}

size_t filesystemTotal() {
    if (!fsMounted_) return 0;
    size_t total = 0, used = 0;
    if (esp_littlefs_info(FS_PARTITION_LABEL, &total, &used) != ESP_OK) return 0;
    return total;
}

} // namespace mm::platform
