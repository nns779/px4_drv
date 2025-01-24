// SPDX-License-Identifier: GPL-2.0-only
/*
 * Toshiba TC90522 driver (tc90522.c)
 *
 * Copyright (c) 2018-2021 nns779
 */

#include "print_format.h"
#include "tc90522.h"

#ifdef __linux__
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#endif

static int tc90522_read_regs_nolock(struct tc90522_demod *demod,
				    u8 reg,
				    u8 *buf, u8 len)
{
	int ret = 0;
	u8 b;
	struct i2c_comm_request req[2];

	if (!buf || !len)
		return -EINVAL;

	b = reg;

	req[0].req = I2C_WRITE_REQUEST;
	req[0].addr = demod->i2c_addr;
	req[0].data = &b;
	req[0].len = 1;

	req[1].req = I2C_READ_REQUEST;
	req[1].addr = demod->i2c_addr;
	req[1].data = buf;
	req[1].len = len;

	ret = i2c_comm_master_request(demod->i2c, req, 2);
	if (ret)
		dev_err(demod->dev,
			"tc90522_read_regs_nolock: i2c_comm_master_request() failed. (addr: 0x%x, reg: 0x%x, len: %u)\n",
			demod->i2c_addr, reg, len);

	return ret;
}

static int tc90522_read_reg_nolock(struct tc90522_demod *demod, u8 reg, u8 *val)
{
	return tc90522_read_regs_nolock(demod, reg, val, 1);
}

int tc90522_read_regs(struct tc90522_demod *demod, u8 reg, u8 *buf, u8 len)
{
	int ret = 0;

	mutex_lock(&demod->priv.lock);

	ret = tc90522_read_regs_nolock(demod, reg, buf, len);

	mutex_unlock(&demod->priv.lock);

	return ret;
}

int tc90522_read_reg(struct tc90522_demod *demod, u8 reg, u8 *val)
{
	int ret = 0;

	mutex_lock(&demod->priv.lock);

	ret = tc90522_read_regs_nolock(demod, reg, val, 1);

	mutex_unlock(&demod->priv.lock);

	return ret;
}

int tc90522_read_multiple_regs(struct tc90522_demod *demod,
			       struct tc90522_regbuf *regbuf, int num)
{
	int ret = 0, i;

	if (!regbuf || !num)
		return -EINVAL;

	mutex_lock(&demod->priv.lock);

	for (i = 0; i < num; i++) {
		ret = tc90522_read_regs_nolock(demod,
					       regbuf[i].reg,
					       regbuf[i].buf, regbuf[i].u.len);
		if (ret)
			break;
	}

	mutex_unlock(&demod->priv.lock);

	return ret;
}

static int tc90522_write_regs_nolock(struct tc90522_demod *demod,
				     u8 reg,
				     u8 *buf, u8 len)
{
	int ret = 0;
	u8 b[255];
	struct i2c_comm_request req[1];

	if (!buf || !len) {
		return -EINVAL;
	} else if (len > 254) {
		dev_dbg(demod->dev,
			"tc90522_write_regs_nolock: Buffer too large. (addr: 0x%x, reg: %u, len: 0x%x)\n",
			demod->i2c_addr, reg, len);
		return -EINVAL;
	}

	b[0] = reg;
	memcpy(&b[1], buf, len);

	req[0].req = I2C_WRITE_REQUEST;
	req[0].addr = demod->i2c_addr;
	req[0].data = b;
	req[0].len = 1 + len;

	ret = i2c_comm_master_request(demod->i2c, req, 1);
	if (ret)
		dev_err(demod->dev,
			"tc90522_write_regs_nolock: i2c_comm_master_request() failed. (addr: 0x%x, reg: 0x%x, len: %u, ret: %d)\n",
			demod->i2c_addr, reg, len, ret);

	return ret;
}

#if 0
static int tc90522_write_reg_nolock(struct tc90522_demod *demod, u8 reg, u8 val)
{
	return tc90522_write_regs_nolock(demod, reg, &val, 1);
}
#endif

