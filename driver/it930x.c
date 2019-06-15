// SPDX-License-Identifier: GPL-2.0-only
/*
 * ITE IT930x driver (it930x.c)
 *
 * Copyright (c) 2018-2019 nns779
 */

#include "print_format.h"

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/firmware.h>

#include "it930x-bus.h"
#include "it930x.h"

#define reg_addr_len(reg)	(((reg) & 0xff000000) ? 4 : (((reg) & 0x00ff0000) ? 3 : (((reg) & 0x0000ff00) ? 2 : 1)))

struct ctrl_buf {
	u8 *buf;
	u8 len;
};

static u16 calc_checksum(const void *buf, size_t len)
{
	int i;
	const u8 *b;
	u16 c = 0;

	i = len / 2;
	b = buf;

	while (i--) {
		c += ((b[0] << 8) | (b[1]));
		b += 2;
	}

	if (len % 2)
		c += (b[0] << 8);

	return ~c;
}

static int _it930x_control(struct it930x_bridge *it930x, u16 cmd, struct ctrl_buf *buf, struct ctrl_buf *rbuf, u8 *rcode, bool no_rx)
{
	int ret;
	u8 *b, l, seq;
	u16 csum1, csum2;
	int rl = 255;

	if (!buf || buf->len > (255 - 4 - 2)) {
		dev_dbg(it930x->dev, "_it930x_control: Invalid parameter.\n");
		return -EINVAL;
	}

	b = it930x->priv.buf;
	l = 3 + buf->len + 2;
	seq = it930x->priv.sequence++;

	b[0] = l;
	b[1] = (cmd >> 8) & 0xff;
	b[2] = cmd & 0xff;
	b[3] = seq;
	if (buf->buf)
		memcpy(&b[4], buf->buf, buf->len);

	csum1 = calc_checksum(&b[1], l - 2);
	b[l - 1] = (csum1 >> 8) & 0xff;
	b[l] = csum1 & 0xff;

	ret = it930x_bus_ctrl_tx(&it930x->bus, b, l + 1, NULL);
	if (ret) {
		dev_err(it930x->dev, "_it930x_control: it930x_bus_ctrl_tx() failed. (cmd: 0x%04x, len: %u, ret: %d)\n", cmd, buf->len, ret);
		goto exit;
	}

	if (no_rx)
		goto exit;

	ret = it930x_bus_ctrl_rx(&it930x->bus, b, &rl, NULL);
	if (ret) {
		dev_err(it930x->dev, "_it930x_control: it930x_bus_ctrl_rx() failed. (cmd: 0x%04x, len: %u, rlen: %u, ret: %d)\n", cmd, buf->len, rl, ret);
		goto exit;
	}

	if (rl < 5) {
		dev_err(it930x->dev, "_it930x_control: No enough response length. (cmd: 0x%04x, len: %u, rlen: %u)\n", cmd, buf->len, rl);
		ret = -EBADMSG;
		goto exit;
	}

	csum1 = calc_checksum(&b[1], rl - 3);
	csum2 = (((b[rl - 2] & 0xff) << 8) | (b[rl - 1] & 0xff));
	if (csum1 != csum2) {
		dev_err(it930x->dev, "_it930x_control: Incorrect checksum! (cmd: 0x%04x, len: %u, rlen: %u, csum1: 0x%04x, csum2: 0x%04x)\n", cmd, buf->len, rl, csum1, csum2);
		ret = -EBADMSG;
		goto exit;
	}

	if (b[1] != seq) {
		dev_err(it930x->dev, "_it930x_control: Incorrect sequence number! (cmd: 0x%04x, len: %u, rlen: %u, seq: %02u, rseq: %02u, csum: 0x%04x)\n", cmd, buf->len, rl, seq, b[1], csum1);
		ret = -EBADMSG;
		goto exit;
	}

	if (b[2]) {
		dev_err(it930x->dev, "_it930x_control: Command failed. (cmd: 0x%04x, len: %u, rlen: %u, rcode: %u, csum: 0x%04x)\n", cmd, buf->len, rl, b[2], csum1);
		ret = -EIO;
	} else if (rbuf) {
		if (rbuf->buf) {
			rbuf->len = ((rl - 3 - 2) > rbuf->len) ? rbuf->len : (rl - 3 - 2);
			memcpy(rbuf->buf, &b[3], rbuf->len);
		} else {
			rbuf->len = rl;
		}
	}

	if (rcode)
		*rcode = b[2];

exit:
	return ret;
}

