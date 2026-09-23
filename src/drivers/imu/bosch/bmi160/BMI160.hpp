/****************************************************************************
 *
 *   Copyright (c) 2026 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 ****************************************************************************/

/**
 * @file BMI160.hpp
 * @brief Bosch BMI160 SPI probe driver for staged Aegis FC v1 bring-up.
 */

#pragma once

#include <drivers/drv_hrt.h>
#include <lib/drivers/device/spi.h>
#include <lib/drivers/accelerometer/PX4Accelerometer.hpp>
#include <lib/drivers/gyroscope/PX4Gyroscope.hpp>
#include <lib/perf/perf_counter.h>
#include <px4_platform_common/i2c_spi_buses.h>

class BMI160 : public device::SPI, public I2CSPIDriver<BMI160>
{
public:
	BMI160(const I2CSPIDriverConfig &config);
	~BMI160() override;

	static void print_usage();

	int init() override;
	void print_status() override;
	void RunImpl();

private:
	static constexpr uint8_t RegisterChipId{0x00};
	static constexpr uint8_t RegisterGyroX{0x0C};
	static constexpr uint8_t RegisterAccelConf{0x40};
	static constexpr uint8_t RegisterAccelRange{0x41};
	static constexpr uint8_t RegisterGyroConf{0x42};
	static constexpr uint8_t RegisterGyroRange{0x43};
	static constexpr uint8_t RegisterCommand{0x7E};
	static constexpr uint8_t ChipId{0xD1};
	static constexpr uint8_t ReadBit{0x80};
	static constexpr uint8_t AccelNormalMode{0x11};
	static constexpr uint8_t GyroNormalMode{0x15};
	static constexpr uint8_t AccelConfig800Hz{0x2B};
	static constexpr uint8_t AccelRange16G{0x0C};
	static constexpr uint8_t GyroConfig800Hz{0x2B};
	static constexpr uint8_t GyroRange2000Dps{0x00};
	static constexpr uint32_t SampleIntervalUs{1'250};
	static constexpr uint16_t TransferFailureLimit{80}; // 100 ms at 800 Hz
	static constexpr hrt_abstime RecoveryCooldownUs{1000000};

	enum class State : uint8_t {
		CONFIGURE_ACCEL,
		CONFIGURE_GYRO,
		CONFIGURE_REGISTERS,
		READ,
	};

	struct DataReadBuffer {
		uint8_t cmd{static_cast<uint8_t>(RegisterGyroX | ReadBit)};
		uint8_t data[12] {};
	};

	static_assert(sizeof(DataReadBuffer) == 13);

	int probe() override;
	bool RegisterRead(uint8_t reg, uint8_t &value);
	bool RegisterWrite(uint8_t reg, uint8_t value);
	void Restart();
	static int16_t ParseInt16(const uint8_t *data);

	State _state{State::CONFIGURE_ACCEL};
	uint16_t _consecutive_transfer_failures{0};
	hrt_abstime _last_recovery_timestamp{0};
	PX4Accelerometer _px4_accel;
	PX4Gyroscope _px4_gyro;

	perf_counter_t _bad_transfer_perf{perf_alloc(PC_COUNT, MODULE_NAME ": bad transfer")};
	perf_counter_t _chip_id_read_perf{perf_alloc(PC_COUNT, MODULE_NAME ": chip ID read")};
	perf_counter_t _sample_perf{perf_alloc(PC_ELAPSED, MODULE_NAME ": read")};
	perf_counter_t _good_transfer_perf{perf_alloc(PC_COUNT, MODULE_NAME ": good transfer")};
	perf_counter_t _recovery_perf{perf_alloc(PC_COUNT, MODULE_NAME ": recovery")};
};
