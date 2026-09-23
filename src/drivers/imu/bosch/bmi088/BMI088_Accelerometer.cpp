/****************************************************************************
 *
 *   Copyright (c) 2020-2023 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include "BMI088_Accelerometer.hpp"

#include <geo/geo.h> // CONSTANTS_ONE_G
#include <px4_platform_common/time.h>

using namespace time_literals;

namespace Bosch::BMI088::Accelerometer
{

BMI088_Accelerometer::BMI088_Accelerometer(const I2CSPIDriverConfig &config) :
	BMI088(config),
	_px4_accel(get_device_id(), config.rotation)
{
	if (config.drdy_gpio != 0) {
		_drdy_missed_perf = perf_alloc(PC_COUNT, MODULE_NAME"_accel: DRDY missed");
	}

	ConfigureSampleRate(_px4_accel.get_max_rate_hz());
}

BMI088_Accelerometer::~BMI088_Accelerometer()
{
	perf_free(_bad_register_perf);
	perf_free(_bad_transfer_perf);
	perf_free(_fifo_empty_perf);
	perf_free(_fifo_overflow_perf);
	perf_free(_fifo_reset_perf);
	perf_free(_recovery_perf);
	perf_free(_drdy_missed_perf);
}

void BMI088_Accelerometer::exit_and_cleanup()
{
	DataReadyInterruptDisable();
	I2CSPIDriverBase::exit_and_cleanup();
}

void BMI088_Accelerometer::print_status()
{
	I2CSPIDriverBase::print_status();

	PX4_INFO("FIFO empty interval: %d us (%.1f Hz)", _fifo_empty_interval_us, 1e6 / _fifo_empty_interval_us);

	perf_print_counter(_bad_register_perf);
	perf_print_counter(_bad_transfer_perf);
	perf_print_counter(_fifo_empty_perf);
	perf_print_counter(_fifo_overflow_perf);
	perf_print_counter(_fifo_reset_perf);
	perf_print_counter(_recovery_perf);
	perf_print_counter(_drdy_missed_perf);
}

int BMI088_Accelerometer::probe()
{
	/* 6.1 Serial Peripheral Interface (SPI)
	 * ... the accelerometer part starts always in I2C mode
	 * (regardless of the level of the PS pin) and needs to be changed to SPI
	 *  mode actively by sending a rising edge on the CSB1 pin
	 *  (chip select of the accelerometer), on which the accelerometer part
	 *  switches to SPI mode and stays in this mode until the next power-up-reset.
	 *
	 *  To change the sensor to SPI mode in the initialization phase, the user
	 *  could perfom a dummy SPI read operation, e.g. of register ACC_CHIP_ID
	 *  (the obtained value will be invalid).In case of read operations,
	 */
	const uint8_t dummy_chip_id = RegisterRead(Register::ACC_CHIP_ID);
	// Allow the CS edge from the dummy read to latch SPI mode before the first
	// real register access. This board has no external CS pull-up.
	px4_usleep(5000);

	const uint8_t ACC_CHIP_ID = RegisterRead(Register::ACC_CHIP_ID);
	PX4_DEBUG("probe SPI %lu Hz: dummy 0x%02x, ACC_CHIP_ID 0x%02x (expected 0x%02x or 0x%02x)",
		 static_cast<unsigned long>(get_frequency()), dummy_chip_id, ACC_CHIP_ID, ID_088, ID_090L);

	if (ACC_CHIP_ID == ID_088) {
		DEVICE_DEBUG("BMI088 Accel");

	} else if (ACC_CHIP_ID == ID_090L) {
		DEVICE_DEBUG("BMI090L Accel");

	} else {
		DEVICE_DEBUG("unexpected ACC_CHIP_ID 0x%02x", ACC_CHIP_ID);
		return PX4_ERROR;
	}

	return PX4_OK;
}

