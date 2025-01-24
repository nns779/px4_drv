// SPDX-License-Identifier: GPL-2.0-only
/*
 * RafaelMicro RT710 driver definitions (rt710.h)
 *
 * Copyright (c) 2018-2021 nns779
 */

#ifndef __RT710_H__
#define __RT710_H__

#ifdef __linux__
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/device.h>
#elif defined(_WIN32) || defined(_WIN64)
#include "misc_win.h"
#endif

#include "i2c_comm.h"

enum rt710_chip_type {
	RT710_CHIP_TYPE_UNKNOWN = 0,
	RT710_CHIP_TYPE_RT710,
	RT710_CHIP_TYPE_RT720,
};

enum rt710_signal_output_mode {
	RT710_SIGNAL_OUTPUT_SINGLE = 0,
	RT710_SIGNAL_OUTPUT_DIFFERENTIAL,
};

enum rt710_agc_mode {
	RT710_AGC_NEGATIVE = 0,
	RT710_AGC_POSITIVE,
};

enum rt710_vga_attenuate_mode {
	RT710_VGA_ATTEN_OFF = 0,
	RT710_VGA_ATTEN_ON,
};

enum rt710_fine_gain {
	RT710_FINE_GAIN_3DB = 0,
	RT710_FINE_GAIN_2DB,
	RT710_FINE_GAIN_1DB,
	RT710_FINE_GAIN_0DB,
};

enum rt710_scan_mode {
	RT710_SCAN_MANUAL = 0,
	RT710_SCAN_AUTO,
};

struct rt710_config {
	u32 xtal;
	bool loop_through;
	bool clock_out;
	enum rt710_signal_output_mode signal_output_mode;
	enum rt710_agc_mode agc_mode;
	enum rt710_vga_attenuate_mode vga_atten_mode;
	enum rt710_fine_gain fine_gain;
	enum rt710_scan_mode scan_mode;		// only for RT720
};

struct rt710_priv {
	struct mutex lock;
	bool init;
	enum rt710_chip_type chip;
	u32 freq;
};

struct rt710_tuner {
	const struct device *dev;
	const struct i2c_comm_master *i2c;
	u8 i2c_addr;
	struct rt710_config config;
	struct rt710_priv priv;
};

#ifdef __cplusplus
extern "C" {
#endif
int rt710_init(struct rt710_tuner *t);
int rt710_term(struct rt710_tuner *t);

int rt710_sleep(struct rt710_tuner *t);
int rt710_set_params(struct rt710_tuner *t, u32 freq, u32 symbol_rate, u32 rolloff);
int rt710_is_pll_locked(struct rt710_tuner *t, bool *locked);
int rt710_get_rf_gain(struct rt710_tuner *t, u8 *gain);
int rt710_get_rf_signal_strength(struct rt710_tuner *t, s32 *ss);
#ifdef __cplusplus
}
#endif

#endif