static int _it930x_write_regs(struct it930x_bridge *it930x, u32 reg, u8 *buf, u8 len)
{
	int ret = 0;
	u8 b[249];
	struct ctrl_buf sb;

	if (!buf || !len || len > (249 - 6)) {
		dev_dbg(it930x->dev, "_it930x_write_regs: Invalid parameter. (reg: 0x%x, len: %u)\n", reg, len);
		return -EINVAL;
	}

	b[0] = len;
	b[1] = reg_addr_len(reg);
	b[2] = (reg >> 24) & 0xff;
	b[3] = (reg >> 16) & 0xff;
	b[4] = (reg >> 8) & 0xff;
	b[5] = reg & 0xff;

	memcpy(&b[6], buf, len);

	sb.buf = b;
	sb.len = 6 + len;

	ret = _it930x_control(it930x, IT930X_CMD_REG_WRITE, &sb, NULL, NULL, false);
	if (ret)
		dev_err(it930x->dev, "_it930x_write_regs: _it930x_control() failed. (reg: 0x%x, len: %u)\n", reg, len);

	return ret;
}

static int _it930x_write_reg(struct it930x_bridge *it930x, u32 reg, u8 val)
{
	return _it930x_write_regs(it930x, reg, &val, 1);
}

int it930x_write_regs(struct it930x_bridge *it930x, struct it930x_regbuf *regbuf, int num)
{
	int ret = 0, i;

	if (!regbuf || !num) {
		dev_dbg(it930x->dev, "it930x_write_regs: Invaild parameter.\n");
		return -EINVAL;
	}

	mutex_lock(&it930x->priv.lock);

	for (i = 0; i < num; i++) {
		if (regbuf[i].buf)
			ret = _it930x_write_regs(it930x, regbuf[i].reg, regbuf[i].buf, regbuf[i].u.len);
		else
			ret = _it930x_write_regs(it930x, regbuf[i].reg, &regbuf[i].u.val, 1);

		if (ret)
			break;
	}

	mutex_unlock(&it930x->priv.lock);

	return ret;
}

int it930x_write_reg(struct it930x_bridge *it930x, u32 reg, u8 val)
{
	int ret = 0;

	mutex_lock(&it930x->priv.lock);

	ret = _it930x_write_regs(it930x, reg, &val, 1);

	mutex_unlock(&it930x->priv.lock);

	return ret;
}

static int _it930x_read_regs(struct it930x_bridge *it930x, u32 reg, u8 *buf, u8 len)
{
	int ret = 0;
	u8 b[6];
	struct ctrl_buf sb, rb;

	if (!buf || !len) {
		dev_dbg(it930x->dev, "_it930x_read_regs: Invalid parameter. (reg: 0x%x, len: %u)\n", reg, len);
		return -EINVAL;
	}

	b[0] = len;
	b[1] = reg_addr_len(reg);
	b[2] = (reg >> 24) & 0xff;
	b[3] = (reg >> 16) & 0xff;
	b[4] = (reg >> 8) & 0xff;
	b[5] = reg & 0xff;

	sb.buf = b;
	sb.len = 6;

	rb.buf = buf;
	rb.len = len;

	ret = _it930x_control(it930x, IT930X_CMD_REG_READ, &sb, &rb, NULL, false);
	if (ret)
		dev_err(it930x->dev, "_it930x_read_regs: _it930x_control() failed. (reg: 0x%x, len: %u, rlen: %u, ret: %d)\n", reg, len, rb.len, ret);
	else if (rb.len != len)
		dev_err(it930x->dev, "_it930x_read_regs: Incorrect size! (reg: 0x%x, len: %u, rlen: %u)\n", reg, len, rb.len);

	return ret;
}

int it930x_read_regs(struct it930x_bridge *it930x, struct it930x_regbuf *regbuf, int num)
{
	int ret = 0, i;

	if (!regbuf || !num) {
		dev_dbg(it930x->dev, "it930x_read_regs: Invald parameter.\n");
		return -EINVAL;
	}

	mutex_lock(&it930x->priv.lock);

	for (i = 0; i < num; i++) {
		ret = _it930x_read_regs(it930x, regbuf[i].reg, regbuf[i].buf, regbuf[i].u.len);
		if (ret)
			break;
	}

	mutex_unlock(&it930x->priv.lock);

	return ret;
}

