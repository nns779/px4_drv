// SPDX-License-Identifier: GPL-2.0-only
/*
 * ITE IT930x driver (it930x.c)
 *
 * Copyright (c) 2018-2019 nns779
 */

#include "print_format.h"

#if defined(_WIN32) || defined(_WIN64)
#include "misc_win.h"
#else
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/firmware.h>
#endif

#include "it930x.h"
#include "itedtv_bus.h"

struct it930x_i2c_master_info {
	struct it930x_bridge *it930x;
	u8 bus;
};

struct it930x_gpio_state {
	bool enable;
	enum it930x_gpio_mode mode;
};

struct it930x_priv {
	struct mutex ctrl_lock;
	struct mutex i2c_lock;
	struct mutex gpio_lock;
	u8 *buf;
	u8 seq;
	struct it930x_i2c_master_info i2c[3];
	struct it930x_gpio_state status[16];
};

struct it930x_ctrl_buf {
	u8 *buf;
	u8 len;
};

static inline u8 it930x_reg_length(u32 reg)
{
	if (reg & 0xff000000)
		return 4;

	if (reg & 0x00ff0000)
		return 3;

	if (reg & 0x0000ff00)
		return 2;

	return 1;
}

static u16 it930x_calc_checksum(void *buf, size_t len)
{
	int i;
	u8 *b;
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

static int it930x_ctrl_msg(struct it930x_bridge *it930x, u16 cmd, struct it930x_ctrl_buf *wbuf, struct it930x_ctrl_buf *rbuf, u8 *result, bool no_rx)
{
	int ret;
	struct it930x_priv *priv = it930x->priv;
	u8 *buf, len, seq;
	u16 csum, csum2;
	int rlen = 256;

	if (wbuf && wbuf->len > (255 - 3 - 2))
		return -EINVAL;

	mutex_lock(&priv->ctrl_lock);

	buf = priv->buf;
	len = 4 + 2;
	if (wbuf)
		len += wbuf->len;
	seq = priv->seq++;

	buf[0] = len - 1;
	buf[1] = ((cmd >> 8) & 0xff);
	buf[2] = (cmd & 0xff);
	buf[3] = seq;
	if (wbuf && wbuf->buf)
		memcpy(&buf[4], wbuf->buf, wbuf->len);

	csum = it930x_calc_checksum(&buf[1], len - 1 - 2);
	buf[len - 2] = ((csum >> 8) & 0xff);
	buf[len - 1] = (csum & 0xff);

	ret = itedtv_bus_ctrl_tx(&it930x->bus, buf, len);
	if (ret)
		goto exit;

	if (no_rx)
		goto exit;

	ret = itedtv_bus_ctrl_rx(&it930x->bus, buf, &rlen);
	if (ret)
		goto exit;

	if (rlen < 5) {
		dev_err(it930x->dev, "it930x_ctrl_msg: no enough response length. (rlen: %d)\n", rlen);
		ret = -EBADMSG;
		goto exit;
	}

	csum = it930x_calc_checksum(&buf[1], rlen - 1 - 2);
	csum2 = ((buf[rlen - 2] << 8) | buf[rlen - 1]);
	if (csum != csum2) {
		dev_err(it930x->dev, "it930x_ctrl_msg: checksum is incorrect. (0x%02x, 0x%02x)\n", csum, csum2);
		ret = -EBADMSG;
		goto exit;
	}

	if (buf[1] != seq) {
		dev_err(it930x->dev, "it930x_ctrl_msg: sequence number is incorrect. (tx: 0x%02x, rx: 0x%02x)\n", seq, buf[1]);
		ret = -EBADMSG;
		goto exit;
	}

	if (buf[2] > 1) {
		dev_err(it930x->dev, "it930x_ctrl_msg: error returned. (result: %u)\n", buf[2]);
		ret = -EIO;
	} else if (!buf[2] && rbuf) {
		if (rbuf->buf) {
			rbuf->len = ((rlen - 3 - 2) > rbuf->len) ? rbuf->len : (rlen - 3 - 2);
			memcpy(rbuf->buf, &buf[3], rbuf->len);
		} else
			rbuf->len = rlen - 3 - 2;
	}

	if (result)
		*result = buf[2];

exit:
	if (ret)
		dev_err(it930x->dev, "it930x_ctrl_msg: operation failed. (cmd: 0x%04x, ret: %d)\n", cmd, ret);

	mutex_unlock(&priv->ctrl_lock);

	return ret;
}

int it930x_read_regs(struct it930x_bridge *it930x, u32 reg, u8 *rbuf, u8 len)
{
	u8 buf[6];
	struct it930x_ctrl_buf wb, rb;

	if (!rbuf || !len || len > 251)
		return -EINVAL;

	buf[0] = len;
	buf[1] = it930x_reg_length(reg);
	buf[2] = ((reg >> 24) & 0xff);
	buf[3] = ((reg >> 16) & 0xff);
	buf[4] = ((reg >> 8) & 0xff);
	buf[5] = (reg & 0xff);

	wb.buf = buf;
	wb.len = 6;

	rb.buf = rbuf;
	rb.len = len;

	return it930x_ctrl_msg(it930x, IT930X_CMD_REG_READ, &wb, &rb, NULL, false);
}

int it930x_read_reg(struct it930x_bridge *it930x, u32 reg, u8 *val)
{
	return it930x_read_regs(it930x, reg, val, 1);
}

int it930x_write_regs(struct it930x_bridge *it930x, u32 reg, u8 *wbuf, u8 len)
{
	u8 buf[250];
	struct it930x_ctrl_buf wb;

	if (!wbuf || !len || len > (250 - 6))
		return -EINVAL;

	buf[0] = len;
	buf[1] = it930x_reg_length(reg);
	buf[2] = ((reg >> 24) & 0xff);
	buf[3] = ((reg >> 16) & 0xff);
	buf[4] = ((reg >> 8) & 0xff);
	buf[5] = (reg & 0xff);
	memcpy(&buf[6], wbuf, len);

	wb.buf = buf;
	wb.len = 6 + len;

	return it930x_ctrl_msg(it930x, IT930X_CMD_REG_WRITE, &wb, NULL, NULL, false);
}

int it930x_write_reg(struct it930x_bridge *it930x, u32 reg, u8 val)
{
	return it930x_write_regs(it930x, reg, &val, 1);
}

int it930x_write_reg_mask(struct it930x_bridge *it930x, u32 reg, u8 val, u8 mask)
{
	int ret = 0;
	u8 tmp;

	if (!mask)
		return -EINVAL;

	if (mask != 0xff) {
		ret = it930x_read_reg(it930x, reg, &tmp);
		if (ret)
			return ret;

		tmp &= ~mask;
		tmp |= (val & mask);
	} else
		tmp = val;

	return it930x_write_reg(it930x, reg, tmp);
}

static int it930x_i2c_master_request(void *i2c_priv, struct i2c_comm_request *req, int num)
{
	int ret = 0, i;
	struct it930x_i2c_master_info *i2c = i2c_priv;
	struct it930x_priv *priv = i2c->it930x->priv;

	mutex_lock(&priv->i2c_lock);

	for (i = 0; i < num; i++) {
		u16 addr;
		u8 *data;
		int len;
		u8 buf[250];

		addr = req[i].addr;
		data = req[i].data;
		len = req[i].len;

		if (!data || !len) {
			ret = -EINVAL;
			break;
		}

		switch (req[i].req) {
		case I2C_READ_REQUEST:
		{
			struct it930x_ctrl_buf wb, rb;

			if (len > 251) {
				ret = -EINVAL;
				break;
			}

			buf[0] = len;
			buf[1] = i2c->bus;
			buf[2] = (addr << 1);

			wb.buf = buf;
			wb.len = 3;

			rb.buf = data;
			rb.len = len;

			ret = it930x_ctrl_msg(i2c->it930x, IT930X_CMD_I2C_READ, &wb, &rb, NULL, false);
			break;
		}

		case I2C_WRITE_REQUEST:
		{
			struct it930x_ctrl_buf wb;

			if (len > (250 - 3)) {
				ret = -EINVAL;
				break;
			}

			buf[0] = len;
			buf[1] = i2c->bus;
			buf[2] = (addr << 1);
			memcpy(&buf[3], data, len);

			wb.buf = buf;
			wb.len = 3 + len;

			ret = it930x_ctrl_msg(i2c->it930x, IT930X_CMD_I2C_WRITE, &wb, NULL, NULL, false);
			break;
		}

		default:
			ret = -EINVAL;
			break;
		}

		if (ret)
			break;
	}

	mutex_unlock(&priv->i2c_lock);

	return ret;
}

static int it930x_read_firmware_version(struct it930x_bridge *it930x, u32 *fw_version)
{
	int ret = 0;
	u8 buf[4];
	struct it930x_ctrl_buf wb, rb;

	buf[0] = 1;

	wb.buf = buf;
	wb.len = 1;

	rb.buf = buf;
	rb.len = 4;

	ret = it930x_ctrl_msg(it930x, IT930X_CMD_QUERYINFO, &wb, &rb, NULL, false);
	if (!ret)
		*fw_version = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];

	return ret;
}

