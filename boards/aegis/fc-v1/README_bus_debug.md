# Aegis sensor bring-up checkpoint, 2026-11-05/06

## Assembly and IMU orientation

The BMI088 gyroscope initially returned 0xff for every register with both hardware and GPIO SPI. After the owner reworked the solder joints, both BMI088 accelerometer and gyroscope started successfully on SPI2. Firmware pin mapping was unchanged.

The assembled second IMU is BMI270, replacing BMI160 on SPI3 with CS PA15. The target enables BMI270 and uses its device type in the SPI table.

Level, nose-up and right-edge-up measurements relative to the board front identified by the owner establish driver rotation 0 for both IMUs:

| IMU | Previous rotation | Verified rotation | Level Z before | Level Z after reboot |
| --- | --- | --- | --- | --- |
| BMI088 | 4 (yaw 180) | 0 | -9.722 m/s² | -9.718 m/s² |
| BMI270 | 12 (pitch 180) | 0 | +9.899 m/s² | -9.883 m/s² |

Before correction, both sensors reported negative X when the nose was raised; raising the right edge produced negative Y on BMI088 and positive Y on BMI270. Applying rotation 0 corrects these signs and makes both level Z readings negative. Startup now uses `-R 0` for both BMI088 components and BMI270, retaining 1 MHz SPI, polling, chip selects and startup order.

Driver rotation is separate from vehicle mounting (`SENS_BOARD_ROT`). Runtime board rotation was 0 during measurement; the existing vehicle-installation default in `rc.board_defaults` was not changed. Recalibrate the IMUs after changing their rotations. Static gyro samples were collected, but no independent dynamic three-axis gyro direction test or flight test was performed.

## Barometer: owner-reported fake part, no pressure data

The owner reports buying a fake barometer sold as BMP280. Bench measurements show no ACK at BMP280 addresses 0x76/0x77. A device ACKs at 7-bit address 0x6c and returns 0x6f from register 0x01, consistently with both hardware and GPIO I2C. This does not identify the actual silicon and does not match BMP280 operation. The BMP280 driver does not start and there is no valid barometer pressure data. No replacement driver was selected and no chip-ID check was bypassed.

## Diagnostic support and validation

The target enables the bench-only `aegis_busdiag` command for SPI/I2C register reads, verified writes to identified BMI088 devices, and independent GPIO bus tests. It requires a disarmed, non-prearmed vehicle; raw SPI tests require both BMI088 drivers stopped. `bmi088 -v ... start` enables bounded register traces and diagnostic write/readback delay. Normal startup does not enable BMI088 tracing. BMP280 I2C probe traces are gated by the diagnostic target configuration.

Generated NuttX builtin headers are excluded from the source dependency glob to prevent the incremental-build missing-header failure. The target also includes its bootloader binary under `extras/`.

`make aegis_fc-v1_default` passed. Application flash usage: 1,786,348 / 1,835,008 bytes. Uploader checked board ID 1179, programmed and verified 100%, without force or bootloader replacement. The uploaded application artifact SHA-256 was `289762afde2455b3cad5ce40b62cfdbfcf1a1996875caf7784a6e3e7acd6c62f` (before this checkpoint commit).

After reboot, the on-board startup script confirmed all three IMU start commands use rotation 0. Thirty accelerometer samples per IMU and ten stationary gyro samples per IMU were checked. All three driver instances reported zero bad-register, bad-transfer, FIFO-empty and FIFO-overflow counters; FIFO reset was 1 and BMI088 accel recovery was 0 at the final check. These are short bench checks, not long-term stability validation. No calibration parameters were explicitly written; PX4 may update learned biases while running.

Full local measurement logs are in the separate hardware workspace:

- `/home/zynh/Projects/Aegis-FC/docs/diagnostics/2026-11-05-register-debug/`
- `/home/zynh/Projects/Aegis-FC/docs/diagnostics/2026-11-05-imu-orientation/`

## Follow-up: board-to-body rotation, 2026-11-06

The R0 driver outputs agree between both accelerometers in nose-down and right-edge-down tests. A further comparison found vehicle_imu X/Y reversed relative to sensor_accel, despite the CLI reporting SENS_BOARD_ROT=0. Refreshing board rotation from 0 to 4 and back to 0 corrected the applied transform. The startup default still specified yaw 180; it is now changed to 0 for the board front used in these checks. The exact earlier event that left the applied transform inconsistent with the CLI value has not been identified.

After building, uploading and verifying the application, ROMFS readback confirms default 0 and all IMU drivers R0. A static tilted sample after reboot confirms raw and body X/Y have matching signs for both IMUs. Existing level adjustment and calibration offsets/scales were retained.

Simultaneous 50 Hz SCALED_IMU/2 captures show roll gyro correlation 0.9990 and pitch correlation 0.9584 after small time alignment. The pitch capture contains abrupt high-rate motion with larger transient differences; these samples do not establish flight readiness or distinguish sampling/filter effects from sensor faults at the peaks. No independent yaw-motion test was made. Full local follow-up logs: `/home/zynh/Projects/Aegis-FC/docs/diagnostics/2026-11-06-imu-recheck/`.
