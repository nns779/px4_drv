// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony CXD2856ER driver definitions (cxd2856er.h)
 *
 * Copyright (c) 2018-2021 nns779
 */

#ifndef __CXD2856ER_H__
#define __CXD2856ER_H__

#ifdef __linux__
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/device.h>
#elif defined(_WIN32) || defined(_WIN64)
#include "misc_win.h"
#endif

#include "i2c_comm.h"

struct cxd2856er_config {
	u32 xtal;
	bool tuner_i2c;
};

enum cxd2856er_state {
	CXD2856ER_UNKNOWN_STATE = 0,
	CXD2856ER_SLEEP_STATE,
	CXD2856ER_ACTIVE_STATE
};

enum cxd2856er_system {
	CXD2856ER_UNSPECIFIED_SYSTEM = 0,
	CXD2856ER_ISDB_T_SYSTEM,
	CXD2856ER_ISDB_S_SYSTEM
};

enum cxd2856er_i2c_target {
	CXD2856ER_I2C_SLVX = 0,
	CXD2856ER_I2C_SLVT
};

union cxd2856er_system_params {
	u32 bandwidth;
};

struct cxd2856er_demod {
	const struct device *dev;
	const struct i2c_comm_master *i2c;
	struct i2c_comm_master i2c_master;
	struct {
		u8 slvx;	// system
		u8 slvt;	// demod
	} i2c_addr;
	struct cxd2856er_config config;
	enum cxd2856er_state state;
	enum cxd2856er_system system;
};

#ifdef __cplusplus
extern "C" {
#endif
int cxd2856er_read_regs(struct cxd2856er_demod *demod,
			enum cxd2856er_i2c_target target,
			u8 reg, u8 *buf, int len);
int cxd2856er_write_regs(struct cxd2856er_demod *demod,
			 enum cxd2856er_i2c_target target,
			 u8 reg, u8 *buf, int len);
int cxd2856er_write_reg_mask(struct cxd2856er_demod *demod,
			     enum cxd2856er_i2c_target target,
			     u8 reg, u8 val, u8 mask);

static inline int cxd2856er_read_slvx_regs(struct cxd2856er_demod *demod,
					  u8 reg, u8 *buf, int len)
{
	return cxd2856er_read_regs(demod, CXD2856ER_I2C_SLVX, reg, buf, len);
}

static inline int cxd2856er_read_slvt_regs(struct cxd2856er_demod *demod,
					  u8 reg, u8 *buf, int len)
{
	return cxd2856er_read_regs(demod, CXD2856ER_I2C_SLVT, reg, buf, len);
}

static inline int cxd2856er_read_slvx_reg(struct cxd2856er_demod *demod,
					  u8 reg, u8 *val)
{
	return cxd2856er_read_regs(demod, CXD2856ER_I2C_SLVX, reg, val, 1);
}

static inline int cxd2856er_read_slvt_reg(struct cxd2856er_demod *demod,
					  u8 reg, u8 *val)
{
	return cxd2856er_read_regs(demod, CXD2856ER_I2C_SLVT, reg, val, 1);
}

static inline int cxd2856er_write_slvx_regs(struct cxd2856er_demod *demod,
					    u8 reg, u8 *buf, int len)
{
	return cxd2856er_write_regs(demod, CXD2856ER_I2C_SLVX, reg, buf, len);
}

static inline int cxd2856er_write_slvt_regs(struct cxd2856er_demod *demod,
					    u8 reg, u8 *buf, int len)
{
	return cxd2856er_write_regs(demod, CXD2856ER_I2C_SLVT, reg, buf, len);
}

static inline int cxd2856er_write_slvx_reg(struct cxd2856er_demod *demod,
					   u8 reg, u8 val)
{
	return cxd2856er_write_regs(demod, CXD2856ER_I2C_SLVX, reg, &val, 1);
}

static inline int cxd2856er_write_slvt_reg(struct cxd2856er_demod *demod,
					   u8 reg, u8 val)
{
	return cxd2856er_write_regs(demod, CXD2856ER_I2C_SLVT, reg, &val, 1);
}

static inline int cxd2856er_write_slvx_reg_mask(struct cxd2856er_demod *demod,
						u8 reg, u8 val, u8 mask)
{
	return cxd2856er_write_reg_mask(demod, CXD2856ER_I2C_SLVX,
					reg, val, mask);
}

static inline int cxd2856er_write_slvt_reg_mask(struct cxd2856er_demod *demod,
						u8 reg, u8 val, u8 mask)
{
	return cxd2856er_write_reg_mask(demod, CXD2856ER_I2C_SLVT,
					reg, val, mask);
}

int cxd2856er_init(struct cxd2856er_demod *demod);
int cxd2856er_term(struct cxd2856er_demod *demod);

int cxd2856er_sleep(struct cxd2856er_demod *demod);
int cxd2856er_wakeup(struct cxd2856er_demod *demod,
		     enum cxd2856er_system system,
		     union cxd2856er_system_params *params);
int cxd2856er_post_tune(struct cxd2856er_demod *demod);
int cxd2856er_set_slot_isdbs(struct cxd2856er_demod *demod, u16 idx);
int cxd2856er_set_tsid_isdbs(struct cxd2856er_demod *demod, u16 tsid);
int cxd2856er_is_ts_locked_isdbt(struct cxd2856er_demod *demod,
				 bool *locked, bool *unlocked);
int cxd2856er_is_ts_locked_isdbs(struct cxd2856er_demod *demod, bool *locked);
int cxd2856er_read_cnr_raw_isdbt(struct cxd2856er_demod *demod, u16 *value);
int cxd2856er_read_cnr_raw_isdbs(struct cxd2856er_demod *demod, u16 *value);
#ifdef __cplusplus
}
#endif

#endif
