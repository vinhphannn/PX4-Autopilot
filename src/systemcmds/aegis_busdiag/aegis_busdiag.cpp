// SPDX-License-Identifier: BSD-3-Clause
// Aegis bench diagnostics. No reset, OTP write, or write to unidentified I2C devices.
#include <px4_platform_common/px4_config.h>
#include <px4_platform_common/log.h>
#include <px4_platform_common/i2c_spi_buses.h>
#include <px4_platform_common/time.h>
#include <drivers/device/spi.h>
#include <drivers/drv_sensor.h>
#include <uORB/Subscription.hpp>
#include <uORB/topics/actuator_armed.h>
#include <board_config.h>
#include <stm32.h>
#include <hardware/stm32_i2c.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

namespace
{
bool bmi_running(uint16_t type, const char *name)
{
	BusCLIArguments cli{false, true};
	BusInstanceIterator it(name, cli, type);
	while (it.next()) { if (it.instance()) { return true; } }
	return false;
}

class RawSPI : public device::SPI
{
public:
	RawSPI(uint8_t type, uint32_t devid, spi_mode_e mode, unsigned hz, bool accel) :
		SPI(type, "aegis_busdiag", 2, devid, mode, hz), _accel(accel) {}
	int initialize() { return init(); }
	uint8_t read(uint8_t reg)
	{
		uint8_t tx[3] {static_cast<uint8_t>(reg | 0x80), 0, 0};
		uint8_t rx[3] {0xa5, 0xa5, 0xa5};
		unsigned n = _accel ? 3 : 2;
		int ret = transfer(tx, rx, n);
		printf("SPI R reg=%02x TX[%u]=%02x %02x %02x RX=%02x %02x %02x ret=%d value=%02x\n",
		       reg, n, tx[0], tx[1], tx[2], rx[0], rx[1], rx[2], ret, rx[n - 1]);
		_last_ret = ret;
		return rx[n - 1];
	}
	bool writecheck(uint8_t reg)
	{
		const uint8_t old = read(reg);
		if (_last_ret != PX4_OK) { return false; }
		const uint8_t changed = old ^ 1;
		write(reg, changed);
		const uint8_t test = read(reg);
		const bool accepted = _last_ret == PX4_OK && test == changed;
		write(reg, old);
		const uint8_t restored = read(reg);
		const bool restore_ok = _last_ret == PX4_OK && old == restored;
		printf("WRITE_VERIFY reg=%02x original=%02x wrote=%02x read=%02x accepted=%u restored=%02x restore_ok=%u\n",
		       reg, old, changed, test, unsigned(accepted), restored, unsigned(restore_ok));
		return accepted && restore_ok;
	}
private:
	void write(uint8_t reg, uint8_t value)
	{
		uint8_t tx[2] {reg, value};
		uint8_t rx[2] {0xa5, 0xa5};
		int ret = transfer(tx, rx, 2);
		printf("SPI W TX=%02x %02x RX=%02x %02x ret=%d\n", reg, value, rx[0], rx[1], ret);
		px4_usleep(1000);
	}
	const bool _accel;
	int _last_ret{PX4_ERROR};
};

int spi_diag(bool accel, unsigned hz, unsigned mode, bool writecheck)
{
	if (bmi_running(DRV_ACC_DEVTYPE_BMI088, "bmi088_accel") || bmi_running(DRV_GYR_DEVTYPE_BMI088, "bmi088_gyro")) {
		PX4_ERR("stop both BMI088 drivers first: bmi088 -A stop; bmi088 -G stop");
		return PX4_ERROR;
	}
	const uint16_t type = accel ? DRV_ACC_DEVTYPE_BMI088 : DRV_GYR_DEVTYPE_BMI088;
	SPIBusIterator bus(SPIBusIterator::FilterType::InternalBus, type, -1, 2);
	if (!bus.next()) { PX4_ERR("SPI2 device absent in target table"); return PX4_ERROR; }
	printf("SPI2 %s CS=%s devid=%08" PRIx32 " mode=%u hz=%u dummy=%u\n",
	       accel ? "ACC" : "GYR", accel ? "PC15" : "PB12", bus.devid(), mode, hz, unsigned(accel));
	RawSPI dev(type, bus.devid(), static_cast<spi_mode_e>(mode), hz, accel);
	const int init = dev.initialize();
	printf("SPI bus init ret=%d; transfer ret=0 is NOT a slave ACK\n", init);
	if (init != PX4_OK) { return init; }
	// First accel read only latches SPI mode; leave a settling interval.
	if (accel) { dev.read(0x00); px4_usleep(5000); }
	bool identified = true;
	for (unsigned i = 0; i < 5; ++i) {
		identified = (dev.read(0x00) == (accel ? 0x1e : 0x0f)) && identified;
		px4_usleep(1000);
	}
	if (accel) {
		const uint8_t regs[] {0x02, 0x03, 0x40, 0x41, 0x48, 0x49, 0x7c, 0x7d};
		for (uint8_t reg : regs) { dev.read(reg); }
	} else {
		const uint8_t regs[] {0x0a, 0x0f, 0x10, 0x11, 0x12, 0x3e};
		for (uint8_t reg : regs) { dev.read(reg); }
	}
	printf("IDENTIFIED=%u expected=%02x\n", unsigned(identified), accel ? 0x1e : 0x0f);
	if (writecheck) {
		if (!identified) { PX4_ERR("writecheck skipped: chip ID not confirmed"); return PX4_ERROR; }
		// Only defined range bits, both settings valid, then restore original.
		return dev.writecheck(accel ? 0x41 : 0x0f) ? PX4_OK : PX4_ERROR;
	}
	return identified ? PX4_OK : PX4_ERROR;
}

void i2c_state(const char *stage)
{
	printf("I2C2 %s SDA=%u SCL=%u CR1=%08" PRIx32 " CR2=%08" PRIx32 " ISR=%08" PRIx32 " TIMINGR=%08" PRIx32 "\n",
	       stage, unsigned(px4_arch_gpioread(GPIO_PORTB | GPIO_PIN11)),
	       unsigned(px4_arch_gpioread(GPIO_PORTB | GPIO_PIN10)),
	       getreg32(STM32_I2C2_CR1), getreg32(STM32_I2C2_CR2), getreg32(STM32_I2C2_ISR), getreg32(STM32_I2C2_TIMINGR));
}

int i2c_diag(unsigned hz, bool reset)
{
	auto *dev = px4_i2cbus_initialize(2);
	if (!dev) { PX4_ERR("I2C2 init failed"); return PX4_ERROR; }
	i2c_state("before");
	if (reset) {
#if defined(CONFIG_I2C_RESET)
		printf("I2C2 explicit bus reset ret=%d\n", I2C_RESET(dev));
#else
		printf("I2C_RESET not compiled\n");
#endif
	}
	const uint8_t addresses[] {0x6b, 0x6c, 0x6d, 0x76, 0x77};
	const uint8_t registers[] {0x00, 0x0d, 0xd0};
	for (uint8_t addr : addresses) {
		for (uint8_t reg : registers) {
			for (unsigned attempt = 0; attempt < 2; ++attempt) {
				uint8_t rx[4] {0xa5, 0xa5, 0xa5, 0xa5};
				i2c_msg_s msgs[2] {};
				msgs[0].frequency = msgs[1].frequency = hz;
				msgs[0].addr = msgs[1].addr = addr;
				msgs[0].buffer = &reg;
				msgs[0].length = 1;
				msgs[1].flags = I2C_M_READ;
				msgs[1].buffer = rx;
				msgs[1].length = sizeof(rx);
				const int ret = I2C_TRANSFER(dev, msgs, 2);
				printf("I2C2 addr7=%02x wire=%02x/%02x hz=%u reg=%02x attempt=%u ret=%d RX=%02x %02x %02x %02x\n",
				       addr, addr << 1, (addr << 1) | 1, hz, reg, attempt, ret, rx[0], rx[1], rx[2], rx[3]);
				px4_usleep(1000);
			}
		}
		i2c_state("after-address");
	}
	px4_i2cbus_uninitialize(dev);
	return PX4_OK;
}

// Independent GPIO paths exclude STM32 I2C/SPI peripheral state and DMA.
constexpr uint32_t scl_gpio = GPIO_I2C2_SCL_GPIO;
constexpr uint32_t sda_gpio = GPIO_I2C2_SDA_GPIO;

class SoftI2C
{
public:
 bool high_clock() {
  px4_arch_gpiowrite(scl_gpio, true);
  for (unsigned i = 0; i < 200; ++i) {
   if (px4_arch_gpioread(scl_gpio)) { px4_usleep(50); return true; }
   px4_usleep(10);
  }
  return false;
 }
 bool start() {
  px4_arch_gpiowrite(sda_gpio, true); px4_usleep(50);
  if (!high_clock() || !px4_arch_gpioread(sda_gpio)) { return false; }
  px4_arch_gpiowrite(sda_gpio, false); px4_usleep(50);
  px4_arch_gpiowrite(scl_gpio, false); return true;
 }
 void stop() {
  px4_arch_gpiowrite(scl_gpio, false); px4_arch_gpiowrite(sda_gpio, false); px4_usleep(50);
  high_clock(); px4_arch_gpiowrite(sda_gpio, true); px4_usleep(50);
 }
 bool send(uint8_t value) {
  for (unsigned bit = 0; bit < 8; ++bit) {
   px4_arch_gpiowrite(sda_gpio, (value & 0x80) != 0); value <<= 1; px4_usleep(50);
   if (!high_clock()) { return false; }
   px4_arch_gpiowrite(scl_gpio, false);
  }
  px4_arch_gpiowrite(sda_gpio, true); px4_usleep(50);
  if (!high_clock()) { return false; }
  bool ack = !px4_arch_gpioread(sda_gpio);
  px4_arch_gpiowrite(scl_gpio, false); px4_usleep(50); return ack;
 }
 bool receive(uint8_t &value, bool ack) {
  value = 0; px4_arch_gpiowrite(sda_gpio, true);
  for (unsigned bit = 0; bit < 8; ++bit) {
   px4_usleep(50); if (!high_clock()) { return false; }
   value = (value << 1) | unsigned(px4_arch_gpioread(sda_gpio));
   px4_arch_gpiowrite(scl_gpio, false);
  }
  px4_arch_gpiowrite(sda_gpio, !ack); px4_usleep(50);
  bool ok = high_clock(); px4_arch_gpiowrite(scl_gpio, false);
  px4_arch_gpiowrite(sda_gpio, true); px4_usleep(50); return ok;
 }
};

int soft_i2c_diag()
{
 // No barometer driver may own this bus during GPIO takeover.
 BusCLIArguments cli{true, false};
 BusInstanceIterator active("bmp280", cli, DRV_BARO_DEVTYPE_BMP280);
 while (active.next()) { if (active.instance()) { PX4_ERR("stop bmp280 first"); return PX4_ERROR; } }
 auto *dev = px4_i2cbus_initialize(2);
 if (!dev) { return PX4_ERROR; }
 px4_i2cbus_uninitialize(dev);
 px4_arch_configgpio(scl_gpio); px4_arch_configgpio(sda_gpio);
 SoftI2C bus;
 const uint8_t addresses[] {0x6b, 0x6c, 0x6d, 0x76, 0x77};
 const uint8_t registers[] {0x00, 0x01, 0xd0};
 for (uint8_t addr : addresses) {
  for (uint8_t reg : registers) {
   bool st = bus.start(); bool aw = st && bus.send(addr << 1);
   bool ar = aw && bus.send(reg); bool sr = ar && bus.start();
   bool ra = sr && bus.send((addr << 1) | 1);
   uint8_t rx[2] {0xa5, 0xa5};
   bool ok = ra && bus.receive(rx[0], true) && bus.receive(rx[1], false);
   bus.stop();
   printf("GPIO-I2C addr7=%02x reg=%02x start=%u addrW_ACK=%u reg_ACK=%u repeatedStart=%u addrR_ACK=%u ok=%u RX=%02x %02x SDA=%u SCL=%u\n",
          addr, reg, unsigned(st), unsigned(aw), unsigned(ar), unsigned(sr), unsigned(ra), unsigned(ok),
          rx[0], rx[1], unsigned(px4_arch_gpioread(sda_gpio)), unsigned(px4_arch_gpioread(scl_gpio)));
  }
 }
 // Restore AF configuration; future normal drivers initialize the peripheral.
 px4_arch_configgpio(GPIO_I2C2_SCL); px4_arch_configgpio(GPIO_I2C2_SDA);
 return PX4_OK;
}

int soft_spi_diag()
{
 if (bmi_running(DRV_ACC_DEVTYPE_BMI088, "bmi088_accel") || bmi_running(DRV_GYR_DEVTYPE_BMI088, "bmi088_gyro")) {
  PX4_ERR("stop both BMI088 drivers first"); return PX4_ERROR;
 }
 constexpr uint32_t sck = GPIO_OUTPUT | GPIO_PUSHPULL | GPIO_SPEED_2MHz | GPIO_OUTPUT_SET | GPIO_PORTB | GPIO_PIN13;
 constexpr uint32_t mosi = GPIO_OUTPUT | GPIO_PUSHPULL | GPIO_SPEED_2MHz | GPIO_OUTPUT_CLEAR | GPIO_PORTC | GPIO_PIN3;
 constexpr uint32_t miso = GPIO_INPUT | GPIO_PULLUP | GPIO_PORTB | GPIO_PIN14;
 constexpr uint32_t cs_g = GPIO_PORTB | GPIO_PIN12;
 constexpr uint32_t cs_a = GPIO_PORTC | GPIO_PIN15;
 px4_arch_gpiowrite(cs_g, true); px4_arch_gpiowrite(cs_a, true);
 px4_arch_configgpio(sck); px4_arch_configgpio(mosi); px4_arch_configgpio(miso);
 for (unsigned test = 0; test < 5; ++test) {
  uint8_t tx[2] {0x80, 0}; uint8_t rx[2] {};
  px4_arch_gpiowrite(cs_g, false); px4_usleep(50);
  bool asserted = !px4_arch_gpioread(cs_g);
  bool other_high = px4_arch_gpioread(cs_a);
  for (unsigned b = 0; b < 2; ++b) {
   for (unsigned bit = 0; bit < 8; ++bit) {
    px4_arch_gpiowrite(sck, false);
    px4_arch_gpiowrite(mosi, (tx[b] & (0x80 >> bit)) != 0); px4_usleep(50);
    px4_arch_gpiowrite(sck, true); px4_usleep(50);
    rx[b] = (rx[b] << 1) | unsigned(px4_arch_gpioread(miso));
   }
  }
  px4_arch_gpiowrite(cs_g, true); px4_usleep(50);
  printf("GPIO-SPI mode3 GYR tx=80 00 rx=%02x %02x CS_G_low=%u CS_A_high=%u CS_G_release=%u expected=0f\n",
         rx[0], rx[1], unsigned(asserted), unsigned(other_high), unsigned(px4_arch_gpioread(cs_g)));
 }
 px4_arch_configgpio(GPIO_SPI2_SCK); px4_arch_configgpio(GPIO_SPI2_MOSI); px4_arch_configgpio(GPIO_SPI2_MISO);
 return PX4_OK;
}
}

