# Gyro Driver

Reads an MPU6050 IMU over I2C and surfaces gyro rates and tilt angles in the UI. Lives under the Drivers container as an input-only driver (it ignores the light buffer).

## Controls

Read-only telemetry updated once per second from 50 Hz sensor polls:

- `gyroX` (read-only text) — X-axis angular rate in °/s
- `gyroY` (read-only text) — Y-axis angular rate in °/s
- `gyroZ` (read-only text) — Z-axis angular rate in °/s
- `pitch` (read-only text) — tilt pitch from accelerometer, degrees
- `roll` (read-only text) — tilt roll from accelerometer, degrees

## Status

The module card header shows one of:

- `MPU6050 ready` — WHO_AM_I matched and reads succeed
- `MPU6050 not found` — WHO_AM_I read failed or wrong device (severity: error)
- `I2C init failed` / `MPU6050 wake failed` / `I2C read failed` — bus or register errors (severity: error)

## I2C wire contract

| Field | Value |
|-------|-------|
| 7-bit address | `0x68` |
| SDA pin | GPIO 21 (hardcoded until BoardModule supplies pins) |
| SCL pin | GPIO 22 |
| WHO_AM_I | register `0x75`, expect `0x68` |
| Wake | write `0` to PWR_MGMT_1 (`0x6B`) |
| Sample burst | 14 bytes from ACCEL_XOUT_H (`0x3B`): accel XYZ + temp + gyro XYZ, big-endian int16 |

Default scales: gyro ±250 °/s (131 LSB/°/s), accel ±2 g (16384 LSB/g). Pitch and roll are computed with `atan2` on the accelerometer vector — no fusion filter.

Desktop builds use a platform-layer MPU6050 simulation so the UI and unit tests see live values without hardware.

## Tests

- [Unit tests: GyroDriver](../../../tests/unit-tests.md#gyrodriver) — WHO_AM_I probe, simulated burst parse, control formatting.

## Prior art

### MoonLight — D_IMU

MoonLight inventory lists `D_IMU` as a gyroscope/accelerometer driver type. SharedData carried gravity/IMU samples between nodes for orientation-aware effects.