static int it930x_config_i2c(struct it930x_bridge *it930x)
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

	ret = it930x_write_reg(it930x, 0xf6a7, it930x->config.i2c_speed);
	if (ret)
		return ret;

	ret = it930x_write_reg(it930x, 0xf103, it930x->config.i2c_speed);
	if (ret)
		return ret;

	// set i2c address and bus

	for(i = 0; i < 5; i++) {
		struct it930x_stream_input *input = &it930x->config.input[i];

		if (input->enable) {
			ret = it930x_write_reg(it930x, i2c_regs[input->slave_number][0], input->i2c_addr);
			if (ret)
				break;

			ret = it930x_write_reg(it930x, i2c_regs[input->slave_number][1], input->i2c_bus);
			if (ret)
				break;
		}
	}

	return ret;
}

static int it930x_config_stream_input(struct it930x_bridge *it930x)
{
	int ret = 0, i;

	for (i = 0; i < 5; i++) {
		struct it930x_stream_input *input = &it930x->config.input[i];

		if (!input->enable) {
			// disable input port
			ret = it930x_write_reg(it930x, 0xda4c + input->port_number, 0);
			if (ret)
				break;

			continue;
		}

		if (input->port_number < 2) {
			ret = it930x_write_reg(it930x, 0xda58 + input->port_number, (input->is_parallel) ? 1 : 0);
			if (ret)
				break;
		}

		// aggregation mode: sync byte
		ret = it930x_write_reg(it930x, 0xda73 + input->port_number, 1);
		if (ret)
			break;

		// write sync byte
		ret = it930x_write_reg(it930x, 0xda78 + input->port_number, input->sync_byte);
		if (ret)
			break;

		// enable input port
		ret = it930x_write_reg(it930x, 0xda4c + input->port_number, 1);
		if (ret)
			break;
	}

	return ret;
}