int it930x_read_reg(struct it930x_bridge *it930x, u32 reg, u8 *val)
{
	int ret = 0;

	mutex_lock(&it930x->priv.lock);

	ret = _it930x_read_regs(it930x, reg, val, 1);

	mutex_unlock(&it930x->priv.lock);

	return ret;
}

static int _it930x_write_reg_bits(struct it930x_bridge *it930x, u32 reg, u8 val, u8 pos, u8 len)
{
	int ret = 0;
	u8 tmp;

	if (len > 8) {
		dev_dbg(it930x->dev, "_it930x_write_reg_bits: Invalid parameter.\n");
		return -EINVAL;
	}

	if (len < 8) {
		ret = _it930x_read_regs(it930x, reg, &tmp, 1);
		if (ret) {
			dev_err(it930x->dev, "_it930x_write_reg_bits: _it930x_read_regs() failed. (reg: 0x%x, val: %u, pos: %u, len: %u, ret: %d)\n", reg, val, pos, len, ret);
			return ret;
		}

		tmp = (val << pos) | (tmp & (~((0xff) >> (8 - len) << pos)));
	} else {
		tmp = val;
	}

	ret = _it930x_write_reg(it930x, reg, tmp);
	if (ret)
		dev_err(it930x->dev, "_it930x_write_reg_bits: _it930x_write_reg() failed. (reg: 0x%x, val: %u, pos: %u, len: %u, t: %u, ret: %d)\n", reg, val, pos, len, tmp, ret);

	return ret;
}

int it930x_write_reg_bits(struct it930x_bridge *it930x, u32 reg, u8 val, u8 pos, u8 len)
{
	int ret = 0;

	mutex_lock(&it930x->priv.lock);

	ret = _it930x_write_reg_bits(it930x, reg, val, pos, len);

	mutex_unlock(&it930x->priv.lock);

	return ret;
}

static int it930x_i2c_master_lock(void *i2c_priv)
{
	struct it930x_i2c_master_info *i2c = i2c_priv;
	struct it930x_priv *priv = &i2c->it930x->priv;

	mutex_lock(&priv->lock);

	return 0;
}

static int it930x_i2c_master_unlock(void *i2c_priv)
{
	struct it930x_i2c_master_info *i2c = i2c_priv;
	struct it930x_priv *priv = &i2c->it930x->priv;

	mutex_unlock(&priv->lock);

	return 0;
}

static int it930x_i2c_master_write(void *i2c_priv, u8 addr, const u8 *data, int len)
{
	int ret = 0;
#ifdef IT930X_I2C_WRITE_REPEAT
	int ret2 = 0;
#endif
	struct it930x_i2c_master_info *i2c = i2c_priv;
	u8 b[249];
	struct ctrl_buf sb;

	if (!data || !len) {
		dev_dbg(i2c->it930x->dev, "it930x_i2c_master_write: Invalid parameter.\n");
		return -EINVAL;
	}

	if (len > (249 - 3)) {
		dev_dbg(i2c->it930x->dev, "it930x_i2c_master_write: Buffer too large.\n");
		return -EINVAL;
	}

	b[0] = len;
	b[1] = i2c->bus;
	b[2] = (addr << 1);
	memcpy(&b[3], data, len);

	sb.buf = b;
	sb.len = 3 + len;

#ifdef IT930X_I2C_WRITE_REPEAT
	ret = _it930x_write_reg(i2c->it930x, 0xf424, 1);
	if (ret) {
		dev_err(i2c->it930x->dev, "it930x_i2c_master_write: it930x_write_reg(0xf424, 1) failed. (ret: %d)\n", ret);
		goto exit;
	}
#endif

	ret = _it930x_control(i2c->it930x, IT930X_CMD_I2C_WRITE, &sb, NULL, NULL, false);

#ifdef IT930X_I2C_WRITE_REPEAT
	ret2 = _it930x_write_reg(i2c->it930x, 0xf424, 0);
	if (ret2)
		dev_err(i2c->it930x->dev, "it930x_i2c_master_write: it930x_write_reg(0xf424, 0) failed. (ret: %d)\n", ret);

	if (!ret)
		ret = ret2;

exit:
#endif
	return ret;
}