void BMI088_Accelerometer::RunImpl()
{
	const hrt_abstime now = hrt_absolute_time();

	switch (_state) {
	case STATE::RESET:
		// AEGIS FC v1.0: do not issue ACC_SOFTRESET here. On this board the
		// device is reachable immediately after probe but ceases responding after
		// the reset command. Configure the existing SPI session instead.
		DataReadyInterruptDisable();
		_reset_timestamp = now;
		_failure_count = 0;
		_normal_mode_requested = false;
		_state = STATE::WAIT_FOR_RESET;
		ScheduleDelayed(1_ms);
		break;

	case STATE::WAIT_FOR_RESET: {
		const uint8_t chip_id = RegisterRead(Register::ACC_CHIP_ID);

		if ((chip_id == ID_088) || (chip_id == ID_090L)) {
			PX4_DEBUG("reset complete: ACC_CHIP_ID 0x%02x", chip_id);
			// ACC_PWR_CONF: power on sensor before enabling normal mode in Configure().
			RegisterWrite(Register::ACC_PWR_CONF, 0);

			// if reset succeeded then configure
			_state = STATE::CONFIGURE;
			ScheduleDelayed(5_ms);

		} else {
			// RESET not complete
			if (hrt_elapsed_time(&_reset_timestamp) > 1000_ms) {
				PX4_WARN("reset failed: ACC_CHIP_ID 0x%02x, retrying", chip_id);
				_state = STATE::RESET;
				ScheduleDelayed(100_ms);

			} else {
				PX4_DEBUG("Reset not complete, check again in 10 ms");
				ScheduleDelayed(10_ms);
			}
		}

		break;
	}

	case STATE::CONFIGURE:
		if (!_normal_mode_requested) {
			// The BMI088 requires at least 450 us after ACC_PWR_CTRL enables
			// normal mode before another register access. Continuing immediately
			// makes the accelerometer stop responding on AEGIS FC v1.0.
			RegisterWrite(Register::ACC_PWR_CTRL, ACC_PWR_CTRL_BIT::acc_enable);
			_normal_mode_requested = true;
			PX4_DEBUG("ACC normal mode requested; waiting 5 ms before configuration");
			ScheduleDelayed(5_ms);
			break;
		}

		if (Configure()) {
			// if configure succeeded then reset the FIFO
			_state = STATE::FIFO_RESET;
			ScheduleDelayed(10_ms);

		} else {
			// CONFIGURE not complete
			if (hrt_elapsed_time(&_reset_timestamp) > 1000_ms) {
				PX4_DEBUG("configuration failed, retrying without soft reset");
				_state = STATE::RESET;

			} else {
				PX4_DEBUG("Configure failed, retrying");
			}

			ScheduleDelayed(100_ms);
		}

		break;

	case STATE::FIFO_RESET:
		// if configure succeeded then start reading from FIFO
		_state = STATE::FIFO_READ;

		FIFOReset();
		_last_success_timestamp = now;

		if (DataReadyInterruptConfigure()) {
			_data_ready_interrupt_enabled = true;
			PX4_INFO("FIFO read started with DRDY interrupt");

			// backup schedule as a watchdog timeout
			ScheduleDelayed(100_ms);

		} else {
			_data_ready_interrupt_enabled = false;
			PX4_INFO("FIFO read started with polling interval %u us", static_cast<unsigned>(_fifo_empty_interval_us));
			ScheduleOnInterval(_fifo_empty_interval_us, _fifo_empty_interval_us);
		}

		break;

	case STATE::FIFO_READ: {
			hrt_abstime timestamp_sample = now;
			uint8_t samples = 0;

			if (_data_ready_interrupt_enabled) {
				// scheduled from interrupt if _drdy_timestamp_sample was set as expected
				const hrt_abstime drdy_timestamp_sample = _drdy_timestamp_sample.fetch_and(0);

				if ((now - drdy_timestamp_sample) < _fifo_empty_interval_us) {
					timestamp_sample = drdy_timestamp_sample;
					samples = _fifo_samples;

				} else {
					perf_count(_drdy_missed_perf);
				}

				// push backup schedule back
				ScheduleDelayed(_fifo_empty_interval_us * 2);
			}

			if (samples == 0) {
				// check current FIFO count
				const uint16_t fifo_byte_counter = FIFOReadCount();

				if (fifo_byte_counter >= FIFO::SIZE) {
					PX4_WARN("ACC FIFO overflow (%u bytes), discarding", static_cast<unsigned>(fifo_byte_counter));
					FIFOReset();
					perf_count(_fifo_overflow_perf);

				} else if ((fifo_byte_counter == 0) || (fifo_byte_counter == 0x8000)) {
					// An empty FIFO corresponds to 0x8000
					perf_count(_fifo_empty_perf);

				} else {
					samples = fifo_byte_counter / sizeof(FIFO::DATA);

					// tolerate minor jitter, leave sample to next iteration if behind by only 1
					if (samples == _fifo_samples + 1) {
						timestamp_sample -= static_cast<int>(FIFO_SAMPLE_DT);
						samples--;
					}

					if (samples > FIFO_MAX_SAMPLES) {
						// not technically an overflow, but more samples than we expected or can publish
						FIFOReset();
						perf_count(_fifo_overflow_perf);
						samples = 0;
					}
				}
			}

			bool success = false;

			if (samples >= 1) {
				if (FIFORead(timestamp_sample, samples)) {
					success = true;

					if (_failure_count > 0) {
						_failure_count--;
					}
				}
			}

			if (!success) {
				_failure_count++;

				// A short no-data period is expected after FIFO setup. Once the
				// stream has been absent for long enough, rerun the known-good
				// configuration sequence instead of leaving a failed sensor silent.
				if ((hrt_elapsed_time(&_last_success_timestamp) > DATA_TIMEOUT_US)
				    && ((_last_recovery_timestamp == 0)
					|| (hrt_elapsed_time(&_last_recovery_timestamp) > RECOVERY_COOLDOWN_US))) {
					_last_recovery_timestamp = now;
					perf_count(_recovery_perf);
					PX4_WARN("ACC data timeout, reconfiguring");
					Reset();
					return;
				}
			}

			// AEGIS FC v1.0: after a FIFO burst, reads of configuration registers
			// can return FIFO payload bytes (for example ACC_PWR_CTRL returns 0x19
			// instead of 0x04) even though valid FIFO frames are arriving. Do not
			// reset a healthy stream based on that false register-check failure.
			if (success) {
				_failure_count = 0;
				_last_success_timestamp = now;

				// periodically update temperature (~1 Hz)
				if (hrt_elapsed_time(&_temperature_update_timestamp) >= 1_s) {
					UpdateTemperature();
					_temperature_update_timestamp = now;
				}
			}
		}

		break;
	}
}

