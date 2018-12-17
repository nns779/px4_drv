// rt710.h

#ifndef __RT710_H__
#define __RT710_H__

#include <linux/types.h>
#include <linux/device.h>

#include "i2c_comm.h"

typedef enum {
	RT710_AGC_NEGATIVE = 0,
	RT710_AGC_POSITIVE,
} rt710_agc_mode_t;

typedef enum {
	RT710_VGA_ATTEN_OFF = 0,
	RT710_VGA_ATTEN_ON,
} rt710_vga_attenuate_mode_t;

typedef enum {
	RT710_FINE_GAIN_HIGH = 0,
	RT710_FINE_GAIN_LOW,
} rt710_fine_gain_t;

struct rt710_config {
	rt710_agc_mode_t agc_mode;
	rt710_vga_attenuate_mode_t vga_atten_mode;
	rt710_fine_gain_t fine_gain;
};

struct rt710_tuner {
	struct device *dev;
	struct i2c_comm_master *i2c;
	u8 i2c_addr;
	struct rt710_config config;
};

int rt710_init(struct rt710_tuner *t);
int rt710_term(struct rt710_tuner *t);

int rt710_sleep(struct rt710_tuner *t);
int rt710_set_params(struct rt710_tuner *t, u32 freq, u32 symbol_rate, u32 rolloff);
int rt710_is_pll_locked(struct rt710_tuner *t, bool *locked);

#endif