int tc90522_write_regs(struct tc90522_demod *demod, u8 reg, u8 *buf, u8 len)
{
	int ret = 0;

	mutex_lock(&demod->priv.lock);

	ret = tc90522_write_regs_nolock(demod, reg, buf, len);

	mutex_unlock(&demod->priv.lock);

	return ret;
}

int tc90522_write_reg(struct tc90522_demod *demod, u8 reg, u8 val)
{
	int ret = 0;

	mutex_lock(&demod->priv.lock);

	ret = tc90522_write_regs_nolock(demod, reg, &val, 1);

	mutex_unlock(&demod->priv.lock);

	return ret;
}

int tc90522_write_multiple_regs(struct tc90522_demod *demod,
				struct tc90522_regbuf *regbuf, int num)
{
	int ret = 0, i;

	if (!regbuf || !num)
		return -EINVAL;

	mutex_lock(&demod->priv.lock);

	for (i = 0; i < num; i++) {
		if (regbuf[i].buf)
			ret = tc90522_write_regs_nolock(demod,
							regbuf[i].reg,
							regbuf[i].buf,
							regbuf[i].u.len);
		else
			ret = tc90522_write_regs_nolock(demod,
							regbuf[i].reg,
							&regbuf[i].u.val,
							1);

		if (ret)
			break;
	}

	mutex_unlock(&demod->priv.lock);

	return ret;
}

static int tc90522_i2c_master_request(void *i2c_priv,
				      const struct i2c_comm_request *req,
				      int num)
{
	int ret = 0, i, master_req_num = 0, n = 0;
	struct tc90522_demod *demod = i2c_priv;
	struct i2c_comm_request *master_req = NULL;

	mutex_lock(&demod->priv.lock);

	for (i = 0; i < num; i++) {
		switch (req[i].req) {
		case I2C_READ_REQUEST:
			if (!req[i].data || !req[i].len) {
				dev_dbg(demod->dev,
					"tc90522_i2c_master_request: Invalid parameter. (i: %d)\n",
					i);
				ret = -EINVAL;
				break;
			}

			master_req_num += 2;
			break;

		case I2C_WRITE_REQUEST:
			if (!req[i].data || !req[i].len || req[i].len > 253) {
				dev_dbg(demod->dev,
					"tc90522_i2c_master_request: Invalid parameter. (i: %d)\n",
					i);
				ret = -EINVAL;
				break;
			}

			master_req_num++;
			break;

		default:
			ret = -EINVAL;
			break;
		}
	}

	if (ret)
		goto exit;

	if (!master_req_num)
		goto exit;

	if ((num == 1 && req[0].req == I2C_WRITE_REQUEST) ||
	    (num == 2 && req[0].req == I2C_WRITE_REQUEST &&
	     req[1].req == I2C_READ_REQUEST)) {
		u8 b[255], br[2];
		struct i2c_comm_request master_req[3];

		b[0] = 0xfe;
		b[1] = (req[0].addr << 1);
		memcpy(&b[2], req[0].data, req[0].len);

		master_req[0].req = I2C_WRITE_REQUEST;
		master_req[0].addr = demod->i2c_addr;
		master_req[0].data = b;
		master_req[0].len = 2 + req[0].len;

		if (num == 2) {
			br[0] = 0xfe;
			br[1] = (req[1].addr << 1) | 0x01;

			master_req[1].req = I2C_WRITE_REQUEST;
			master_req[1].addr = demod->i2c_addr;
			master_req[1].data = br;
			master_req[1].len = 2;

			master_req[2].req = I2C_READ_REQUEST;
			master_req[2].addr = demod->i2c_addr;
			master_req[2].data = req[1].data;
			master_req[2].len = req[1].len;
		}

		ret = i2c_comm_master_request(demod->i2c,
					      master_req, (num == 2) ? 3 : 1);
		goto exit;
	}

	master_req = (struct i2c_comm_request *)kmalloc(sizeof(*master_req) * master_req_num,
							GFP_KERNEL);
	if (!master_req) {
		ret = -ENOMEM;
		goto exit;
	}

	for (i = 0; i < num; i++) {
		u8 *b;

		switch (req[i].req) {
		case I2C_READ_REQUEST:
			b = (u8 *)kmalloc(sizeof(*b) * 2, GFP_KERNEL);
			if (!b) {
				ret = -ENOMEM;
				break;
			}

			b[0] = 0xfe;
			b[1] = (req[i].addr << 1) | 0x01;

			master_req[n].req = I2C_WRITE_REQUEST;
			master_req[n].addr = demod->i2c_addr;
			master_req[n].data = b;
			master_req[n].len = 2;

			master_req[n + 1].req = I2C_READ_REQUEST;
			master_req[n + 1].addr = demod->i2c_addr;
			master_req[n + 1].data = req[i].data;
			master_req[n + 1].len = req[i].len;

			n += 2;
			break;

		case I2C_WRITE_REQUEST:
			b = (u8 *)kmalloc(sizeof(*b) * (2 + req[i].len),
					  GFP_KERNEL);
			if (!b) {
				ret = -ENOMEM;
				break;
			}

			b[0] = 0xfe;
			b[1] = (req[i].addr << 1);
			memcpy(&b[2], req[i].data, req[i].len);

			master_req[n].req = I2C_WRITE_REQUEST;
			master_req[n].addr = demod->i2c_addr;
			master_req[n].data = b;
			master_req[n].len = 2 + req[i].len;

			n++;
			break;

		default:
			ret = -EINVAL;
			break;
		}

		if (ret)
			goto exit_with_free;
	}

	ret = i2c_comm_master_request(demod->i2c, master_req, master_req_num);

exit_with_free:
	for (i = 0; i < master_req_num; i++) {
		if (master_req[i].req == I2C_WRITE_REQUEST &&
		    master_req[i].data)
			kfree(master_req[i].data);
	}

	if (master_req)
		kfree(master_req);

exit:
	mutex_unlock(&demod->priv.lock);

	return ret;
}