static int it930x_i2c_master_read(void *i2c_priv, u8 addr, u8 *data, int len)
{
	int ret = 0;
	struct it930x_i2c_master_info *i2c = i2c_priv;
	u8 b[3];
	struct ctrl_buf sb, rb;

	if (!data || !len) {
		dev_dbg(i2c->it930x->dev, "it930x_i2c_master_read: Invalid parameter.\n");
		return -EINVAL;
	}

	b[0] = len;
	b[1] = i2c->bus;
	b[2] = (addr << 1);

	sb.buf = b;
	sb.len = 3;

	rb.buf = data;
	rb.len = len;

	ret = _it930x_control(i2c->it930x, IT930X_CMD_I2C_READ, &sb, &rb, NULL, false);

	return ret;
}

static int _it930x_get_firmware_version(struct it930x_bridge *it930x, u32 *fw_version)
{
	int ret = 0;
	u8 b[4];
	struct ctrl_buf sb, rb;

	b[0] = 1;

	sb.buf = b;
	sb.len = 1;

	rb.buf = b;
	rb.len = 4;

	ret = _it930x_control(it930x, IT930X_CMD_QUERYINFO, &sb, &rb, NULL, false);
	if (!ret)
		*fw_version = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];

	return ret;
}

static int _it930x_enable_dvbt_mode(struct it930x_bridge *it930x, bool enable)
{
	int ret = 0;

	ret = _it930x_write_reg_bits(it930x, 0xf41f, (enable) ? 1 : 0, 2, 1);
	if (ret)
		return ret;

	// mpeg full speed
	ret = _it930x_write_reg_bits(it930x, 0xda10, (enable) ? 0 : 1, 0, 1);
	if (ret)
		return ret;

	// enable
	ret = _it930x_write_reg_bits(it930x, 0xf41a, (enable) ? 1 : 0, 0, 1);
	if (ret)
		return ret;

	return 0;
}

static int _it930x_enable_stream_output(struct it930x_bridge *it930x, bool enable)
{
	int ret = 0, ret2 = 0;

	ret = _it930x_write_reg_bits(it930x, 0xda1d, 1, 0, 1);
	if (ret)
		return ret;

	switch(it930x->bus.type) {
	case IT930X_BUS_USB:
		// disable ep
		ret = _it930x_write_reg_bits(it930x, 0xdd11, 0, 5, 1);
		if (ret)
			goto end_rst_off;

		// disable/enable nak
		ret = _it930x_write_reg_bits(it930x, 0xdd13, (enable) ? 0 : 1, 5, 1);
		if (ret)
			goto end_rst_off;

		// enable ep
		ret = _it930x_write_reg_bits(it930x, 0xdd11, 1, 5, 1);
		if (ret)
			goto end_rst_off;

		if (enable) {
			u16 x;
			u8 b[2];

			x = (it930x->config.xfer_size / 4) & 0xffff;

			b[0] = (x & 0xff);
			b[1] = ((x >> 8) & 0xff);

			// transfer size
			ret = _it930x_write_regs(it930x, 0xdd88, b, 2);
			if (ret)
				goto end_rst_off;

			// max packet size
			ret = _it930x_write_reg(it930x, 0xdd0c, 512 / 4 /* USB 2.0 */);
			if (ret)
				goto end_rst_off;

			ret = _it930x_write_reg_bits(it930x, 0xda05, 0, 0, 1);
			if (ret)
				goto end_rst_off;

			ret = _it930x_write_reg_bits(it930x, 0xda06, 0, 0, 1);
			if (ret)
				goto end_rst_off;
		}

		break;

	default:
		break;
	}

end_rst_off:
	ret2 = _it930x_write_reg_bits(it930x, 0xda1d, 0, 0, 1);

	// reverse: no
	ret2 = _it930x_write_reg(it930x, 0xd920, 0);

	return (ret) ? ret : ret2;
}

#if 0
static int _it930x_set_xfer_size(struct it930x_bridge *it930x, u32 xfer_size)
{
	int ret = 0, ret2 = 0;
	struct it930x_regbuf regbuf[2];
	u16 x;
	u8 b[2];

	if (it930x->bus.type != IT930X_BUS_USB)
		// only for usb
		return 0;

	ret = _it930x_write_reg_bits(it930x, 0xda1d, 1, 0, 1);
	if (ret)
		return ret;

	x = (xfer_size / 4) & 0xffff;

	b[0] = (x & 0xff);
	b[1] = ((x >> 8) & 0xff);

	// transfer size
	ret = _it930x_write_regs(it930x, 0xdd88, b, 2);
	if (ret)
		goto exit;

	// max packet size
	ret = _it930x_write_reg(it930x, 0xdd0c, 512 / 4 /* USB 2.0 */);
	if (ret)
		goto exit;

exit:
	ret2 = _it930x_write_reg_bits(it930x, 0xda1d, 0, 0, 1);

	return (ret) ? ret : ret2;
}
#endif

