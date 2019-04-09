// r850_lite.h

#ifndef __R850_LITE_H__
#define __R850_LITE_H__

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/device.h>

#include "i2c_comm.h"

#define R850_NUM_REGS	0x30

enum r850_system {
	R850_SYSTEM_STANDARD = 0,
	R850_SYSTEM_ISDB_T,
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
	bool is_cable_system;	// DVB-C, J38B
};

struct r850_priv {
	struct mutex lock;
	bool init;
	int chip;
	u8 xtal_pwr;
	u8 regs[R850_NUM_REGS];
};

struct r850_tuner {
	struct device *dev;
	struct i2c_comm_master *i2c;
	u8 i2c_addr;
	u32 xtal;
	struct r850_system_config system_config;
	struct r850_priv priv;
};

int r850_init(struct r850_tuner *t);
int r850_term(struct r850_tuner *t);

int r850_write_config_regs(struct r850_tuner *t, u8 *regs);
int r850_is_pll_locked(struct r850_tuner *t, bool *locked);

#endif
