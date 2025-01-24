// SPDX-License-Identifier: GPL-2.0-only
/*
 * Definitions for I2C communicators (i2c_comm.h)
 *
 * Copyright (c) 2018-2021 nns779
 */

#ifndef __I2C_COMM_H__
#define __I2C_COMM_H__

#ifdef __linux__
#include <linux/types.h>
#elif defined(_WIN32) || defined(_WIN64)
#include "misc_win.h"
#endif

struct i2c_comm_request {
	enum i2c_request_type {
		I2C_UNDEFINED_REQUEST = 0,
		I2C_READ_REQUEST,
		I2C_WRITE_REQUEST
	} req;
	u16 addr;
	u8 *data;
	int len;
};

struct i2c_comm_master {
	int (*gate_ctrl)(void *i2c_priv, bool open);
	int (*request)(void *i2c_priv,
		       const struct i2c_comm_request *req,
		       int num);
	void *priv;
};

static inline int i2c_comm_master_gate_ctrl(const struct i2c_comm_master *m,
					    bool open)
{
	return ((m && m ->gate_ctrl) ? m->gate_ctrl(m->priv, open) : -EFAULT);
}

static inline int i2c_comm_master_request(const struct i2c_comm_master *m,
					  const struct i2c_comm_request *req,
					  int num)
{
	return ((m && m->request) ? m->request(m->priv, req, num) : -EFAULT);
}

#if 0
static inline int i2c_comm_master_read(const struct i2c_comm_master *m,
				       u8 addr, u8 *data, int len)
{
	struct i2c_comm_request req[1];

	req[0].req = I2C_READ_REQUEST;
	req[0].addr = addr;
	req[0].data = data;
	req[0].len = len;

	return i2c_comm_master_request(m, &req, 1);
}

static inline int i2c_comm_master_write(const struct i2c_comm_master *m,
					u8 addr, const u8 *data, int len)
{
	struct i2c_comm_request req[1];

	req[0].req = I2C_WRITE_REQUEST;
	req[0].addr = addr;
	req[0].data = data;
	req[0].len = len;

	return i2c_comm_master_request(m, &req, 1);
}
#endif

#endif