int tc90522_init(struct tc90522_demod *demod)
{
	mutex_init(&demod->priv.lock);

	demod->i2c_master.gate_ctrl = NULL;
	demod->i2c_master.request = tc90522_i2c_master_request;
	demod->i2c_master.priv = demod;

	return 0;
}

int tc90522_term(struct tc90522_demod *demod)
{
	demod->i2c_master.gate_ctrl = NULL;
	demod->i2c_master.request = NULL;
	demod->i2c_master.priv = NULL;

	mutex_destroy(&demod->priv.lock);

	return 0;
}

int tc90522_sleep_s(struct tc90522_demod *demod, bool sleep)
{
#if 1
	struct tc90522_regbuf regbuf[2] = {
		{ 0x13, NULL, { 0x00 } },
		{ 0x17, NULL, { 0x00 } }
	};

	if (sleep) {
		/* sleep */
		regbuf[0].u.val = 0x80;
		regbuf[1].u.val = 0xff;
	}

	return tc90522_write_multiple_regs(demod, regbuf, 2);
#else
	return tc90522_write_reg(demod, 0x17, (sleep) ? 0x01 : 0x00);
#endif
}

int tc90522_set_agc_s(struct tc90522_demod *demod, bool on)
{
	struct tc90522_regbuf regbuf[] = {
		{ 0x0a, NULL, { 0x00 } },
		{ 0x10, NULL, { 0xb0 } },
		{ 0x11, NULL, { 0x02 } },
		{ 0x03, NULL, { 0x01 } }
	};

	if (demod->is_secondary)
		regbuf[1].u.val = 0x30;

	if (on) {
		/* on */
		regbuf[0].u.val = 0xff;
		regbuf[1].u.val |= 0x02;
		regbuf[2].u.val = 0x00;
	}

	return tc90522_write_multiple_regs(demod, regbuf, 4);
}

