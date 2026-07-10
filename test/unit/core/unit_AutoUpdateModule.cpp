// @module AutoUpdateModule

#include "doctest.h"
#include "core/AutoUpdateModule.h"

#include <cstdio>
#include <cstring>

using mm::AutoUpdateModule;

static constexpr const char* kHash =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

// A compact manifest selects its newer OTA object and resolves the relative image path.
TEST_CASE("AutoUpdateModule selects a newer ota object and resolves relative URL") {
    char manifest[900];
    std::snprintf(manifest, sizeof(manifest),
        "{\"version\":\"9.0.0\",\"ota\":{\"path\":\"firmware-esp32s3.bin\","
        "\"offset\":65536,\"chipFamily\":\"ESP32-S3\",\"size\":1234,\"sha256\":\"%s\"}}", kHash);

    auto d = AutoUpdateModule::selectCandidate(
        manifest, "https://updates.example.test/devices/manifest.json",
        "1.0.0", "ESP32-S3");
    CHECK(d.shouldInstall());
    CHECK(std::strcmp(d.candidate.url, "https://updates.example.test/devices/firmware-esp32s3.bin") == 0);
    CHECK(std::strcmp(d.candidate.version, "9.0.0") == 0);
}

// A manifest with the running version is recognised as already installed.
TEST_CASE("AutoUpdateModule refuses a same-version manifest") {
    char manifest[900];
    std::snprintf(manifest, sizeof(manifest),
        "{\"version\":\"1.0.0\",\"ota\":{\"path\":\"firmware.bin\","
        "\"offset\":65536,\"chipFamily\":\"ESP32-S3\",\"sha256\":\"%s\"}}", kHash);
    auto d = AutoUpdateModule::selectCandidate(manifest, "https://x.test/manifest.json",
                                               "1.0.0", "ESP32-S3");
    CHECK(d.code == AutoUpdateModule::DecisionCode::UpToDate);
    CHECK_FALSE(d.shouldInstall());
}

// A full flash bundle at offset 0 is rejected because OTA can only write the app partition.
TEST_CASE("AutoUpdateModule rejects full-bundle offset zero") {
    char manifest[900];
    std::snprintf(manifest, sizeof(manifest),
        "{\"version\":\"9.0.0\",\"ota\":{\"path\":\"merged.bin\","
        "\"offset\":0,\"chipFamily\":\"ESP32-S3\",\"sha256\":\"%s\"}}", kHash);
    auto d = AutoUpdateModule::selectCandidate(manifest, "https://x.test/manifest.json",
                                               "1.0.0", "ESP32-S3");
    CHECK(d.code == AutoUpdateModule::DecisionCode::BadOffset);
}

// ESP Web Tools manifests pick the app part for the matching chip family.
TEST_CASE("AutoUpdateModule selects app part from ESP Web Tools builds") {
    char manifest[1200];
    std::snprintf(manifest, sizeof(manifest),
        "{\"version\":\"2.0.0\",\"builds\":[{\"chipFamily\":\"ESP32\","
        "\"parts\":[{\"path\":\"wrong.bin\",\"offset\":65536,\"sha256\":\"%s\"}]},"
        "{\"chipFamily\":\"ESP32-S3\",\"parts\":["
        "{\"path\":\"boot.bin\",\"offset\":0},"
        "{\"path\":\"app.bin\",\"offset\":65536,\"size\":456,\"sha256\":\"%s\"}]}]}",
        kHash, kHash);
    auto d = AutoUpdateModule::selectCandidate(manifest, "https://host/fw/manifest.json",
                                               "1.0.0", "ESP32-S3");
    CHECK(d.shouldInstall());
    CHECK(std::strcmp(d.candidate.url, "https://host/fw/app.bin") == 0);
    CHECK(d.candidate.size == 456);
}

// Development and stable semantic versions compare in release order.
TEST_CASE("AutoUpdateModule compares dev versions monotonically") {
    CHECK(AutoUpdateModule::versionIsNewer("2.1.0-dev.7", "2.1.0-dev.6"));
    CHECK_FALSE(AutoUpdateModule::versionIsNewer("2.1.0-dev.6", "2.1.0-dev.7"));
    CHECK(AutoUpdateModule::versionIsNewer("2.1.0", "2.1.0-dev.9"));
}
