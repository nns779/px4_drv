// tc90522.c

// Toshiba TC90522 driver

#include "print_format.h"

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/device.h>

#include "i2c_comm.h"
#include "tc90522.h"

static int _tc90522_write_regs(struct tc90522_demod *demod, u8 reg, u8 *buf, u8 len)
{
	int ret = 0;
	u8 b[255];

	if (!buf || !len)
		return -EINVAL;
	else if (len > 254) {
		dev_dbg(demod->dev, "_tc90522_write_regs: Buffer too large. (addr: 0x%x, reg: %u, len: 0x%x)\n", demod->i2c_addr, reg, len);
		return -EINVAL;
	}

	b[0] = reg;
	memcpy(&b[1], buf, len);

	ret = i2c_comm_master_lock(demod->i2c);
	if (ret) {
		dev_err(demod->dev, "_tc90522_write_regs: i2c_comm_master_lock() failed. (addr: 0x%x, reg: 0x%x, len: %u, ret: %d)\n", demod->i2c_addr, reg, len, ret);
		return ret;
	}

	ret = i2c_comm_master_write(demod->i2c, demod->i2c_addr, b, len + 1);
	if (ret)
		dev_err(demod->dev, "_tc90522_write_regs: i2c_comm_master_write() failed. (addr: 0x%x, reg: 0x%x, len: %u, ret: %d)\n", demod->i2c_addr, reg, len, ret);

	i2c_comm_master_unlock(demod->i2c);

	return ret;
}

int tc90522_write_regs(struct tc90522_demod *demod, struct tc90522_regbuf *regbuf, int num)
{
	int ret = 0, i;

	if (!regbuf || !num)
		return -EINVAL;

	mutex_lock(&demod->priv.lock);

	for (i = 0; i < num; i++) {
		if (regbuf[i].buf)
			ret = _tc90522_write_regs(demod, regbuf[i].reg, regbuf[i].buf, regbuf[i].u.len);
		else
			ret = _tc90522_write_regs(demod, regbuf[i].reg, &regbuf[i].u.val, 1);

		if (ret)
			break;
	}

	mutex_unlock(&demod->priv.lock);

	return ret;
}

int tc90522_write_reg(struct tc90522_demod *demod, u8 reg, u8 val)
{
	int ret = 0;

	mutex_lock(&demod->priv.lock);

	ret = _tc90522_write_regs(demod, reg, &val, 1);

	mutex_unlock(&demod->priv.lock);

	return ret;
}

static int _tc90522_read_regs(struct tc90522_demod *demod, u8 reg, u8 *buf, u8 len)
{
	int ret = 0;
	u8 b;

	if (!buf || !len)
		return -EINVAL;

	b = reg;

	ret = i2c_comm_master_lock(demod->i2c);
	if (ret) {
		dev_err(demod->dev, "_tc90522_read_regs: i2c_comm_master_lock() failed. (addr: 0x%x, reg: 0x%x, len: %u)\n", demod->i2c_addr, reg, len);
		return ret;
	}

	ret = i2c_comm_master_write(demod->i2c, demod->i2c_addr, &b, 1);
	if (ret) {
		dev_err(demod->dev, "_tc90522_read_regs: i2c_comm_master_write() failed. (addr: 0x%x, reg: 0x%x, len: %u)\n", demod->i2c_addr, reg, len);
		goto exit;
	}

	ret = i2c_comm_master_read(demod->i2c, demod->i2c_addr, buf, len);
	if (ret)
		dev_err(demod->dev, "_tc90522_read_regs: i2c_comm_master_read() failed. (addr: 0x%x, reg: 0x%x, len: %u)\n", demod->i2c_addr, reg, len);

exit:
	i2c_comm_master_unlock(demod->i2c);

	return ret;
}

int tc90522_read_regs(struct tc90522_demod *demod, struct tc90522_regbuf *regbuf, int num)
{
	int ret = 0, i;

	if (!regbuf || !num)
		return -EINVAL;

	mutex_lock(&demod->priv.lock);

	for (i = 0; i < num; i++) {
		ret = _tc90522_read_regs(demod, regbuf[i].reg, regbuf[i].buf, regbuf[i].u.len);
		if (ret)
			break;
	}

	mutex_unlock(&demod->priv.lock);

	return ret;
}

int tc90522_read_reg(struct tc90522_demod *demod, u8 reg, u8 *val)
{
	int ret = 0;

	mutex_lock(&demod->priv.lock);

	ret = _tc90522_read_regs(demod, reg, val, 1);

	mutex_unlock(&demod->priv.lock);

	return ret;
}

static int tc90522_i2c_master_lock(void *i2c_priv)
{
	struct tc90522_demod *demod = i2c_priv;

	mutex_lock(&demod->priv.lock);
	return i2c_comm_master_lock(demod->i2c);
}

static int tc90522_i2c_master_unlock(void *i2c_priv)
{
	int ret = 0;
	struct tc90522_demod *demod = i2c_priv;

	ret = i2c_comm_master_unlock(demod->i2c);
	mutex_unlock(&demod->priv.lock);

	return ret;
}

