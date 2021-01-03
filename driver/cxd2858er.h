// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony CXD2858ER driver definitions (cxd2858er.h)
 *
 * Copyright (c) 2018-2020 nns779
 */

#ifndef __CXD2858ER_H__
#define __CXD2858ER_H__

#include <linux/types.h>
#include <linux/device.h>

#include "i2c_comm.h"

struct cxd2858er_config {
	u32 xtal;
};

enum cxd2858er_system {
	CXD2858ER_UNSPECIFIED_SYSTEM = 0,
	CXD2858ER_ISDB_T_SYSTEM,
	CXD2858ER_ISDB_S_SYSTEM
};

struct cxd2858er_tuner {
	const struct device *dev;
	const struct i2c_comm_master *i2c;
	u8 i2c_addr;
	struct cxd2858er_config config;
	enum cxd2858er_system system;
};

int cxd2858er_init(struct cxd2858er_tuner *tuner);
int cxd2858er_term(struct cxd2858er_tuner *tuner);

int cxd2858er_set_params_t(struct cxd2858er_tuner *tuner,
			   enum cxd2858er_system system,
			   u32 freq, u32 bandwidth);
int cxd2858er_set_params_s(struct cxd2858er_tuner *tuner,
			   enum cxd2858er_system system,
			   u32 freq, u32 symbol_rate);
int cxd2858er_stop(struct cxd2858er_tuner *tuner);

#endif