static int it930x_config_stream_output(struct it930x_bridge *it930x)
{
	int ret = 0, ret2 = 0;

	ret = it930x_write_reg_mask(it930x, 0xda1d, 0x01, 0x01);
	if (ret)
		return ret;

	switch (it930x->bus.type) {
	case ITEDTV_BUS_USB:
	{
		u16 x;
		u8 buf[2];

		// disable ep
		ret = it930x_write_reg_mask(it930x, 0xdd11, 0x00, 0x20);
		if (ret)
			goto exit;

		// disable nak
		ret = it930x_write_reg_mask(it930x, 0xdd13, 0x00, 0x20);
		if (ret)
			goto exit;

		// enable ep
		ret = it930x_write_reg_mask(it930x, 0xdd11, 0x20, 0x20);
		if (ret)
			goto exit;

		x = ((it930x->config.xfer_size / 4) & 0xffff);

		buf[0] = (x & 0xff);
		buf[1] = ((x >> 8) & 0xff);

		// transfer size
		ret = it930x_write_regs(it930x, 0xdd88, buf, 2);
		if (ret)
			goto exit;

		// max packet size
		ret = it930x_write_reg(it930x, 0xdd0c, (it930x->bus.usb.max_bulk_size / 4) & 0xff);
		if (ret)
			goto exit;

		ret = it930x_write_reg_mask(it930x, 0xda05, 0x00, 0x01);
		if (ret)
			goto exit;

		ret = it930x_write_reg_mask(it930x, 0xda06, 0x00, 0x01);
		if (ret)
			goto exit;

		break;
	}

	default:
		break;
	}

exit:
	ret2 = it930x_write_reg_mask(it930x, 0xda1d, 0x00, 0x01);

	// reverse: no
	ret2 = it930x_write_reg(it930x, 0xd920, 0);

	return (ret) ? ret : ret2;
}

