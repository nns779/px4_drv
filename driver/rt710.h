// rt710.h

#ifndef __RT710_H__
#define __RT710_H__

#include <linux/types.h>

#include "i2c_comm.h"

struct rt710_tuner {
	struct i2c_comm_master *i2c;
	u8 i2c_addr;
};

int rt710_init(struct rt710_tuner *t);
int rt710_sleep(struct rt710_tuner *t, bool sleep);
int rt710_set_params(struct rt710_tuner *t, u32 freq, u32 symbol_rate, u32 rolloff);
int rt710_is_pll_locked(struct rt710_tuner *t, bool *locked);

#endif