static int _it930x_config_i2c(struct it930x_bridge *it930x)
{
	int ret = 0, i;
	u32 i2c_regs[5][2] = {
		{ 0x4975, 0x4971 },
		{ 0x4974, 0x4970 },
		{ 0x4973, 0x496f },
		{ 0x4972, 0x496e },
		{ 0x4964, 0x4963 }
	};

	// set i2c speed

	ret = _it930x_write_reg(it930x, 0xf6a7, it930x->config.i2c_speed);
	if (ret)
		return ret;

	ret = _it930x_write_reg(it930x, 0xf103, it930x->config.i2c_speed);
	if (ret)
		return ret;

	// set i2c address and bus

	for(i = 0; i < 5; i++) {
		struct it930x_stream_input *input = &it930x->config.input[i];

		if (input->enable) {
			ret = _it930x_write_reg(it930x, i2c_regs[input->slave_number][0], input->i2c_addr);
			if (ret)
				break;

			ret = _it930x_write_reg(it930x, i2c_regs[input->slave_number][1], input->i2c_bus);
			if (ret)
				break;
		}
	}

	return ret;
}

static int _it930x_config_stream_input(struct it930x_bridge *it930x)
{
	int ret = 0, i;

	for (i = 0; i < 5; i++) {
		struct it930x_stream_input *input = &it930x->config.input[i];

		if (!input->enable) {
			// disable input port
			ret = _it930x_write_reg(it930x, 0xda4c + input->port_number, 0);
			if (ret) {
				dev_err(it930x->dev, "_it930x_config_stream_input: _it930x_write_reg(0xda4c + port_number): failed. (idx: %d, ret: %d)\n", i, ret);
				break;
			}

			continue;
		}

		if (input->port_number < 2) {
			ret = _it930x_write_reg(it930x, 0xda58 + input->port_number, (input->is_parallel) ? 1 : 0);
			if (ret) {
				dev_err(it930x->dev, "_it930x_config_stream_input: _it930x_write_reg(0xda58 + port_number): failed. (idx: %d, ret: %d)\n", i, ret);
				break;
			}
		}

		// aggregation mode: sync byte
		ret = _it930x_write_reg(it930x, 0xda73 + input->port_number, 1);
		if (ret) {
			dev_err(it930x->dev, "_it930x_config_stream_input: _it930x_write_reg(0xda73 + port_number): failed. (idx: %d, ret: %d)\n", i, ret);
			break;
		}

		// write sync byte
		ret = _it930x_write_reg(it930x, 0xda78 + input->port_number, input->sync_byte);
		if (ret) {
			dev_err(it930x->dev, "_it930x_config_stream_input: _it930x_write_reg(0xda78 + port_number): failed. (idx: %d, ret: %d)\n", i, ret);
			break;
		}

		// enable input port
		ret = _it930x_write_reg(it930x, 0xda4c + input->port_number, 1);
		if (ret) {
			dev_err(it930x->dev, "_it930x_config_stream_input: _it930x_write_reg(0xda4c + port_number): failed. (idx: %d, ret: %d)\n", i, ret);
			break;
		}
	}

	return ret;
}

int it930x_init(struct it930x_bridge *it930x)
{
	u8 *buf;
	int i;

	buf = kmalloc(sizeof(u8) * 255, GFP_NOIO);
	if (!buf)
		return -ENOMEM;

	mutex_init(&it930x->priv.lock);

	// set i2c operator

	for (i = 0; i < 2; i++) {
		it930x->priv.i2c[i].it930x = it930x;
		it930x->priv.i2c[i].bus = i + 2;

		it930x->i2c_master[i].lock = it930x_i2c_master_lock;
		it930x->i2c_master[i].unlock = it930x_i2c_master_unlock;
		it930x->i2c_master[i].wr = it930x_i2c_master_write;
		it930x->i2c_master[i].rd = it930x_i2c_master_read;
		it930x->i2c_master[i].priv = &it930x->priv.i2c[i];
	}

	it930x->priv.buf = buf;

	return 0;
}