void BMI088_Accelerometer::ConfigureAccel()
{
	const uint8_t ACC_RANGE = RegisterRead(Register::ACC_RANGE) & (Bit1 | Bit0);

	switch (ACC_RANGE) {
	case acc_range_3g:
		_px4_accel.set_scale(CONSTANTS_ONE_G * (powf(2, ACC_RANGE + 1) * 1.5f) / 32768.f);
		_px4_accel.set_range(3.f * CONSTANTS_ONE_G);
		break;

	case acc_range_6g:
		_px4_accel.set_scale(CONSTANTS_ONE_G * (powf(2, ACC_RANGE + 1) * 1.5f) / 32768.f);
		_px4_accel.set_range(6.f * CONSTANTS_ONE_G);
		break;

	case acc_range_12g:
		_px4_accel.set_scale(CONSTANTS_ONE_G * (powf(2, ACC_RANGE + 1) * 1.5f) / 32768.f);
		_px4_accel.set_range(12.f * CONSTANTS_ONE_G);
		break;

	case acc_range_24g:
		_px4_accel.set_scale(CONSTANTS_ONE_G * (powf(2, ACC_RANGE + 1) * 1.5f) / 32768.f);
		_px4_accel.set_range(24.f * CONSTANTS_ONE_G);
		break;
	}
}

void BMI088_Accelerometer::ConfigureSampleRate(int sample_rate)
{
	// round down to nearest FIFO sample dt * SAMPLES_PER_TRANSFER
	const float min_interval = FIFO_SAMPLE_DT;
	_fifo_empty_interval_us = math::max(roundf((1e6f / (float)sample_rate) / min_interval) * min_interval, min_interval);

	_fifo_samples = math::min((float)_fifo_empty_interval_us / (1e6f / RATE), (float)FIFO_MAX_SAMPLES);

	// recompute FIFO empty interval (us) with actual sample limit
	_fifo_empty_interval_us = _fifo_samples * (1e6f / RATE);

	ConfigureFIFOWatermark(_fifo_samples);
}

void BMI088_Accelerometer::ConfigureFIFOWatermark(uint8_t samples)
{
	// FIFO_WTM: 13 bit FIFO watermark level value
	// unit of the fifo watermark is one byte
	const uint16_t fifo_watermark_threshold = samples * sizeof(FIFO::DATA);

	for (auto &r : _register_cfg) {
		if (r.reg == Register::FIFO_WTM_0) {
			// fifo_water_mark[7:0]
			r.set_bits = fifo_watermark_threshold & 0x00FF;
			r.clear_bits = ~r.set_bits;

		} else if (r.reg == Register::FIFO_WTM_1) {
			// fifo_water_mark[12:8]
			r.set_bits = (fifo_watermark_threshold & 0x0700) >> 8;
			r.clear_bits = ~r.set_bits;
		}
	}
}

