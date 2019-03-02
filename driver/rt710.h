// rt710.h

#ifndef __RT710_H__
#define __RT710_H__

#include <linux/types.h>
#include <linux/device.h>

#include "i2c_comm.h"

enum rt710_agc_mode {
	RT710_AGC_NEGATIVE = 0,
	RT710_AGC_POSITIVE,
};

enum rt710_vga_attenuate_mode{
	RT710_VGA_ATTEN_OFF = 0,
	RT710_VGA_ATTEN_ON,
};

enum rt710_fine_gain {
	RT710_FINE_GAIN_HIGH = 0,
	RT710_FINE_GAIN_LOW,
};

struct rt710_config {
	enum rt710_agc_mode agc_mode;
	enum rt710_vga_attenuate_mode vga_atten_mode;
	enum rt710_fine_gain fine_gain;
};

struct rt710_priv {
	u32 freq;
};

struct rt710_tuner {
	struct device *dev;
	struct i2c_comm_master *i2c;
	u8 i2c_addr;
	struct rt710_config config;
	struct rt710_priv priv;
};

int rt710_init(struct rt710_tuner *t);
int rt710_term(struct rt710_tuner *t);

int rt710_sleep(struct rt710_tuner *t);
int rt710_set_params(struct rt710_tuner *t, u32 freq, u32 symbol_rate, u32 rolloff);
int rt710_is_pll_locked(struct rt710_tuner *t, bool *locked);
int rt710_get_rf_gain(struct rt710_tuner *t, u8 *gain);
int rt710_get_rf_signal_strength(struct rt710_tuner *t, s32 *ss);

#endif