int it930x_term(struct it930x_bridge *it930x)
{
	int i;

	if (it930x->priv.buf) {
		kfree(it930x->priv.buf);
		it930x->priv.buf = NULL;
	}

	// clear i2c operator

	for (i = 0; i < 2; i++) {
		it930x->priv.i2c[i].it930x = NULL;
		it930x->priv.i2c[i].bus = 0;

		it930x->i2c_master[i].lock = NULL;
		it930x->i2c_master[i].unlock = NULL;
		it930x->i2c_master[i].wr = NULL;
		it930x->i2c_master[i].rd = NULL;
		it930x->i2c_master[i].priv = NULL;
	}

	mutex_destroy(&it930x->priv.lock);

	return 0;
}

int it930x_load_firmware(struct it930x_bridge *it930x, const char *filename)
{
	int ret = 0;
	u32 fw_version;
	const struct firmware *fw;
	size_t i, n, len = 0;
	struct ctrl_buf sb;

	if (!filename)
		return -EINVAL;

	mutex_lock(&it930x->priv.lock);

	ret = _it930x_get_firmware_version(it930x, &fw_version);
	if (ret) {
		dev_err(it930x->dev, "it930x_load_firmware: _it930x_get_firmware_version() failed. 1 (ret: %d)\n", ret);
		goto exit;
	}

	if (fw_version) {
		dev_info(it930x->dev, "Firmware is already loaded. version: %d.%d.%d.%d\n", ((fw_version >> 24) & 0xff), ((fw_version >> 16) & 0xff), ((fw_version >> 8) & 0xff), (fw_version & 0xff));
		goto exit;
	}

	ret = _it930x_write_reg(it930x, 0xf103, it930x->config.i2c_speed);
	if (ret) {
		dev_err(it930x->dev, "it930x_load_firmware: _it930x_write_reg(0xf103) failed. (ret: %d)\n", ret);
		goto exit;
	}

	ret = request_firmware(&fw, filename, it930x->dev);
	if (ret) {
		dev_err(it930x->dev, "it930x_load_firmware: request_firmware() failed. (ret: %d)\n", ret);
		dev_err(it930x->dev, "Couldn't load firmware from the file.\n");
		goto exit;
	}

	n = fw->size;

	for(i = 0; i < n; i += len) {
		const u8 *p = &fw->data[i];
		unsigned j, m = p[3];

		len = 0;

		if (p[0] != 0x03) {
			dev_err(it930x->dev, "it930x_load_firmware: Invalid firmware block was found. Abort. (ofs: %zx)\n", i);
			ret = -ECANCELED;
			goto exit_fw;
		}

		for(j = 0; j < m; j++)
			len += p[6 + (j * 3)];

		if (!len) {
			dev_warn(it930x->dev, "it930x_load_firmware: No data in the block. (ofs: %zx)\n", i);
			continue;
		}

		len += 4 + (m * 3);

		// send firmware block

		sb.buf = (u8 *)p;
		sb.len = len;

		ret = _it930x_control(it930x, IT930X_CMD_FW_SCATTER_WRITE, &sb, NULL, NULL, false);
		if (ret) {
			dev_err(it930x->dev, "it930x_load_firmware: _it930x_control(IT930X_CMD_FW_SCATTER_WRITE) failed. (ofs: %zx, ret: %d)\n", i, ret);
			goto exit_fw;
		}
	}

	sb.buf = NULL;
	sb.len = 0;

	ret = _it930x_control(it930x, IT930X_CMD_BOOT, &sb, NULL, NULL, false);
	if (ret) {
		dev_err(it930x->dev, "it930x_load_firmware: _it930x_control(IT930X_CMD_BOOT) failed. (ret: %d)\n", ret);
		goto exit_fw;
	}

	ret = _it930x_get_firmware_version(it930x, &fw_version);
	if (ret) {
		dev_err(it930x->dev, "it930x_load_firmware: _it930x_get_firmware_version() failed. 2 (ret: %d)\n", ret);
		goto exit_fw;
	}

	if (!fw_version) {
		ret = -EREMOTEIO;
		goto exit_fw;
	}

	dev_info(it930x->dev, "Firmware loaded. version: %d.%d.%d.%d\n", ((fw_version >> 24) & 0xff), ((fw_version >> 16) & 0xff), ((fw_version >> 8) & 0xff), (fw_version & 0xff));

exit_fw:
	release_firmware(fw);

exit:
	mutex_unlock(&it930x->priv.lock);

	return ret;
}

