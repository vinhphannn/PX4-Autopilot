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

#include "BMI160.hpp"

#include <lib/geo/geo.h>
#include <px4_platform_common/time.h>

using namespace time_literals;

BMI160::BMI160(const I2CSPIDriverConfig &config) :
	SPI(config),
	I2CSPIDriver(config),
	_px4_accel(get_device_id(), config.rotation),
	_px4_gyro(get_device_id(), config.rotation)
{
	_px4_accel.set_range(16.f * CONSTANTS_ONE_G);
	_px4_accel.set_scale(CONSTANTS_ONE_G / 2048.f); // +/-16 g, 2048 LSB/g
	_px4_gyro.set_range(math::radians(2000.f));
	_px4_gyro.set_scale(math::radians(1.f / 16.4f)); // +/-2000 dps, 16.4 LSB/(dps)
}

BMI160::~BMI160()
{
	perf_free(_bad_transfer_perf);
	perf_free(_chip_id_read_perf);
	perf_free(_sample_perf);
	perf_free(_good_transfer_perf);
}

int BMI160::init()
{
	const int ret = SPI::init();

	if (ret != PX4_OK) {
		DEVICE_DEBUG("SPI::init failed (%d)", ret);
		return ret;
	}

	// The staged driver deliberately does not soft-reset BMI160. We first
	// configure the existing SPI session, matching the known-good legacy PX4
	// initialization sequence and avoiding an unnecessary bus-state change.
	_state = State::CONFIGURE_ACCEL;
	ScheduleNow();
	return PX4_OK;
}

int BMI160::probe()
{
	// The BMI160 SPI protocol returns register data in the byte following the
	// read command. This is the same two-byte CHIP_ID transfer used by PX4's
	// historical BMI160 driver. Do not reset or configure the device here.
	uint8_t first_id = 0;
	uint8_t chip_id = 0;

	if (!RegisterRead(RegisterChipId, first_id)) {
		PX4_WARN("probe SPI %lu Hz: first CHIP_ID transfer failed", static_cast<unsigned long>(get_frequency()));
		return PX4_ERROR;
	}

	px4_usleep(200);

	if (!RegisterRead(RegisterChipId, chip_id)) {
		PX4_WARN("probe SPI %lu Hz: second CHIP_ID transfer failed", static_cast<unsigned long>(get_frequency()));
		return PX4_ERROR;
	}

	PX4_INFO("probe SPI %lu Hz: CHIP_ID 0x%02x -> 0x%02x (expected 0x%02x)",
		 static_cast<unsigned long>(get_frequency()), first_id, chip_id, ChipId);

	return (chip_id == ChipId) ? PX4_OK : PX4_ERROR;
}

bool BMI160::RegisterRead(uint8_t reg, uint8_t &value)
{
	uint8_t buffer[2] {static_cast<uint8_t>(reg | ReadBit), 0};

	if (transfer(buffer, buffer, sizeof(buffer)) != PX4_OK) {
		perf_count(_bad_transfer_perf);
		return false;
	}

	value = buffer[1];
	perf_count(_chip_id_read_perf);
	return true;
}

bool BMI160::RegisterWrite(uint8_t reg, uint8_t value)
{
	uint8_t buffer[2] {reg, value};

	if (transfer(buffer, nullptr, sizeof(buffer)) != PX4_OK) {
		perf_count(_bad_transfer_perf);
		return false;
	}

	return true;
}

int16_t BMI160::ParseInt16(const uint8_t *data)
{
	return static_cast<int16_t>(static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8));
}

