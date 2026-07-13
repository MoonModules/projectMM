// @module BleProvisioningModule

#include "doctest.h"
#include "core/BleProvisioningModule.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

struct FakeBleState {
    uint32_t now = 0;
    bool startResult = true;
    unsigned startCalls = 0;
    unsigned stopCalls = 0;
    char* ssidOut = nullptr;
    size_t ssidOutLen = 0;
    char* passwordOut = nullptr;
    size_t passwordOutLen = 0;
    std::atomic<bool>* ready = nullptr;
};

FakeBleState fake;

uint32_t fakeMillis() { return fake.now; }
const char* fakeChipModel() { return "desktop-test"; }

bool fakeStart(const char* /*deviceName*/, const char* /*chipModel*/, const char* /*version*/,
               char* ssidOut, size_t ssidOutLen,
               char* passwordOut, size_t passwordOutLen,
               std::atomic<bool>* ready,
               char* statusBuf, size_t statusBufLen) {
    fake.startCalls++;
    fake.ssidOut = ssidOut;
    fake.ssidOutLen = ssidOutLen;
    fake.passwordOut = passwordOut;
    fake.passwordOutLen = passwordOutLen;
    fake.ready = ready;
    std::snprintf(statusBuf, statusBufLen, "BLE test listener");
    return fake.startResult;
}

void fakeStop() { fake.stopCalls++; }

const mm::BleProvisioningRuntime runtime{
    fakeMillis,
    fakeChipModel,
    fakeStart,
    fakeStop,
};

struct FakeGuard {
    FakeGuard() { fake = {}; }
    ~FakeGuard() { fake = {}; }
};

} // namespace

TEST_CASE("BLE provisioning advertises only during Network AP fallback") {
    FakeGuard guard;
    mm::NetworkModule network;
    mm::BleProvisioningModule ble;
    ble.setNetworkModule(&network);
    ble.setRuntime(&runtime);

    ble.setup();
    CHECK(fake.startCalls == 0);

    network.setProvisioningModeForTest(true);
    fake.now = 5000;
    ble.tick1s();
    CHECK(fake.startCalls == 1);

    network.setProvisioningModeForTest(false);
    ble.tick1s();
    CHECK(fake.stopCalls == 1);
}

TEST_CASE("BLE provisioning retries failed listener startup every five seconds") {
    FakeGuard guard;
    fake.startResult = false;
    mm::NetworkModule network;
    network.setProvisioningModeForTest(true);
    mm::BleProvisioningModule ble;
    ble.setNetworkModule(&network);
    ble.setRuntime(&runtime);

    ble.setup();
    CHECK(fake.startCalls == 1);

    fake.now = 4999;
    ble.tick1s();
    CHECK(fake.startCalls == 1);

    fake.now = 5000;
    ble.tick1s();
    CHECK(fake.startCalls == 2);
}

TEST_CASE("BLE provisioning publishes credentials to Network and clears the password") {
    FakeGuard guard;
    mm::NetworkModule network;
    network.setProvisioningModeForTest(true);
    mm::BleProvisioningModule ble;
    ble.setNetworkModule(&network);
    ble.setRuntime(&runtime);
    ble.setup();

    REQUIRE(fake.ssidOut != nullptr);
    REQUIRE(fake.passwordOut != nullptr);
    REQUIRE(fake.ready != nullptr);
    std::snprintf(fake.ssidOut, fake.ssidOutLen, "homeAP");
    std::snprintf(fake.passwordOut, fake.passwordOutLen, "secret123");
    fake.ready->store(true, std::memory_order_release);

    CHECK_FALSE(network.dirty());
    ble.tick1s();
    CHECK(network.dirty());
    CHECK_FALSE(fake.ready->load(std::memory_order_acquire));
    CHECK(fake.passwordOut[0] == 0);

    ble.release();
    CHECK(fake.stopCalls == 1);
}