int it930x_init(struct it930x_bridge *it930x)
{
	int ret = 0;
	struct it930x_priv *priv;
	u8 *buf = NULL;
	int i;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto fail;
	}

	buf = kmalloc(sizeof(u8) * 256, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto fail;
	}

	mutex_init(&priv->ctrl_lock);
	mutex_init(&priv->i2c_lock);
	mutex_init(&priv->gpio_lock);

	priv->buf = buf;

	// setup the i2c operator

	for (i = 0; i < 3; i++) {
		priv->i2c[i].it930x = it930x;
		priv->i2c[i].bus = i + 1;

		it930x->i2c_master[i].request = it930x_i2c_master_request;
		it930x->i2c_master[i].priv = &priv->i2c[i];
	}

	it930x->priv = priv;

	return 0;

fail:
	if (buf)
		kfree(buf);

	if (priv)
		kfree(priv);

	return ret;
}

int it930x_term(struct it930x_bridge *it930x)
{
	int i;
	struct it930x_priv *priv = it930x->priv;

	if (priv->buf)
		kfree(priv->buf);

	// clear the i2c operator

	for (i = 0; i < 3; i++) {
		it930x->i2c_master[i].request = NULL;
		it930x->i2c_master[i].priv = NULL;
	}

	mutex_destroy(&priv->ctrl_lock);
	mutex_destroy(&priv->i2c_lock);
	mutex_destroy(&priv->gpio_lock);

	kfree(priv);

	it930x->priv = NULL;

	return 0;
}

