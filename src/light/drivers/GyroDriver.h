#pragma once

#include "light/drivers/Drivers.h"
#include "platform/platform.h"

#include <cmath>
#include <cstdio>

namespace mm {

class GyroDriver : public DriverBase {
public:
    // Input-only driver under the Drivers container — no light buffer to consume.
    void setSourceBuffer(Buffer*) override {}

    void setup() override {
        if (!platform::i2cInit(kSdaPin, kSclPin)) {
            setStatus("I2C init failed", Severity::Error);
            return;
        }

        uint8_t who = 0;
        if (!platform::i2cReadRegs(kDevAddr, kRegWhoAmI, &who, 1) || who != kWhoAmIExpected) {
            setStatus("MPU6050 not found", Severity::Error);
            return;
        }

        if (!platform::i2cWriteReg(kDevAddr, kRegPwrMgmt1, 0)) {
            setStatus("MPU6050 wake failed", Severity::Error);
            return;
        }

        ready_ = true;
        setStatus("MPU6050 ready");
    }

    void onBuildControls() override {
        controls_.addReadOnly("gyroX", gyroXStr_, sizeof(gyroXStr_));
        controls_.addReadOnly("gyroY", gyroYStr_, sizeof(gyroYStr_));
        controls_.addReadOnly("gyroZ", gyroZStr_, sizeof(gyroZStr_));
        controls_.addReadOnly("pitch", pitchStr_, sizeof(pitchStr_));
        controls_.addReadOnly("roll", rollStr_, sizeof(rollStr_));
    }

    void loop20ms() override {
        if (!ready_) return;

        uint8_t raw[14];
        if (!platform::i2cReadRegs(kDevAddr, kRegAccelXoutH, raw, sizeof(raw))) {
            setStatus("I2C read failed", Severity::Error);
            return;
        }

        const int16_t axRaw = readBe16(raw + 0);
        const int16_t ayRaw = readBe16(raw + 2);
        const int16_t azRaw = readBe16(raw + 4);
        const int16_t gxRaw = readBe16(raw + 8);
        const int16_t gyRaw = readBe16(raw + 10);
        const int16_t gzRaw = readBe16(raw + 12);

        gyroX_ = static_cast<float>(gxRaw) / 131.0f;
        gyroY_ = static_cast<float>(gyRaw) / 131.0f;
        gyroZ_ = static_cast<float>(gzRaw) / 131.0f;

        const float ax = static_cast<float>(axRaw) / 16384.0f;
        const float ay = static_cast<float>(ayRaw) / 16384.0f;
        const float az = static_cast<float>(azRaw) / 16384.0f;
        pitch_ = std::atan2(ax, std::sqrt(ay * ay + az * az)) * kRadToDeg;
        roll_ = std::atan2(ay, az) * kRadToDeg;

        setStatus("MPU6050 ready");
    }

    void loop1s() override {
        std::snprintf(gyroXStr_, sizeof(gyroXStr_), "%.0f °/s", gyroX_);
        std::snprintf(gyroYStr_, sizeof(gyroYStr_), "%.0f °/s", gyroY_);
        std::snprintf(gyroZStr_, sizeof(gyroZStr_), "%.0f °/s", gyroZ_);
        std::snprintf(pitchStr_, sizeof(pitchStr_), "%.0f°", pitch_);
        std::snprintf(rollStr_, sizeof(rollStr_), "%.0f°", roll_);
    }

private:
    static constexpr uint8_t kDevAddr = 0x68;
    static constexpr uint8_t kRegWhoAmI = 0x75;
    static constexpr uint8_t kWhoAmIExpected = 0x68;
    static constexpr uint8_t kRegPwrMgmt1 = 0x6B;
    static constexpr uint8_t kRegAccelXoutH = 0x3B;

    // Hardcoded until BoardModule exposes I2C pin mapping.
    static constexpr uint8_t kSdaPin = 21;
    static constexpr uint8_t kSclPin = 22;
    static constexpr float kRadToDeg = 180.0f / 3.14159265f;

    static int16_t readBe16(const uint8_t* p) {
        return static_cast<int16_t>(
            (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
    }

    bool ready_ = false;
    float gyroX_ = 0.0f;
    float gyroY_ = 0.0f;
    float gyroZ_ = 0.0f;
    float pitch_ = 0.0f;
    float roll_ = 0.0f;
    char gyroXStr_[16] = "0 °/s";
    char gyroYStr_[16] = "0 °/s";
    char gyroZStr_[16] = "0 °/s";
    char pitchStr_[16] = "0°";
    char rollStr_[16] = "0°";
};

} // namespace mm
