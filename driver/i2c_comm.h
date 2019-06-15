// SPDX-License-Identifier: GPL-2.0-only
/*
 * Definitions for I2C communicators (i2c_comm.h)
 *
 * Copyright (c) 2018-2019 nns779
 */

#ifndef __I2C_COMM_H__
#define __I2C_COMM_H__

struct i2c_comm_master {
	int (*lock) (void *i2c_priv);
	int (*unlock) (void *i2c_priv);
	int (*wr) (void *i2c_priv, u8 addr, const u8 * data, int len);
	int (*rd) (void *i2c_priv, u8 addr, u8 *data, int len);
	void *priv;
};

static inline int i2c_comm_master_lock(struct i2c_comm_master *m)
{
	if (m && m->lock)
		return m->lock(m->priv);
	else
		return -EFAULT;
}

static inline int i2c_comm_master_unlock(struct i2c_comm_master *m)
{
	if (m && m->unlock)
		return m->unlock(m->priv);
	else
		return -EFAULT;
}

static inline int i2c_comm_master_write(struct i2c_comm_master *m, u8 addr, const u8 *data, int len)
{
	if (m && m->wr)
		return m->wr(m->priv, addr, data, len);
	else
		return -EFAULT;
}

static inline int i2c_comm_master_read(struct i2c_comm_master *m, u8 addr, u8 *data, int len)
{
	if (m && m->rd)
		return m->rd(m->priv, addr, data, len);
	else
		return -EFAULT;
}

#endif