int it930x_init_device(struct it930x_bridge *it930x)
{
	int ret = 0;

	if (it930x->bus.type != IT930X_BUS_USB) {
		dev_dbg(it930x->dev, "it930x_init_device: This driver only supports usb.\n");
		return -EINVAL;
	}

	mutex_lock(&it930x->priv.lock);

	ret = _it930x_write_reg(it930x, 0x4976, 0);
	if (ret)
		goto exit;

	ret = _it930x_write_reg(it930x, 0x4bfb, 0);
	if (ret)
		goto exit;

	ret = _it930x_write_reg(it930x, 0x4978, 0);
	if (ret)
		goto exit;

	ret = _it930x_write_reg(it930x, 0x4977, 0);
	if (ret)
		goto exit;

	// ignore sync byte: no
	ret = _it930x_write_reg(it930x, 0xda1a, 0);
	if (ret)
		goto exit;

	ret = _it930x_enable_dvbt_mode(it930x, true);
	if (ret) {
		dev_err(it930x->dev, "it930x_init_device: _it930x_enable_dvbt_mode() failed.\n");
		goto exit;
	}

	ret = _it930x_enable_stream_output(it930x, true);
	if (ret) {
		dev_err(it930x->dev, "it930x_init_device: _it930x_enable_stream_output() failed.\n");
		goto exit;
	}

	// power config ?

	ret = _it930x_write_reg(it930x, 0xd833, 1);
	if (ret)
		goto exit;

	ret = _it930x_write_reg(it930x, 0xd830, 0);
	if (ret)
		goto exit;

	ret = _it930x_write_reg(it930x, 0xd831, 1);
	if (ret)
		goto exit;

	ret = _it930x_write_reg(it930x, 0xd832, 0);
	if (ret)
		goto exit;

	ret = _it930x_config_i2c(it930x);
	if (ret) {
		dev_err(it930x->dev, "it930x_init_device: _it930x_config_i2c() failed. (ret: %d)\n", ret);
		goto exit;
	}

	ret = _it930x_config_stream_input(it930x);
	if (ret) {
		dev_err(it930x->dev, "it930x_init_device: _it930x_config_stream_input() failed. (ret: %d)\n", ret);
		goto exit;
	}

exit:
	mutex_unlock(&it930x->priv.lock);

	return 0;
}

int it930x_set_gpio_mode(struct it930x_bridge *it930x, int gpio, enum it930x_gpio_mode mode, bool enable)
{
	u32 gpio_en_regs[] = {
		0xd8b0,		// gpioh1
		0xd8b8,		// gpioh2
		0xd8b4,		// gpioh3
		0xd8c0,		// gpioh4
		0xd8bc,		// gpioh5
		0xd8c8,		// gpioh6
		0xd8c4,		// gpioh7
		0xd8d0,		// gpioh8
		0xd8cc,		// gpioh9
		0xd8d8,		// gpioh10
		0xd8d4,		// gpioh11
		0xd8e0,		// gpioh12
		0xd8dc,		// gpioh13
		0xd8e4,		// gpioh14
		0xd8e8,		// gpioh15
		0xd8ec,		// gpioh16
	};
	u8 val;
	struct it930x_regbuf regbuf[2];
	int num = 1;

	if (gpio <= 0 || gpio > (sizeof(gpio_en_regs) / sizeof(gpio_en_regs[0])))
		return -EINVAL;

	switch (mode) {
	case IT930X_GPIO_IN:
		val = 0;
		break;

	case IT930X_GPIO_OUT:
		val = 1;
		break;

	default:
		return -EINVAL;
	}

	gpio--;

	if (it930x->priv.status[gpio].mode == mode)
		return 0;

	it930x->priv.status[gpio].mode = mode;

	it930x_regbuf_set_val(&regbuf[0], gpio_en_regs[gpio], val);
	if (enable && !it930x->priv.status[gpio].enable) {
		it930x_regbuf_set_val(&regbuf[1], gpio_en_regs[gpio] + 1, 1);
		it930x->priv.status[gpio].enable = true;
		num++;
	}

	return it930x_write_regs(it930x, regbuf, num);
}