extern "C" __EXPORT int aegis_busdiag_main(int argc, char *argv[])
{
	uORB::Subscription armed_sub{ORB_ID(actuator_armed)};
	actuator_armed_s armed{};
	if (!armed_sub.copy(&armed) || armed.armed || armed.prearmed) {
		PX4_ERR("requires a disarmed, non-prearmed bench vehicle"); return PX4_ERROR;
	}
	if (argc < 2) {
		printf("aegis_busdiag accel|gyro [kHz=1000] [mode=3] [write]\n"
		       "aegis_busdiag i2c|i2c-reset [kHz=100]\n"
		       "aegis_busdiag i2c-gpio|gyro-gpio\n");
		return PX4_ERROR;
	}
	if (!strcmp(argv[1], "i2c-gpio")) { return soft_i2c_diag(); }
	if (!strcmp(argv[1], "gyro-gpio")) { return soft_spi_diag(); }
	if (!strcmp(argv[1], "i2c") || !strcmp(argv[1], "i2c-reset")) {
		unsigned khz = argc > 2 ? strtoul(argv[2], nullptr, 0) : 100;
		if (khz < 10 || khz > 400) { return PX4_ERROR; }
		return i2c_diag(khz * 1000, !strcmp(argv[1], "i2c-reset"));
	}
	if (strcmp(argv[1], "accel") && strcmp(argv[1], "gyro")) { return PX4_ERROR; }
	unsigned khz = argc > 2 ? strtoul(argv[2], nullptr, 0) : 1000;
	unsigned mode = argc > 3 ? strtoul(argv[3], nullptr, 0) : 3;
	if (khz < 50 || khz > 10000 || (mode != 0 && mode != 3)) { return PX4_ERROR; }
	return spi_diag(!strcmp(argv[1], "accel"), khz * 1000, mode, argc > 4 && !strcmp(argv[4], "write"));
}
