// SPDX-License-Identifier: GPL-2.0-only
/*
 * RafaelMicro R850 driver definitions (r850.h)
 *
 * Copyright (c) 2018-2021 nns779
 */

#ifndef __R850_H__
#define __R850_H__

#ifdef __linux__
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/device.h>
#elif defined(_WIN32) || defined(_WIN64)
#include "misc_win.h"
#endif

#include "i2c_comm.h"

#define R850_NUM_REGS	0x30

struct r850_config {
	u32 xtal;
	bool loop_through;
	bool clock_out;
	bool no_imr_calibration;
	bool no_lpf_calibration;
};

enum r850_system {
	R850_SYSTEM_UNDEFINED = 0,
	R850_SYSTEM_DVB_T,
	R850_SYSTEM_DVB_T2,
	R850_SYSTEM_DVB_T2_1,
	R850_SYSTEM_DVB_C,
	R850_SYSTEM_J83B,
	R850_SYSTEM_ISDB_T,
	R850_SYSTEM_DTMB,
	R850_SYSTEM_ATSC,
	R850_SYSTEM_FM,
};

enum r850_bandwidth{
	R850_BANDWIDTH_6M = 0,
	R850_BANDWIDTH_7M,
	R850_BANDWIDTH_8M,
};

struct r850_system_config {
	enum r850_system system;
	enum r850_bandwidth bandwidth;
	u32 if_freq;
};

struct r850_imr {
	u8 gain;	// x
	u8 phase;	// y
	u8 iqcap;
	u8 value;
};

struct r850_priv {
	struct mutex lock;
	bool init;
	int chip;
	u8 xtal_pwr;
	u8 regs[R850_NUM_REGS];
	bool sleep;
	struct r850_system_config sys;
	u8 mixer_mode;
	u8 mixer_amp_lpf_imr_cal;
	struct {
		struct r850_imr imr[5];
		bool done;
		bool result[5];
		u8 mixer_amp_lpf;
	} imr_cal[2];
	struct r850_system_config sys_curr;
};

struct r850_tuner {
	const struct device *dev;
	const struct i2c_comm_master *i2c;
	u8 i2c_addr;
	struct r850_config config;
	struct r850_priv priv;
};

#ifdef __cplusplus
extern "C" {
#endif
int r850_init(struct r850_tuner *t);
int r850_term(struct r850_tuner *t);

int r850_sleep(struct r850_tuner *t);
int r850_wakeup(struct r850_tuner *t);
int r850_set_system(struct r850_tuner *t,
		    struct r850_system_config *system);
int r850_set_frequency(struct r850_tuner *t, u32 freq);
int r850_is_pll_locked(struct r850_tuner *t, bool *locked);
#ifdef __cplusplus
}
#endif

#endif