bool BMI088_Accelerometer::Configure()
{
	// first set and clear all configured register bits
	for (const auto &reg_cfg : _register_cfg) {
		// ACC_PWR_CONF was written in WAIT_FOR_RESET and ACC_PWR_CTRL was
		// written in the preceding CONFIGURE pass. Do not access either until
		// the required normal-mode settling delay has elapsed.
		if ((reg_cfg.reg == Register::ACC_PWR_CONF) || (reg_cfg.reg == Register::ACC_PWR_CTRL)) {
			continue;
		}

		RegisterSetAndClearBits(reg_cfg.reg, reg_cfg.set_bits, reg_cfg.clear_bits);
	}

	// now check that all are configured
	bool success = true;

	for (const auto &reg_cfg : _register_cfg) {
		if (!RegisterCheck(reg_cfg)) {
			success = false;
		}
	}

	ConfigureAccel();

	return success;
}

int BMI088_Accelerometer::DataReadyInterruptCallback(int irq, void *context, void *arg)
{
	static_cast<BMI088_Accelerometer *>(arg)->DataReady();
	return 0;
}

void BMI088_Accelerometer::DataReady()
{
	_drdy_timestamp_sample.store(hrt_absolute_time());
	ScheduleNow();
}

bool BMI088_Accelerometer::DataReadyInterruptConfigure()
{
	if (_drdy_gpio == 0) {
		return false;
	}

	// Setup data ready on falling edge
	return px4_arch_gpiosetevent(_drdy_gpio, false, true, true, &DataReadyInterruptCallback, this) == 0;
}

bool BMI088_Accelerometer::DataReadyInterruptDisable()
{
	if (_drdy_gpio == 0) {
		return false;
	}

	return px4_arch_gpiosetevent(_drdy_gpio, false, false, false, nullptr, nullptr) == 0;
}

bool BMI088_Accelerometer::RegisterCheck(const register_config_t &reg_cfg)
{
	bool success = true;

	const uint8_t reg_value = RegisterRead(reg_cfg.reg);

	if (reg_cfg.set_bits && ((reg_value & reg_cfg.set_bits) != reg_cfg.set_bits)) {
		PX4_DEBUG("ACC register check 0x%02hhX: 0x%02hhX (0x%02hhX not set)", (uint8_t)reg_cfg.reg, reg_value,
			  reg_cfg.set_bits);
		success = false;
	}

	if (reg_cfg.clear_bits && ((reg_value & reg_cfg.clear_bits) != 0)) {
		PX4_DEBUG("ACC register check 0x%02hhX: 0x%02hhX (0x%02hhX not cleared)", (uint8_t)reg_cfg.reg, reg_value,
			  reg_cfg.clear_bits);
		success = false;
	}

	return success;
}

uint8_t BMI088_Accelerometer::RegisterRead(Register reg)
{
	// 6.1.2 SPI interface of accelerometer part
	//
	// In case of read operations of the accelerometer part, the requested data
	// is not sent immediately, but instead first a dummy byte is sent, and
	// after this dummy byte the actual requested register content is transmitted.
	uint8_t cmd[3] {};
	cmd[0] = static_cast<uint8_t>(reg) | DIR_READ;
	// cmd[1] dummy byte
	transfer(cmd, cmd, sizeof(cmd));
	return cmd[2];
}

void BMI088_Accelerometer::RegisterWrite(Register reg, uint8_t value)
{
	uint8_t cmd[2] { (uint8_t)reg, value };
	transfer(cmd, cmd, sizeof(cmd));
}

void BMI088_Accelerometer::RegisterSetAndClearBits(Register reg, uint8_t setbits, uint8_t clearbits)
{
	const uint8_t orig_val = RegisterRead(reg);

	uint8_t val = (orig_val & ~clearbits) | setbits;

	if (orig_val != val) {
		RegisterWrite(reg, val);
	}
}