int it930x_load_firmware(struct it930x_bridge *it930x, const char *filename)
{
	int ret = 0;
	u32 fw_version;
	const struct firmware *fw;
	size_t i, n, len = 0;
	struct it930x_ctrl_buf wb;

	if (!filename)
		return -EINVAL;

	ret = it930x_read_firmware_version(it930x, &fw_version);
	if (ret) {
		dev_err(it930x->dev, "it930x_load_firmware: it930x_read_firmware_version() failed. 1 (ret: %d)\n", ret);
		return ret;
	}

	if (fw_version) {
		dev_info(it930x->dev, "Firmware is already loaded. version: %d.%d.%d.%d\n", ((fw_version >> 24) & 0xff), ((fw_version >> 16) & 0xff), ((fw_version >> 8) & 0xff), (fw_version & 0xff));
		return ret;
	}

	ret = it930x_write_reg(it930x, 0xf103, it930x->config.i2c_speed);
	if (ret) {
		dev_err(it930x->dev, "it930x_load_firmware: it930x_write_reg(0xf103) failed. (ret: %d)\n", ret);
		return ret;
	}

	ret = request_firmware(&fw, filename, it930x->dev);
	if (ret) {
		dev_err(it930x->dev, "it930x_load_firmware: request_firmware() failed. (ret: %d)\n", ret);
		dev_err(it930x->dev, "Couldn't load firmware from the file.\n");
		return ret;
	}

	n = fw->size;

	for(i = 0; i < n; i += len) {
		const u8 *p = &fw->data[i];
		unsigned j, m = p[3];

		len = 0;

		if (p[0] != 0x03) {
			dev_err(it930x->dev, "it930x_load_firmware: Invalid firmware block was found. Abort. (ofs: %zx)\n", i);
			ret = -ECANCELED;
			goto exit_release_fw;
		}

		for(j = 0; j < m; j++)
			len += p[6 + (j * 3)];

		if (!len) {
			dev_warn(it930x->dev, "it930x_load_firmware: No data in the block. (ofs: %zx)\n", i);
			continue;
		}

		len += 4 + (m * 3);

		// send firmware block

		wb.buf = (u8 *)p;
		wb.len = (u8)len;

		ret = it930x_ctrl_msg(it930x, IT930X_CMD_FW_SCATTER_WRITE, &wb, NULL, NULL, false);
		if (ret) {
			dev_err(it930x->dev, "it930x_load_firmware: it930x_ctrl_msg(IT930X_CMD_FW_SCATTER_WRITE) failed. (ofs: %zx, ret: %d)\n", i, ret);
			goto exit_release_fw;
		}
	}

	ret = it930x_ctrl_msg(it930x, IT930X_CMD_BOOT, NULL, NULL, NULL, false);
	if (ret) {
		dev_err(it930x->dev, "it930x_load_firmware: it930x_ctrl_msg(IT930X_CMD_BOOT) failed. (ret: %d)\n", ret);
		goto exit_release_fw;
	}

	ret = it930x_read_firmware_version(it930x, &fw_version);
	if (ret) {
		dev_err(it930x->dev, "it930x_load_firmware: it930x_read_firmware_version() failed. 2 (ret: %d)\n", ret);
		goto exit_release_fw;
	}

	if (!fw_version) {
		ret = -EIO;
		goto exit_release_fw;
	}

	dev_info(it930x->dev, "Firmware loaded. version: %d.%d.%d.%d\n", ((fw_version >> 24) & 0xff), ((fw_version >> 16) & 0xff), ((fw_version >> 8) & 0xff), (fw_version & 0xff));

exit_release_fw:
	release_firmware(fw);

	return ret;
}

int it930x_init_warm(struct it930x_bridge *it930x)
{
	int ret = 0;

	if (it930x->bus.type != ITEDTV_BUS_USB) {
		dev_dbg(it930x->dev, "it930x_init_warm: This driver only supports USB.\n");
		return -EINVAL;
	}

	ret = it930x_write_reg(it930x, 0x4976, 0);
	if (ret)
		return ret;

	ret = it930x_write_reg(it930x, 0x4bfb, 0);
	if (ret)
		return ret;

	ret = it930x_write_reg(it930x, 0x4978, 0);
	if (ret)
		return ret;

	ret = it930x_write_reg(it930x, 0x4977, 0);
	if (ret)
		return ret;

	// ignore sync byte: no
	ret = it930x_write_reg(it930x, 0xda1a, 0);
	if (ret)
		return ret;

	// dvb-t interrupt: enable
	ret = it930x_write_reg_mask(it930x, 0xf41f, 0x04, 0x04);
	if (ret)
		return ret;

	// mpeg full speed
	ret = it930x_write_reg_mask(it930x, 0xda10, 0x00, 0x01);
	if (ret)
		return ret;

	// dvb-t mode: enable
	ret = it930x_write_reg_mask(it930x, 0xf41a, 0x01, 0x01);
	if (ret)
		return ret;

	ret = it930x_config_stream_output(it930x);
	if (ret) {
		dev_err(it930x->dev, "it930x_init_warm: it930x_config_stream_output() failed. (ret: %d)\n", ret);
		return ret;
	}

	// power config ?

	ret = it930x_write_reg(it930x, 0xd833, 1);
	if (ret)
		return ret;

	ret = it930x_write_reg(it930x, 0xd830, 0);
	if (ret)
		return ret;

	ret = it930x_write_reg(it930x, 0xd831, 1);
	if (ret)
		return ret;

	ret = it930x_write_reg(it930x, 0xd832, 0);
	if (ret)
		return ret;

	ret = it930x_config_i2c(it930x);
	if (ret) {
		dev_err(it930x->dev, "it930x_init_warm: _it930x_config_i2c() failed. (ret: %d)\n", ret);
		return ret;
	}

	ret = it930x_config_stream_input(it930x);
	if (ret) {
		dev_err(it930x->dev, "it930x_init_warm: _it930x_config_stream_input() failed. (ret: %d)\n", ret);
		return ret;
	}

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
	int ret = 0;
	struct it930x_priv *priv = it930x->priv;
	u8 val;

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

	mutex_lock(&priv->gpio_lock);

	if (priv->status[gpio].mode == mode)
		goto exit;

	priv->status[gpio].mode = mode;

	ret = it930x_write_reg(it930x, gpio_en_regs[gpio], val);
	if (ret)
		goto exit;

	if (!enable || priv->status[gpio].enable)
		goto exit;

	priv->status[gpio].enable = true;

	ret = it930x_write_reg(it930x, gpio_en_regs[gpio] + 1, 1);

exit:
	mutex_unlock(&priv->gpio_lock);

	return ret;
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
	int ret = 0;
	struct it930x_priv *priv = it930x->priv;

	if (gpio <= 0 || gpio > (sizeof(gpio_on_regs) / sizeof(gpio_on_regs[0])))
		return -EINVAL;

	gpio--;

	mutex_lock(&priv->gpio_lock);

	if ((!priv->status[gpio].enable && !enable) || (priv->status[gpio].enable && enable))
		goto exit;

	priv->status[gpio].enable = (enable) ? true : false;

	ret = it930x_write_reg(it930x, gpio_on_regs[gpio], (enable) ? 1 : 0);

exit:
	mutex_unlock(&priv->gpio_lock);

	return ret;
}