static int tc90522_i2c_master_write(void *i2c_priv, u8 addr, const u8 *data, int len)
{
	int ret = 0;
	struct tc90522_demod *demod = i2c_priv;
	u8 b[255];

	if (!data || !len || len > 253)
		return -EINVAL;

	b[0] = 0xfe;
	b[1] = (addr << 1);
	memcpy(&b[2], data, len);

	ret = i2c_comm_master_write(demod->i2c, demod->i2c_addr, b, len + 2);
	if (ret)
		dev_err(demod->dev, "tc09522_i2c_master_write: i2c_comm_master_write() failed. (demod addr: 0x%x, addr: 0x%x, len: %u)\n", demod->i2c_addr, addr, len);

	return ret;
}

static int tc90522_i2c_master_read(void *i2c_priv, u8 addr, u8 *data, int len)
{
	int ret = 0;
	struct tc90522_demod *demod = i2c_priv;
	u8 b[2];

	if (!data || !len)
		return -EINVAL;

	b[0] = 0xfe;
	b[1] = (addr << 1) | 0x01;

	ret = i2c_comm_master_write(demod->i2c, demod->i2c_addr, b, 2);
	if (ret) {
		dev_err(demod->dev, "tc09522_i2c_master_read: i2c_comm_master_write() failed. (demod addr: 0x%x, addr: 0x%x, len: %u)\n", demod->i2c_addr, addr, len);
		return ret;
	}

	ret = i2c_comm_master_read(demod->i2c, demod->i2c_addr, data, len);
	if (ret)
		dev_err(demod->dev, "tc09522_i2c_master_read: i2c_comm_master_read() failed. (demod addr: 0x%x, addr: 0x%x, len: %u)\n", demod->i2c_addr, addr, len);

	return ret;
}

int tc90522_init(struct tc90522_demod *demod)
{
	mutex_init(&demod->priv.lock);

	demod->i2c_master.lock = tc90522_i2c_master_lock;
	demod->i2c_master.unlock = tc90522_i2c_master_unlock;
	demod->i2c_master.wr = tc90522_i2c_master_write;
	demod->i2c_master.rd = tc90522_i2c_master_read;
	demod->i2c_master.priv = demod;

	return 0;
}

int tc90522_term(struct tc90522_demod *demod)
{
	demod->i2c_master.wr = NULL;
	demod->i2c_master.rd = NULL;
	demod->i2c_master.priv = NULL;

	mutex_unlock(&demod->priv.lock);

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
		// sleep
		regbuf[0].u.val = 0x80;
		regbuf[1].u.val = 0xff;
	}

	return tc90522_write_regs(demod, regbuf, 2);
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
		// on
		regbuf[0].u.val = 0xff;
		regbuf[1].u.val |= 0x02;
		regbuf[2].u.val = 0x00;
	}

	return tc90522_write_regs(demod, regbuf, 4);
}

int tc90522_tmcc_get_tsid_s(struct tc90522_demod *demod, u8 idx, u16 *tsid)
{
	int ret = 0;
	u8 b[2];
	struct tc90522_regbuf regbuf[1];

	if (idx >= 12)
		return -EINVAL;

	tc90522_regbuf_set_buf(&regbuf[0], 0xce + (idx * 2), &b[0], 2);
	ret = tc90522_read_regs(demod, regbuf, 1);
	if (!ret)
		*tsid = (b[0] << 8 | b[1]);

	return ret;
}

int tc90522_set_tsid_s(struct tc90522_demod *demod, u16 tsid)
{
	u8 b[2];
	struct tc90522_regbuf regbuf[2];

	b[0] = ((tsid >> 8) & 0xff);
	b[1] = (tsid & 0xff);

	tc90522_regbuf_set_buf(&regbuf[0], 0x8f, &b[0], 1);
	tc90522_regbuf_set_buf(&regbuf[1], 0x90, &b[1], 1);

	return tc90522_write_regs(demod, regbuf, 2);
}

int tc90522_get_tsid_s(struct tc90522_demod *demod, u16 *tsid)
{
	int ret = 0;
	u8 b[2];
	struct tc90522_regbuf regbuf[1];

	tc90522_regbuf_set_buf(&regbuf[0], 0xe6, &b[0], 2);
	ret = tc90522_read_regs(demod, regbuf, 1);
	if (!ret)
		*tsid = (b[0] << 8 | b[1]);

	return ret;
}

int tc90522_get_cn_s(struct tc90522_demod *demod, u16 *cn)
{
	int ret = 0;
	u8 b[2];
	struct tc90522_regbuf regbuf[1];

	tc90522_regbuf_set_buf(&regbuf[0], 0xbc, &b[0], 2);
	ret = tc90522_read_regs(demod, regbuf, 1);
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

	return tc90522_write_regs(demod, regbuf, 2);
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
		// on
		regbuf[2].u.val &= ~0x01;

	return tc90522_write_regs(demod, regbuf, 4);
}

int tc90522_get_cndat_t(struct tc90522_demod *demod, u32 *cndat)
{
	int ret = 0;
	u8 b[3];
	struct tc90522_regbuf regbuf[1];

	tc90522_regbuf_set_buf(&regbuf[0], 0x8b, &b[0], 3);
	ret = tc90522_read_regs(demod, regbuf, 1);
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

	ret = _tc90522_read_regs(demod, 0x80, &b, 1);
	if (ret || (b & 0x28))
		goto exit;

	ret = _tc90522_read_regs(demod, 0xb0, &b, 1);
	if (ret || (b & 0x0f) < 8)
		goto exit;

	*lock = true;

exit:
	mutex_unlock(&demod->priv.lock);

	return 0;
}