uint16_t BMI088_Accelerometer::FIFOReadCount()
{
	// FIFO length registers FIFO_LENGTH_1 and FIFO_LENGTH_0 contain the 14 bit FIFO byte
	uint8_t fifo_len_buf[4] {};
	fifo_len_buf[0] = static_cast<uint8_t>(Register::FIFO_LENGTH_0) | DIR_READ;
	// fifo_len_buf[1] dummy byte

	if (transfer(&fifo_len_buf[0], &fifo_len_buf[0], sizeof(fifo_len_buf)) != PX4_OK) {
		perf_count(_bad_transfer_perf);
		return 0;
	}

	const uint8_t FIFO_LENGTH_0 = fifo_len_buf[2];        // fifo_byte_counter[7:0]
	const uint8_t FIFO_LENGTH_1 = fifo_len_buf[3] & 0x3F; // fifo_byte_counter[13:8]

	return combine(FIFO_LENGTH_1, FIFO_LENGTH_0);
}

bool BMI088_Accelerometer::FIFORead(const hrt_abstime &timestamp_sample, uint8_t samples)
{
	FIFOTransferBuffer buffer{};
	const size_t transfer_size = math::min(samples * sizeof(FIFO::DATA) + 4, FIFO::SIZE);

	if (transfer((uint8_t *)&buffer, (uint8_t *)&buffer, transfer_size) != PX4_OK) {
		perf_count(_bad_transfer_perf);
		return false;
	}

	const size_t fifo_byte_counter = combine(buffer.FIFO_LENGTH_1 & 0x3F, buffer.FIFO_LENGTH_0);

	// An empty FIFO corresponds to 0x8000
	if (fifo_byte_counter == 0x8000) {
		perf_count(_fifo_empty_perf);
		return false;

	} else if (fifo_byte_counter >= FIFO::SIZE) {
		perf_count(_fifo_overflow_perf);
		return false;
	}

	sensor_accel_fifo_s accel{};
	accel.timestamp_sample = timestamp_sample;
	accel.samples = 0;
	accel.dt = FIFO_SAMPLE_DT;

	// first find all sensor data frames in the buffer
	uint8_t *data_buffer = (uint8_t *)&buffer.f[0];
	unsigned fifo_buffer_index = 0; // start of buffer

	while (fifo_buffer_index < math::min(fifo_byte_counter, transfer_size - 4)) {
		// look for header signature (first 6 bits) followed by two bits indicating the status of INT1 and INT2
		switch (data_buffer[fifo_buffer_index] & 0xFC) {
		case FIFO::header::sensor_data_frame: {
				// Acceleration sensor data frame
				// Frame length: 7 bytes (1 byte header + 6 bytes payload)

				FIFO::DATA *fifo_sample = (FIFO::DATA *)&data_buffer[fifo_buffer_index];
				const int16_t accel_x = combine(fifo_sample->ACC_X_MSB, fifo_sample->ACC_X_LSB);
				const int16_t accel_y = combine(fifo_sample->ACC_Y_MSB, fifo_sample->ACC_Y_LSB);
				const int16_t accel_z = combine(fifo_sample->ACC_Z_MSB, fifo_sample->ACC_Z_LSB);

				// sensor's frame is +x forward, +y left, +z up
				//  flip y & z to publish right handed with z down (x forward, y right, z down)
				accel.x[accel.samples] = accel_x;
				accel.y[accel.samples] = (accel_y == INT16_MIN) ? INT16_MAX : -accel_y;
				accel.z[accel.samples] = (accel_z == INT16_MIN) ? INT16_MAX : -accel_z;
				accel.samples++;

				fifo_buffer_index += 7; // move forward to next record
			}
			break;

		case FIFO::header::skip_frame:
			// Skip Frame
			// Frame length: 2 bytes (1 byte header + 1 byte payload)
			PX4_DEBUG("Skip Frame");
			fifo_buffer_index += 2;
			break;

		case FIFO::header::sensor_time_frame:
			// Sensortime Frame
			// Frame length: 4 bytes (1 byte header + 3 bytes payload)
			PX4_DEBUG("Sensortime Frame");
			fifo_buffer_index += 4;
			break;

		case FIFO::header::FIFO_input_config_frame:
			// FIFO input config Frame
			// Frame length: 2 bytes (1 byte header + 1 byte payload)
			PX4_DEBUG("FIFO input config Frame");
			fifo_buffer_index += 2;
			break;

		case FIFO::header::sample_drop_frame:
			// Sample drop Frame
			// Frame length: 2 bytes (1 byte header + 1 byte payload)
			PX4_DEBUG("Sample drop Frame");
			fifo_buffer_index += 2;
			break;

		default:
			fifo_buffer_index++;
			break;
		}
	}

	_px4_accel.set_error_count(perf_event_count(_bad_register_perf) + perf_event_count(_bad_transfer_perf) +
				   perf_event_count(_fifo_empty_perf) + perf_event_count(_fifo_overflow_perf));

	if (accel.samples > 0) {
		_px4_accel.updateFIFO(accel);
		return true;
	}

	return false;
}

