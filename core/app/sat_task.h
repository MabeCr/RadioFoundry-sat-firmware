// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The satellite service's task (CONFIG_SAT; core/sat/sat_svc.h, docs/spec/satellite.md 5.1)
#ifndef SAT_TASK_H
#define SAT_TASK_H

// After the settings, the APRS position sources and the UI are up
void sat_task_start(void);
// The clock was set by hand (the Set time screen): write it to the battery-backed clock, from the task. Any task
void sat_task_rtc_write(void);

#endif