int it930x_read_gpio(struct it930x_bridge *it930x, int gpio, bool *high)
{
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
	int ret = 0;
	struct it930x_priv *priv = it930x->priv;
	u8 tmp;

	if (gpio <= 0 || gpio > (sizeof(gpio_i_regs) / sizeof(gpio_i_regs[0])))
		return -EINVAL;

	gpio--;

	mutex_lock(&priv->gpio_lock);

	if (priv->status[gpio].mode == IT930X_GPIO_IN) {
		ret = it930x_read_reg(it930x, gpio_i_regs[gpio], &tmp);
		if (!ret)
			*high = (tmp) ? true : false;
	} else
		ret = -EINVAL;

	mutex_unlock(&priv->gpio_lock);

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
	int ret = 0;
	struct it930x_priv *priv = it930x->priv;

	if (gpio <= 0 || gpio > (sizeof(gpio_o_regs) / sizeof(gpio_o_regs[0])))
		return -EINVAL;

	gpio--;

	mutex_lock(&priv->gpio_lock);

	if (priv->status[gpio].mode == IT930X_GPIO_OUT)
		ret = it930x_write_reg(it930x, gpio_o_regs[gpio], (high) ? 1 : 0);
	else
		ret = -EINVAL;

	mutex_unlock(&priv->gpio_lock);

	return ret;
}

int it930x_purge_psb(struct it930x_bridge *it930x, int timeout)
{
	int ret = 0;
	void *p;
	int len;

	if (it930x->bus.type != ITEDTV_BUS_USB)
		return -EINVAL;

	ret = it930x_write_reg_mask(it930x, 0xda1d, 0x01, 0x01);
	if (ret)
		return ret;

	len = 1024;

	p = kmalloc(len, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	ret = itedtv_bus_stream_rx(&it930x->bus, p, &len, timeout);
	kfree(p);

	it930x_write_reg_mask(it930x, 0xda1d, 0x00, 0x01);

	if (ret)
		dev_dbg(it930x->dev, "it930x_purge_psb: itedtv_bus_stream_rx() returned error code %d.\n", ret);

	dev_dbg(it930x->dev, "it930x_purge_psb: len: %d\n", len);

	if (len == 512)
		ret = 0;

	return ret;
}