int tc90522_tmcc_get_tsid_s(struct tc90522_demod *demod, u8 idx, u16 *tsid)
{
	int ret = 0;
	u8 b[2];

	if (idx >= 12)
		return -EINVAL;

	ret = tc90522_read_regs(demod, 0xce + (idx * 2), &b[0], 2);
	if (!ret)
		*tsid = (b[0] << 8 | b[1]);

	return ret;
}

int tc90522_get_tsid_s(struct tc90522_demod *demod, u16 *tsid)
{
	int ret = 0;
	u8 b[2];

	ret = tc90522_read_regs(demod, 0xe6, &b[0], 2);
	if (!ret)
		*tsid = (b[0] << 8 | b[1]);

	return ret;
}

int tc90522_set_tsid_s(struct tc90522_demod *demod, u16 tsid)
{
	u8 b[2];

	b[0] = ((tsid >> 8) & 0xff);
	b[1] = (tsid & 0xff);

	return tc90522_write_regs(demod, 0x8f, b, 2);
}

int tc90522_get_cn_s(struct tc90522_demod *demod, u16 *cn)
{
	int ret = 0;
	u8 b[2];

	ret = tc90522_read_regs(demod, 0xbc, &b[0], 2);
	if (!ret)
		*cn = (b[0] << 8) | b[1];

	return ret;
}

int tc90522_enable_ts_pins_s(struct tc90522_demod *demod, bool e)
{
	struct tc90522_regbuf regbuf[] = {
		{ 0x1c, NULL, { 0x00 } },
		{ 0x1f, NULL, { 0x00 } }
	};

	if (!e) {
		regbuf[0].u.val = 0x80;
		regbuf[1].u.val = 0x22;
	}

	return tc90522_write_multiple_regs(demod, regbuf, 2);
}

int tc90522_is_signal_locked_s(struct tc90522_demod *demod, bool *lock)
{
	int ret = 0;
	u8 b;

	*lock = false;

	ret = tc90522_read_reg(demod, 0xc3, &b);
	if (!ret && !(b & 0x10))
		*lock = true;

	return ret;
}

int tc90522_sleep_t(struct tc90522_demod *demod, bool sleep)
{
#if 1
	return tc90522_write_reg(demod, 0x03, (sleep) ? 0xf0 : 0x00);
#else
	return tc90522_write_reg(demod, 0x03, (sleep) ? 0x90 : 0x80);
#endif
}

int tc90522_set_agc_t(struct tc90522_demod *demod, bool on)
{
	struct tc90522_regbuf regbuf[] = {
		{ 0x25, NULL, { 0x00 } },
		{ 0x20, NULL, { 0x00 } },
		{ 0x23, NULL, { 0x4d } },
		{ 0x01, NULL, { 0x50 } }
	};

	if (on)
		/* on */
		regbuf[2].u.val &= ~0x01;

	return tc90522_write_multiple_regs(demod, regbuf, 4);
}

int tc90522_get_cndat_t(struct tc90522_demod *demod, u32 *cndat)
{
	int ret = 0;
	u8 b[3];

	ret = tc90522_read_regs(demod, 0x8b, &b[0], 3);
	if (!ret)
		*cndat = (b[0] << 16) | (b[1] << 8) | b[2];

	return ret;
}

int tc90522_enable_ts_pins_t(struct tc90522_demod *demod, bool e)
{
	return tc90522_write_reg(demod, 0x1d, (e) ? 0x00 : 0xa8);
}

int tc90522_is_signal_locked_t(struct tc90522_demod *demod, bool *lock)
{
	int ret = 0;
	u8 b;

	*lock = false;

	mutex_lock(&demod->priv.lock);

	ret = tc90522_read_reg_nolock(demod, 0x80, &b);
	if (ret || (b & 0x28))
		goto exit;

	ret = tc90522_read_reg_nolock(demod, 0xb0, &b);
	if (ret || (b & 0x0f) < 8)
		goto exit;

	*lock = true;

exit:
	mutex_unlock(&demod->priv.lock);

	return 0;
}