int it930x_enable_gpio(struct it930x_bridge *it930x, int gpio, bool enable)
{
	u32 gpio_on_regs[] = {
		0xd8b1,		// gpioh1
		0xd8b9,		// gpioh2
		0xd8b5,		// gpioh3
		0xd8c1,		// gpioh4
		0xd8bd,		// gpioh5
		0xd8c9,		// gpioh6
		0xd8c5,		// gpioh7
		0xd8d1,		// gpioh8
		0xd8cd,		// gpioh9
		0xd8d9,		// gpioh10
		0xd8d5,		// gpioh11
		0xd8e1,		// gpioh12
		0xd8dd,		// gpioh13
		0xd8e5,		// gpioh14
		0xd8e9,		// gpioh15
		0xd8ed,		// gpioh16
	};

	if (gpio <= 0 || gpio > (sizeof(gpio_on_regs) / sizeof(gpio_on_regs[0])))
		return -EINVAL;

	gpio--;

	if ((!it930x->priv.status[gpio].enable && !enable) || (it930x->priv.status[gpio].enable && enable))
		return 0;

	it930x->priv.status[gpio].enable = (enable) ? true : false;

	return it930x_write_reg(it930x, gpio_on_regs[gpio], (enable) ? 1 : 0);
}

int it930x_read_gpio(struct it930x_bridge *it930x, int gpio, bool *high)
{
	int ret = 0;
	u32 gpio_i_regs[] = {
		0xd8ae,		// gpioh1
		0xd8b6,		// gpioh2
		0xd8b2,		// gpioh3
		0xd8be,		// gpioh4
		0xd8ba,		// gpioh5
		0xd8c6,		// gpioh6
		0xd8c2,		// gpioh7
		0xd8ce,		// gpioh8
		0xd8ca,		// gpioh9
		0xd8d6,		// gpioh10
		0xd8d2,		// gpioh11
		0xd8de,		// gpioh12
		0xd8da,		// gpioh13
		0xd8e2,		// gpioh14
		0xd8e6,		// gpioh15
		0xd8ea,		// gpioh16
	};
	u8 tmp;

	if (gpio <= 0 || gpio > (sizeof(gpio_i_regs) / sizeof(gpio_i_regs[0])))
		return -EINVAL;

	gpio--;

	ret = it930x_read_reg(it930x, gpio_i_regs[gpio], &tmp);
	if (!ret)
		*high = (tmp) ? true : false;

	return ret;
}

int it930x_write_gpio(struct it930x_bridge *it930x, int gpio, bool high)
{
	u32 gpio_o_regs[] = {
		0xd8af,		// gpioh1
		0xd8b7,		// gpioh2
		0xd8b3,		// gpioh3
		0xd8bf,		// gpioh4
		0xd8bb,		// gpioh5
		0xd8c7,		// gpioh6
		0xd8c3,		// gpioh7
		0xd8cf,		// gpioh8
		0xd8cb,		// gpioh9
		0xd8d7,		// gpioh10
		0xd8d3,		// gpioh11
		0xd8df,		// gpioh12
		0xd8db,		// gpioh13
		0xd8e3,		// gpioh14
		0xd8e7,		// gpioh15
		0xd8eb,		// gpioh16
	};

	if (gpio <= 0 || gpio > (sizeof(gpio_o_regs) / sizeof(gpio_o_regs[0])))
		return -EINVAL;

	gpio--;

	return it930x_write_reg(it930x, gpio_o_regs[gpio], (high) ? 1 : 0);
}

int it930x_enable_stream_input(struct it930x_bridge *it930x, u8 input_idx, bool enable)
{
	if (input_idx >= 5)
		return -EINVAL;

	return it930x_write_reg(it930x, 0xda4c + it930x->config.input[input_idx].port_number, (enable) ? 1 : 0);
}

int it930x_purge_psb(struct it930x_bridge *it930x, int timeout)
{
	int ret = 0;
	void *p;
	int len;

	if (it930x->bus.type != IT930X_BUS_USB)
		return -EINVAL;

	mutex_lock(&it930x->priv.lock);

	ret = _it930x_write_reg_bits(it930x, 0xda1d, 1, 0, 1);
	if (ret)
		goto exit;

	len = 1024;

	p = kmalloc(len, GFP_KERNEL);
	if (!p) {
		ret = -ENOMEM;
		goto exit;
	}

	ret = it930x_bus_stream_rx(&it930x->bus, p, &len, timeout);
	kfree(p);

	_it930x_write_reg_bits(it930x, 0xda1d, 0, 0, 1);

	dev_dbg(it930x->dev, "it930x_purge_psb: len: %d\n", len);

	if (len == 512)
		ret = 0;

exit:
	mutex_unlock(&it930x->priv.lock);

	return ret;
}
