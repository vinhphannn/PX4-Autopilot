/****************************************************************************
 *
 *   Copyright (c) 2020-2021 PX4 Development Team. All rights reserved.
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

#pragma once

#include "BMI088.hpp"

#include <lib/drivers/accelerometer/PX4Accelerometer.hpp>

#include "Bosch_BMI088_Accelerometer_Registers.hpp"

namespace Bosch::BMI088::Accelerometer
{

class BMI088_Accelerometer : public BMI088
{
public:
	BMI088_Accelerometer(const I2CSPIDriverConfig &config);
	~BMI088_Accelerometer() override;

	void RunImpl() override;
	void print_status() override;

private:
	void exit_and_cleanup() override;

	// Sensor Configuration
	static constexpr uint32_t RATE{1600}; // 1600 Hz
	static constexpr float FIFO_SAMPLE_DT{1e6f / RATE};
	static constexpr hrt_abstime DATA_TIMEOUT_US{100000};
	static constexpr hrt_abstime RECOVERY_COOLDOWN_US{1000000};

	static constexpr int32_t FIFO_MAX_SAMPLES{math::min(FIFO::SIZE / sizeof(FIFO::DATA), sizeof(sensor_accel_fifo_s::x) / sizeof(sensor_accel_fifo_s::x[0]))};

	// Transfer data
	struct FIFOTransferBuffer {
		uint8_t cmd{static_cast<uint8_t>(Register::FIFO_DATA) | DIR_READ};
		uint8_t dummy{0};
		FIFO::DATA f[FIFO_MAX_SAMPLES] {};
	};
	// ensure no struct padding
	static_assert(sizeof(FIFOTransferBuffer) == (2 + FIFO_MAX_SAMPLES *sizeof(FIFO::DATA)));

	// Keep this buffer in the driver object rather than on the SPI work
	// queue stack: a whole FIFO-depth transaction is needed to guarantee that
	// no variable-length FIFO frame is read only partially.
	struct FIFOFlushBuffer {
		uint8_t cmd{static_cast<uint8_t>(Register::FIFO_DATA) | DIR_READ};
		uint8_t dummy{0};
		uint8_t data[FIFO::SIZE] {};
	};
	static_assert(sizeof(FIFOFlushBuffer) == (2 + FIFO::SIZE));

	struct register_config_t {
		Register reg;
		uint8_t set_bits{0};
		uint8_t clear_bits{0};
	};

	int probe() override;

	bool Configure();
	void ConfigureAccel();
	void ConfigureSampleRate(int sample_rate = 0);
	void ConfigureFIFOWatermark(uint8_t samples);

	static int DataReadyInterruptCallback(int irq, void *context, void *arg);
	void DataReady();
	bool DataReadyInterruptConfigure();
	bool DataReadyInterruptDisable();

	bool RegisterCheck(const register_config_t &reg_cfg);

	uint8_t RegisterRead(Register reg);
	void RegisterWrite(Register reg, uint8_t value);
	void RegisterSetAndClearBits(Register reg, uint8_t setbits, uint8_t clearbits);

	uint16_t FIFOReadCount();
	bool FIFORead(const hrt_abstime &timestamp_sample, uint8_t samples);
	bool FIFOFlush();
	void FIFOReset();

	void UpdateTemperature();

	PX4Accelerometer _px4_accel;

	perf_counter_t _bad_register_perf{perf_alloc(PC_COUNT, MODULE_NAME"_accel: bad register")};
	perf_counter_t _bad_transfer_perf{perf_alloc(PC_COUNT, MODULE_NAME"_accel: bad transfer")};
	perf_counter_t _fifo_empty_perf{perf_alloc(PC_COUNT, MODULE_NAME"_accel: FIFO empty")};
	perf_counter_t _fifo_overflow_perf{perf_alloc(PC_COUNT, MODULE_NAME"_accel: FIFO overflow")};
	perf_counter_t _fifo_reset_perf{perf_alloc(PC_COUNT, MODULE_NAME"_accel: FIFO reset")};
	perf_counter_t _recovery_perf{perf_alloc(PC_COUNT, MODULE_NAME"_accel: recovery")};
	perf_counter_t _drdy_missed_perf{nullptr};

	uint8_t _fifo_samples{static_cast<uint8_t>(_fifo_empty_interval_us / (1000000 / RATE))};
	FIFOFlushBuffer _fifo_flush_buffer{};

	bool _normal_mode_requested{false};
	hrt_abstime _last_success_timestamp{0};
	hrt_abstime _last_recovery_timestamp{0};
#if defined(CONFIG_BMI088_ACCELEROMETER_INT1) || defined(CONFIG_BMI088_ACCELEROMETER_INT2)
	static constexpr uint8_t size_register_cfg{10};
#else
	static constexpr uint8_t size_register_cfg{8};
#endif
	register_config_t _register_cfg[size_register_cfg] {
		// Register                        | Set bits, Clear bits
		{ Register::ACC_PWR_CONF,          0, ACC_PWR_CONF_BIT::acc_pwr_save },
		{ Register::ACC_PWR_CTRL,          ACC_PWR_CTRL_BIT::acc_enable, 0 },
		{ Register::ACC_CONF,              ACC_CONF_BIT::acc_bwp_Normal | ACC_CONF_BIT::acc_odr_1600, Bit1 | Bit0 },
		{ Register::ACC_RANGE,             ACC_RANGE_BIT::acc_range_24g, 0 },
		{ Register::FIFO_WTM_0,            0, 0 },
		{ Register::FIFO_WTM_1,            0, 0 },
		{ Register::FIFO_CONFIG_0,         FIFO_CONFIG_0_BIT::BIT1_ALWAYS | FIFO_CONFIG_0_BIT::FIFO_mode, 0 },
		{ Register::FIFO_CONFIG_1,         FIFO_CONFIG_1_BIT::BIT4_ALWAYS | FIFO_CONFIG_1_BIT::Acc_en, 0 },
# if defined(CONFIG_BMI088_ACCELEROMETER_INT1)
		{ Register::INT1_IO_CONF,          INT1_IO_CONF_BIT::int1_out, 0 },
		{ Register::INT1_INT2_MAP_DATA,    INT1_INT2_MAP_DATA_BIT::int1_fwm, 0},
# elif defined(CONFIG_BMI088_ACCELEROMETER_INT2)
		{ Register::INT2_IO_CONF,	   INT2_IO_CONF_BIT::int2_out, 0 },
		{ Register::INT1_INT2_MAP_DATA,	   INT1_INT2_MAP_DATA_BIT::int2_fwm, 0},
# endif
	};
};

} // namespace Bosch::BMI088::Accelerometer