bool BMI088_Accelerometer::FIFOFlush()
{
	const uint16_t fifo_byte_counter = FIFOReadCount();

	if ((fifo_byte_counter == 0) || (fifo_byte_counter == 0x8000)) {
		return true;
	}

	// FIFO frames can be 2, 4, or 7 bytes. The BMI088 repeats any frame that
	// is read only partly, so splitting a drain into fixed-size chunks can leave
	// the FIFO perpetually non-empty. A single 1024-byte burst is at least one
	// full FIFO depth and ends only after all frames present at its start have
	// been consumed. The SPI2 DMA buffer is 2048 bytes; this is a startup-only
	// transaction and does not affect steady-state polling load.
	_fifo_flush_buffer.cmd = static_cast<uint8_t>(Register::FIFO_LENGTH_0) | DIR_READ;

	if (transfer(reinterpret_cast<uint8_t *>(&_fifo_flush_buffer), reinterpret_cast<uint8_t *>(&_fifo_flush_buffer),
		     sizeof(_fifo_flush_buffer)) != PX4_OK) {
		perf_count(_bad_transfer_perf);
		return false;
	}

	const uint16_t fifo_byte_counter_after = FIFOReadCount();
	const uint16_t bytes_remaining = (fifo_byte_counter_after == 0x8000) ? 0 : fifo_byte_counter_after;

	// A software reboot leaves the sensor powered and can leave almost its
	// entire FIFO full. Make this distinct from normal startup without adding
	// any logging or work to the steady-state sampling path.
	if (fifo_byte_counter > FIFO_MAX_SAMPLES * sizeof(FIFO::DATA)) {
		PX4_INFO("ACC discarded %u stale FIFO bytes after MCU reset (%u bytes remain)",
			 static_cast<unsigned>(fifo_byte_counter), static_cast<unsigned>(bytes_remaining));
	}

	return true;
}

void BMI088_Accelerometer::FIFOReset()
{
	perf_count(_fifo_reset_perf);

	// PX4 normally writes 0xB0 to ACC_SOFTRESET here. On AEGIS FC v1.0 that
	// command can make the accelerometer stop responding. Draining FIFO_DATA
	// has the same FIFO-clearing effect while leaving the sensor configuration
	// and its SPI session intact. This matters after an MCU-only reboot, where
	// the powered sensor retains samples from before the reboot.
	FIFOFlush();

	_drdy_timestamp_sample.store(0);
}

void BMI088_Accelerometer::UpdateTemperature()
{
	// stored in an 11-bit value in 2’s complement format
	uint8_t temperature_buf[4] {};
	temperature_buf[0] = static_cast<uint8_t>(Register::TEMP_MSB) | DIR_READ;
	// temperature_buf[1] dummy byte

	if (transfer(&temperature_buf[0], &temperature_buf[0], sizeof(temperature_buf)) != PX4_OK) {
		perf_count(_bad_transfer_perf);
		return;
	}

	const uint8_t TEMP_MSB = temperature_buf[2];
	const uint8_t TEMP_LSB = temperature_buf[3];

	// Datasheet 5.3.7: Register 0x22 – 0x23: Temperature sensor data
	uint16_t Temp_uint11 = (TEMP_MSB * 8) + (TEMP_LSB / 32);
	int16_t Temp_int11 = 0;

	if (Temp_uint11 > 1023) {
		Temp_int11 = Temp_uint11 - 2048;

	} else {
		Temp_int11 = Temp_uint11;
	}

	float temperature = (Temp_int11 * 0.125f) + 23.f; // Temp_int11 * 0.125°C/LSB + 23°C

	if (PX4_ISFINITE(temperature)) {
		_px4_accel.set_temperature(temperature);

	} else {
		perf_count(_bad_transfer_perf);
	}
}

} // namespace Bosch::BMI088::Accelerometer