void BMI160::RunImpl()
{
	switch (_state) {
	case State::CONFIGURE_ACCEL:
		if (RegisterWrite(RegisterCommand, AccelNormalMode)) {
			PX4_INFO("ACC normal mode requested; waiting 5 ms");
			_state = State::CONFIGURE_GYRO;
			ScheduleDelayed(5_ms);

		} else {
			PX4_WARN("ACC normal-mode command failed; retrying");
			ScheduleDelayed(100_ms);
		}

		break;

	case State::CONFIGURE_GYRO:
		if (RegisterWrite(RegisterCommand, GyroNormalMode)) {
			PX4_INFO("GYR normal mode requested; waiting 85 ms");
			_state = State::CONFIGURE_REGISTERS;
			ScheduleDelayed(85_ms);

		} else {
			PX4_WARN("GYR normal-mode command failed; retrying");
			ScheduleDelayed(100_ms);
		}

		break;

	case State::CONFIGURE_REGISTERS: {
		uint8_t accel_conf = 0;
		uint8_t accel_range = 0;
		uint8_t gyro_conf = 0;
		uint8_t gyro_range = 0;

		const bool configured = RegisterWrite(RegisterAccelConf, AccelConfig800Hz)
						&& RegisterWrite(RegisterAccelRange, AccelRange16G)
						&& RegisterWrite(RegisterGyroConf, GyroConfig800Hz)
						&& RegisterWrite(RegisterGyroRange, GyroRange2000Dps)
						&& RegisterRead(RegisterAccelConf, accel_conf)
						&& RegisterRead(RegisterAccelRange, accel_range)
						&& RegisterRead(RegisterGyroConf, gyro_conf)
						&& RegisterRead(RegisterGyroRange, gyro_range);

		if (configured && accel_conf == AccelConfig800Hz && accel_range == AccelRange16G
		    && gyro_conf == GyroConfig800Hz && gyro_range == GyroRange2000Dps) {
			PX4_INFO("configuration verified: ACC 0x%02x/0x%02x, GYR 0x%02x/0x%02x", accel_conf, accel_range,
				 gyro_conf, gyro_range);
			_state = State::READ;
			ScheduleOnInterval(SampleIntervalUs, SampleIntervalUs);

		} else {
			PX4_WARN("configuration mismatch: ACC 0x%02x/0x%02x, GYR 0x%02x/0x%02x; retrying", accel_conf,
				 accel_range, gyro_conf, gyro_range);
			ScheduleDelayed(100_ms);
		}

		break;
	}

	case State::READ: {
		DataReadBuffer buffer{};
		const hrt_abstime timestamp_sample = hrt_absolute_time();
		perf_begin(_sample_perf);

		if (transfer(reinterpret_cast<uint8_t *>(&buffer), reinterpret_cast<uint8_t *>(&buffer), sizeof(buffer)) != PX4_OK) {
			perf_count(_bad_transfer_perf);
			perf_end(_sample_perf);
			break;
		}

		const int16_t gyro_x = ParseInt16(&buffer.data[0]);
		const int16_t gyro_y = ParseInt16(&buffer.data[2]);
		const int16_t gyro_z = ParseInt16(&buffer.data[4]);
		const int16_t accel_x = ParseInt16(&buffer.data[6]);
		const int16_t accel_y = ParseInt16(&buffer.data[8]);
		const int16_t accel_z = ParseInt16(&buffer.data[10]);

		const uint32_t error_count = perf_event_count(_bad_transfer_perf);
		_px4_accel.set_error_count(error_count);
		_px4_gyro.set_error_count(error_count);
		_px4_accel.update(timestamp_sample, accel_x, accel_y, accel_z);
		_px4_gyro.update(timestamp_sample, gyro_x, gyro_y, gyro_z);
		perf_count(_good_transfer_perf);
		perf_end(_sample_perf);
		break;
	}
	}
}

void BMI160::print_status()
{
	I2CSPIDriverBase::print_status();
	PX4_INFO("polling interval: %lu us (%.1f Hz)", static_cast<unsigned long>(SampleIntervalUs),
		 static_cast<double>(1e6f / SampleIntervalUs));
	perf_print_counter(_bad_transfer_perf);
	perf_print_counter(_chip_id_read_perf);
	perf_print_counter(_sample_perf);
	perf_print_counter(_good_transfer_perf);
}
