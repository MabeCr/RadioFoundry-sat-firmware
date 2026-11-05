// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A battery-backed clock, where the board has one (CONFIG_SAT's clock, docs/spec/satellite.md 4.1): its time of day and
// a 16-bit day number, as the hardware keeps them. Core turns them into UTC (sat_clock_from_rtc). A board without the
// clock leaves both calls out (weak versions say "none"). Called from one task only: the satellite task
#ifndef HAL_RTC_H
#define HAL_RTC_H

#include <stdbool.h>
#include <stdint.h>

// The registers now; false when the board has no clock, it didn't answer, or it was never set
bool hal_rtc_read(uint16_t *day, uint8_t *h, uint8_t *m, uint8_t *s);
// Set it (and start it if it wasn't running); false when the board has none or it didn't answer
bool hal_rtc_write(uint16_t day, uint8_t h, uint8_t m, uint8_t s);

#endif
