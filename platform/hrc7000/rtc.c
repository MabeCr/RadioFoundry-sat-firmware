// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The HR_C7000's battery-backed clock (hal_rtc.h, CONFIG_SAT; docs/spec/hardware-dm32.md 3.3): an RTC on the SoC's
// internal I2C2 at address 0x70, a 32.768 kHz crystal and a coin cell. Every wait is bounded, so a clock that doesn't
// answer costs a few milliseconds and a false. Nothing here keeps state, and nothing resets the RTC: only a write
// of the time touches its registers
#include "config.h"

#if CONFIG_SAT
#include "platform.h"
#include "hal_rtc.h"
#include "hal_time.h"

#define I2C2_BASE	0x14080000u
#define I2C_CON		REG32(I2C2_BASE + 0x00)
#define I2C_TAR		REG32(I2C2_BASE + 0x04)
#define I2C_DATA_CMD	REG32(I2C2_BASE + 0x10)
#define I2C_FS_HCNT	REG32(I2C2_BASE + 0x1C)
#define I2C_FS_LCNT	REG32(I2C2_BASE + 0x20)
#define I2C_INTR_MASK	REG32(I2C2_BASE + 0x30)
#define I2C_RX_TL	REG32(I2C2_BASE + 0x38)
#define I2C_TX_TL	REG32(I2C2_BASE + 0x3C)
#define I2C_CLR_INTR	REG32(I2C2_BASE + 0x40)
#define I2C_ENABLE	REG32(I2C2_BASE + 0x6C)
#define I2C_STATUS	REG32(I2C2_BASE + 0x70)
#define I2C_EN_STATUS	REG32(I2C2_BASE + 0x9C)
#define I2C_START	REG32(I2C2_BASE + 0xA0)	// not a standard DesignWare register

#define RTC_ADDR	0x70u
#define RTC_SEC		0x00	// current seconds, minutes, hours, day (low, high)
#define RTC_LOAD_SEC	0x0C	// the same to load: seconds, minutes, hours, day low, day high
#define RTC_CCR		0x11	// bit 0 interrupt enable, bit 1 run, bit 2 load (clears itself)
#define RTC_VSTAT	0x12	// bit 0: a time was loaded
#define WAIT_US		5000u	// longest wait for the bus

static bool wait_status(uint32_t mask, uint32_t want)
{
	uint32_t t0 = hal_micros();
	while ((I2C_STATUS & mask) != want) {
		if (hal_micros() - t0 > WAIT_US)
			return false;
	}
	return true;
}

static bool wait_disabled(void)
{
	uint32_t t0 = hal_micros();
	while ((I2C_EN_STATUS & 1u) != 0) {
		if (hal_micros() - t0 > WAIT_US)
			return false;
	}
	return true;
}

static void open_bus(void)
{
	I2C_ENABLE = 0;
	(void)I2C_CLR_INTR;
	I2C_TAR = RTC_ADDR;
	I2C_ENABLE = 1;
}

static void setup(void)
{
	I2C_ENABLE = 0;
	I2C_CON = 0x65;
	I2C_INTR_MASK = 0x40;
	I2C_FS_HCNT = 0x61;
	I2C_FS_LCNT = 0x68;
	I2C_TX_TL = 6;
	I2C_RX_TL = 6;
}

static bool reg_write(uint8_t reg, uint8_t v)
{
	open_bus();
	I2C_DATA_CMD = reg;
	I2C_DATA_CMD = v;
	I2C_START = 1;
	bool ok = wait_status(0x05, 0x04);
	I2C_START = 0;
	I2C_ENABLE = 0;
	return wait_disabled() && ok;
}

static bool reg_read(uint8_t reg, uint8_t *v)
{
	for (unsigned i = 0; (I2C_STATUS & 0x08u) != 0 && i < 16u; i++)
		(void)I2C_DATA_CMD;	// drain the receive FIFO
	open_bus();
	I2C_DATA_CMD = reg;
	I2C_DATA_CMD = 0x100;	// read
	I2C_START = 1;
	bool ok = wait_status(0x08, 0x08);
	if (ok)
		*v = (uint8_t)I2C_DATA_CMD;
	I2C_START = 0;
	I2C_ENABLE = 0;
	return wait_disabled() && ok;
}

// The five time registers; the seconds a second time, so a rollover between the reads shows
static bool read_time(uint8_t r[5])
{
	for (unsigned tries = 0; tries < 3; tries++) {
		uint8_t again;
		for (uint8_t i = 0; i < 5; i++) {
			if (!reg_read((uint8_t)(RTC_SEC + i), &r[i]))
				return false;
		}
		if (!reg_read(RTC_SEC, &again))
			return false;
		if (again == r[0])
			return true;
	}
	return false;
}

bool hal_rtc_read(uint16_t *day, uint8_t *h, uint8_t *m, uint8_t *s)
{
	uint8_t v, r[5];
	setup();
	if (!reg_read(RTC_VSTAT, &v) || (v & 1u) == 0 || !read_time(r))
		return false;
	*s = r[0] & 0x3Fu;
	*m = r[1] & 0x3Fu;
	*h = r[2] & 0x1Fu;
	*day = (uint16_t)(r[3] | (uint16_t)r[4] << 8);
	return true;
}

bool hal_rtc_write(uint16_t day, uint8_t h, uint8_t m, uint8_t s)
{
	setup();
	// Valid flag off while the load is half done, the load registers, run + load, then valid on (the datasheet's order)
	return reg_write(RTC_VSTAT, 0) && reg_write(RTC_LOAD_SEC, s) && reg_write(RTC_LOAD_SEC + 1, m) &&
	       reg_write(RTC_LOAD_SEC + 2, h) && reg_write(RTC_LOAD_SEC + 3, (uint8_t)day) &&
	       reg_write(RTC_LOAD_SEC + 4, (uint8_t)(day >> 8)) && reg_write(RTC_CCR, 0x07) && reg_write(RTC_VSTAT, 1);
}
#endif
