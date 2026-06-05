// @module GyroDriver

#include "doctest.h"
#include "light/drivers/GyroDriver.h"
#include "platform/platform.h"

#include <cstring>

// GyroDriver polls an MPU6050 over I2C in loop20ms() and formats read-only
// controls in loop1s(). Desktop platform simulates the sensor so these tests
// run without hardware.

TEST_CASE("GyroDriver setup finds simulated MPU6050") {
    mm::GyroDriver driver;
    driver.onBuildControls();
    driver.setup();

    REQUIRE(driver.status() != nullptr);
    CHECK(std::strcmp(driver.status(), "MPU6050 ready") == 0);
    CHECK(driver.severity() == mm::MoonModule::Severity::Status);
}

TEST_CASE("GyroDriver loop20ms and loop1s populate rotation controls") {
    mm::platform::setTestNowMs(5000);

    mm::GyroDriver driver;
    driver.onBuildControls();
    driver.setup();
    REQUIRE(std::strcmp(driver.status(), "MPU6050 ready") == 0);

    driver.loop20ms();
    driver.loop1s();

    CHECK(driver.controls().count() == 5);
    const char* gx = static_cast<char*>(driver.controls()[0].ptr);
    const char* gy = static_cast<char*>(driver.controls()[1].ptr);
    const char* gz = static_cast<char*>(driver.controls()[2].ptr);
    const bool anyGyroNonZero = (std::strcmp(gx, "0 °/s") != 0) ||
                                (std::strcmp(gy, "0 °/s") != 0) ||
                                (std::strcmp(gz, "0 °/s") != 0);
    CHECK(anyGyroNonZero);
    CHECK(std::strstr(static_cast<char*>(driver.controls()[3].ptr), "°") != nullptr);
    CHECK(std::strstr(static_cast<char*>(driver.controls()[4].ptr), "°") != nullptr);

    mm::platform::setTestNowMs(0);
}

TEST_CASE("GyroDriver gyro values track simulated time ramp") {
    mm::platform::setTestNowMs(1100);

    mm::GyroDriver driver;
    driver.onBuildControls();
    driver.setup();
    driver.loop20ms();
    driver.loop1s();
    char gyroXAt1s[16];
    std::strncpy(gyroXAt1s, static_cast<char*>(driver.controls()[0].ptr), sizeof(gyroXAt1s));

    mm::platform::setTestNowMs(1200);
    driver.loop20ms();
    driver.loop1s();
    char gyroXAt2s[16];
    std::strncpy(gyroXAt2s, static_cast<char*>(driver.controls()[0].ptr), sizeof(gyroXAt2s));

    CHECK(std::strcmp(gyroXAt1s, gyroXAt2s) != 0);

    mm::platform::setTestNowMs(0);
}
