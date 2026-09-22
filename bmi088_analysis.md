# BMI088 bring-up record — Aegis FC v1.0

## Accepted production baseline

The BMI088 is working reliably on SPI2 with the following configuration:

- SPI clock: **1 MHz**
- Start order: **gyro first, then accelerometer**
- Data path: **FIFO polling with SPI2 DMA**; DRDY interrupts are disabled
- Sensor rotation: `ROTATION_YAW_180` (`bmi088 ... -R 4`)

This makes the default sensor frame match the installed IMU orientation. Do not also set a board/sensor yaw rotation in QGroundControl. Clear/re-do accelerometer and level calibration after moving from the earlier `-R 6` configuration.

### Observed at 1 MHz

Repeated cold boots, USB reconnects, and extended soak ran normally. The final DMA test uses a 2048-byte SPI2 DMA buffer per direction:

- accelerometer raw FIFO: **1600 Hz** (`2` samples per transfer, `dt = 625 us`)
- gyroscope FIFO: about **1998 Hz**
- control allocator and rate control: **666.7 Hz**; DShot: about **662 Hz**
- no bad register or transfer events
- startup FIFO overflow/reset counters remained fixed during a six-minute DMA soak; one gyro FIFO-empty read caused no data gap or reset
- CPU idle: about **62.5%**; SPI2 work queue: about **12.2%** CPU

The previous PIO implementation used about 61% CPU in `wq:SPI2` with only about 16% CPU idle. DMA reduces SPI2 CPU load by about 80% while retaining the baseline sampling and control rates. This is the current flight-control baseline.

## SPI clock-margin experiments

| SPI clock | Result | Decision |
| --- | --- | --- |
| 1 MHz | Correct output rates; stable during soak and USB reconnects | Accepted |
| 2 MHz | Driver remains alive, but output stayed at about 659 Hz accel / 1340 Hz gyro; each FIFO had an extra overflow/reset | Rejected |
| 8 MHz | BMI088 disappeared and QGroundControl reported missing accel/gyro | Rejected |
| 10 MHz | Earlier probe could not reliably read chip IDs | Rejected |

`bad transfer: 0` only confirms that the MCU SPI transaction completed. It does not prove that the returned FIFO bytes were valid. The permanent rate reduction at 2 MHz and loss of sensors at 8 MHz are therefore treated as an SPI signal-integrity/margin issue, not a performance win.

## Rejected software optimization

Reducing polling to 400 Hz accel / 333 Hz gyro and reading larger FIFO batches lowered SPI2 CPU only from about 61% to 52%, but also reduced rate control and DShot from about 667 Hz to about 333 Hz. It is rejected. The accepted solution is normal polling cadence with DMA at 1 MHz.

## What made v1 work

1. Polling avoids the unsupported/unstable DRDY interrupt setup on this board.
2. Starting gyro before accelerometer produces a repeatable SPI bring-up sequence.
3. The accelerometer chip-select is on PC15. Firmware explicitly disables LSE so PC15 is usable as GPIO.
4. SPI2 DMA at 1 MHz uses the board's DMA1/DMAMUX SPI2 RX/TX mapping and was verified alongside DShot on hardware.
5. The driver has targeted BMI088 FIFO/recovery work from bring-up. Keep that work until it is separately cleaned up and regression-tested.

The original brown-out diagnosis was a **hypothesis**, not a proven root cause. Stable long-term operation at 1 MHz makes it insufficient as the primary explanation; the measured frequency dependence is stronger evidence of limited SPI electrical margin and/or chip-select state at reset.

## Revision V2 hardware checklist

- Do not route a BMI088 CS pin to PC15/OSC32_OUT. Use an ordinary GPIO so LSE does not need to be disabled.
- Add a defined reset/power-up state for **both** BMI088 CS nets. Reserve pull-resistor footprints near the sensors and choose their direction/value against the BMI088 datasheet and a scope capture; do not leave CS floating during MCU reset.
- Keep SPI2 traces short with a continuous ground reference. Provide footprint options for source series termination on SCK and MOSI, then validate values by measurement.
- Bring both DRDY nets to known, non-conflicting MCU EXTI-capable pins, with test pads. Only enable DRDY after validating the electrical mapping and interrupt timing.
- Add SPI and CS test pads plus a nearby ground point, so SCK/MOSI/MISO/CS can be checked with an oscilloscope or logic analyzer.
- Keep SPI DMA buffers/cache handling explicit when evolving the board. DMA reduces CPU load and jitter but cannot increase BMI088 physical ODR.

## Future firmware validation order

Change one variable per build and retain the 1 MHz configuration as the recovery point:

1. Qualify the 1 MHz DMA baseline with repeated cold boots, USB reconnects, and a longer soak.
2. If V2 hardware passes signal measurements, test clock increments while checking sensor rates and FIFO counters.
3. Add gyro DRDY first, then validate it independently before considering accel DRDY.

Pass criteria for every experiment: correct output rate, no growing `bad transfer`, `bad register`, FIFO overflow/reset counters, stable USB/QGroundControl, and repeated cold boots.
