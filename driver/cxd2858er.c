// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony CXD2858ER driver (cxd2858er.c)
 *
 * Copyright (c) 2018-2021 nns779
 */

#include "print_format.h"
#include "cxd2858er.h"

#ifdef __linux__
#include <linux/delay.h>
#endif

static int cxd2858er_stop_t(struct cxd2858er_tuner *tuner);
static int cxd2858er_stop_s(struct cxd2858er_tuner *tuner);

static int cxd2858er_read_regs(struct cxd2858er_tuner *tuner,
			       u8 reg, u8 *buf, int len)
{
	u8 b;
	struct i2c_comm_request req[2];

	if (!buf || !len)
		return -EINVAL;

	b = reg;

	req[0].req = I2C_WRITE_REQUEST;
	req[0].addr = tuner->i2c_addr;
	req[0].data = &b;
	req[0].len = 1;

	req[1].req = I2C_READ_REQUEST;
	req[1].addr = tuner->i2c_addr;
	req[1].data = buf;
	req[1].len = len;

	return i2c_comm_master_request(tuner->i2c, req, 2);
}

static int cxd2858er_read_reg(struct cxd2858er_tuner *tuner,
			      u8 reg, u8 *val)
{
	return cxd2858er_read_regs(tuner, reg, val, 1);
}

static int cxd2858er_write_regs(struct cxd2858er_tuner *tuner,
				u8 reg, u8 *buf, int len)
{
	u8 b[255];
	struct i2c_comm_request req[1];

	if (!buf || !len || len > 254)
		return -EINVAL;

	b[0] = reg;
	memcpy(&b[1], buf, len);

	req[0].req = I2C_WRITE_REQUEST;
	req[0].addr = tuner->i2c_addr;
	req[0].data = b;
	req[0].len = 1 + len;

	return i2c_comm_master_request(tuner->i2c, req, 1);
}

static int cxd2858er_write_reg(struct cxd2858er_tuner *tuner,
			       u8 reg, u8 val)
{
	return cxd2858er_write_regs(tuner, reg, &val, 1);
}

static int cxd2858er_write_reg_mask(struct cxd2858er_tuner *tuner,
				    u8 reg, u8 val, u8 mask)
{
	int ret = 0;
	u8 tmp;

	if (!mask)
		return -EINVAL;

	if (mask != 0xff) {
		ret = cxd2858er_read_regs(tuner, reg, &tmp, 1);
		if (ret)
			return ret;

		tmp &= ~mask;
		tmp |= (val & mask);
	} else {
		tmp = val;
	}

	return cxd2858er_write_regs(tuner, reg, &tmp, 1);
}

static int cxd2858er_power_on(struct cxd2858er_tuner *tuner)
{
	int ret = 0;
	u8 data[20], tmp;

	/* T mode */
	ret = cxd2858er_write_reg(tuner, 0x01, 0x00);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x67, 0x00);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x43,
				  0x05 | ((tuner->config.ter.lna) ? 0x02
								  : 0x00));
	if (ret)
		return ret;

	data[0] = 0x15;
	data[1] = 0x00;
	data[2] = 0x00;

	ret = cxd2858er_write_regs(tuner, 0x5e, data, 3);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x0c, 0x14);
	if (ret)
		return ret;

	data[0] = 0x7a;
	data[1] = 0x01;

	ret = cxd2858er_write_regs(tuner, 0x99, data, 2);
	if (ret)
		return ret;

	switch (tuner->config.xtal) {
	case 16000:
		data[0] = 0x10;
		break;

	case 24000:
		data[0] = 0x18;
		break;

	default:
		return -EINVAL;
	}

	data[1] = 0x80 | (0x04 & 0x1f);
	data[2] = 0x80 | 0x26;
	data[3] = 0x00;
	data[4] = 0x00;
	data[5] = 0x00;
	data[6] = 0xc4;
	data[7] = 0x40;
	data[8] = 0x10;
	data[9] = 0x00;
	data[10] = 0x45;
	data[11] = 0x75;
	data[12] = 0x07;
	data[13] = 0x1c;
	data[14] = 0x3f;
	data[15] = 0x02;
	data[16] = 0x10;
	data[17] = 0x20;
	data[18] = 0x0a;
	data[19] = 0x00;

	ret = cxd2858er_write_regs(tuner, 0x81, data, 20);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x9b, 0x00);
	if (ret)
		return ret;

	msleep(10);

	ret = cxd2858er_read_reg(tuner, 0x1a, &tmp);
	if (ret)
		return ret;

	if (tmp != 0x00)
		return -EIO;

	data[0] = 0x90;
	data[1] = 0x06;

	ret = cxd2858er_write_regs(tuner, 0x17, data, 2);
	if (ret)
		return ret;

	msleep(1);

	ret = cxd2858er_read_reg(tuner, 0x19, &tmp);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x95, (tmp & 0xf0) >> 4);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x74, 0x02);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x88, 0x00);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x87, 0xc0);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x80, 0x01);
	if (ret)
		return ret;

	data[0] = 0x07;
	data[1] = 0x00;

	ret = cxd2858er_write_regs(tuner, 0x41, data, 2);
	if (ret)
		return ret;

	return 0;
}

int cxd2858er_init(struct cxd2858er_tuner *tuner)
{
	int ret = 0;

	if (!tuner->dev || !tuner->i2c || !tuner->i2c_addr)
		return -EINVAL;

	if (tuner->config.xtal != 16000 && tuner->config.xtal != 24000)
		return -EINVAL;

	tuner->system = CXD2858ER_UNSPECIFIED_SYSTEM;

	ret = i2c_comm_master_gate_ctrl(tuner->i2c, true);
	if (ret)
		return ret;

	ret = cxd2858er_power_on(tuner);

	i2c_comm_master_gate_ctrl(tuner->i2c, false);

	return ret;
}

int cxd2858er_term(struct cxd2858er_tuner *tuner)
{
	int ret = 0;

	if (tuner->system == CXD2858ER_UNSPECIFIED_SYSTEM)
		return 0;

	ret = i2c_comm_master_gate_ctrl(tuner->i2c, true);
	if (!ret) {
		switch (tuner->system) {
		case CXD2858ER_ISDB_T_SYSTEM:
			cxd2858er_stop_t(tuner);
			break;

		case CXD2858ER_ISDB_S_SYSTEM:
			cxd2858er_stop_s(tuner);
			break;

		default:
			break;
		}

		i2c_comm_master_gate_ctrl(tuner->i2c, false);
	} else {
		tuner->system = CXD2858ER_UNSPECIFIED_SYSTEM;
	}

	return 0;
}

int cxd2858er_set_params_t(struct cxd2858er_tuner *tuner,
			   enum cxd2858er_system system,
			   u32 freq, u32 bandwidth)
{
	int ret = 0;
	u8 data[17];

	if (system != CXD2858ER_ISDB_T_SYSTEM)
		return -EINVAL;

	ret = i2c_comm_master_gate_ctrl(tuner->i2c, true);
	if (ret)
		return ret;

	switch (tuner->system) {
	case CXD2858ER_ISDB_S_SYSTEM:
		ret = cxd2858er_stop_s(tuner);
		break;

	default:
		break;
	}

	if (ret)
		goto exit;

	/* T mode */
	ret = cxd2858er_write_reg(tuner, 0x01, 0x00);
	if (ret)
		goto exit;

	ret = cxd2858er_write_reg(tuner, 0x74, 0x02);
	if (ret)
		goto exit;

	data[0] = 0xc4;
	data[1] = 0x40;

	ret = cxd2858er_write_regs(tuner, 0x87, data, 2);
	if (ret)
		goto exit;

	data[0] = 0x10;
	data[1] = 0x20;

	ret = cxd2858er_write_regs(tuner, 0x91, data, 2);
	if (ret)
		goto exit;

	data[0] = 0x00;
	data[1] = 0x00;

	ret = cxd2858er_write_regs(tuner, 0x9c, data, 2);
	if (ret)
		goto exit;

	data[0] = 0xee;
	data[1] = 0x02;
	data[2] = 0x1e;
	data[3] = 0x67;

	switch (tuner->config.xtal) {
	case 16000:
		data[4] = 0x02;
		break;

	case 24000:
		data[4] = 0x03;
		break;

	default:
		ret = -EINVAL;
		goto exit;
	}

	data[5] = 0xb4;
	data[6] = 0x78;
	data[7] = 0x08;
	data[8] = 0x30;

	ret = cxd2858er_write_regs(tuner, 0x5e, data, 9);
	if (ret)
		goto exit;

	ret = cxd2858er_write_reg_mask(tuner, 0x67, 0x00, 0x02);
	if (ret)
		goto exit;

	data[0] = 0x00;
	data[1] = 0x88;
	data[2] = 0x00;
	data[3] = 0x0b;
	data[4] = 0x22;
	data[5] = 0x00;
	data[6] = 0x17;
	data[7] = 0x1b;

	data[8] = freq & 0xff;
	data[9] = (freq >> 8) & 0xff;
	data[10] = (freq >> 16) & 0x0f;

	data[11] = 0xff;
	data[12] = 0x01;
	data[13] = 0x99;
	data[14] = 0x00;
	data[15] = 0x24;
	data[16] = 0x87;

	ret = cxd2858er_write_regs(tuner, 0x68, data, 17);
	if (ret)
		goto exit;

	msleep(50);

	ret = cxd2858er_write_reg(tuner, 0x88, 0x00);
	if (ret)
		goto exit;

	ret = cxd2858er_write_reg(tuner, 0x87, 0xc0);
	if (ret)
		goto exit;

	tuner->system = system;

exit:
	i2c_comm_master_gate_ctrl(tuner->i2c, false);
	return ret;
}

int cxd2858er_set_params_s(struct cxd2858er_tuner *tuner,
			   enum cxd2858er_system system,
			   u32 freq, u32 symbol_rate)
{
	int ret = 0;
	u8 data[18];

	if (system != CXD2858ER_ISDB_S_SYSTEM)
		return -EINVAL;

	ret = i2c_comm_master_gate_ctrl(tuner->i2c, true);
	if (ret)
		return ret;

	switch (tuner->system) {
	case CXD2858ER_ISDB_T_SYSTEM:
		ret = cxd2858er_stop_t(tuner);
		break;

	default:
		break;
	}

	if (ret)
		goto exit;

	ret = cxd2858er_write_reg(tuner, 0x15, 0x02);
	if (ret)
		goto exit;

	ret = cxd2858er_write_reg(tuner, 0x43, 0x06);
	if (ret)
		goto exit;

	data[0] = 0x00;
	data[1] = 0x00;

	ret = cxd2858er_write_regs(tuner, 0x6a, data, 2);
	if (ret)
		goto exit;

	ret = cxd2858er_write_reg(tuner, 0x75, 0x99);
	if (ret)
		goto exit;

	ret = cxd2858er_write_reg(tuner, 0x9d, 0x00);
	if (ret)
		goto exit;

	ret = cxd2858er_write_reg(tuner, 0x61, 0x07);
	if (ret)
		goto exit;

	/* S mode */
	ret = cxd2858er_write_reg(tuner, 0x01, 0x01);
	if (ret)
		goto exit;

	data[0] = 0xc4;
	data[1] = 0x40;

	switch (tuner->config.xtal) {
	case 16000:
		data[2] = 0x02;
		break;

	case 24000:
		data[2] = 0x03;
		break;

	default:
		ret = -EINVAL;
		goto exit;
	}

	data[3] = 0x00;
	data[4] = 0xb4;
	data[5] = 0x78;
	data[6] = 0x08;
	data[7] = 0x30;
	data[8] = 0xfe | ((tuner->config.sat.lna) ? 0x01 : 0x00);
	data[9] = 0x02;
	data[10] = 0x1e;

	switch (system) {
	case CXD2858ER_ISDB_S_SYSTEM:
		data[11] = 0x16;
		break;

	default:
		ret = -EINVAL;
		goto exit;
	}

	freq = (freq + 2) / 4;

	data[12] = freq & 0xff;
	data[13] = (freq >> 8) & 0xff;
	data[14] = (freq >> 16) & 0x0f;

	data[15] = 0xff;
	data[16] = 0x00;
	data[17] = 0x01;

	ret = cxd2858er_write_regs(tuner, 0x04, data, 18);
	if (ret)
		goto exit;

	msleep(10);

	ret = cxd2858er_write_reg(tuner, 0x05, 0x00);
	if (ret)
		goto exit;

	ret = cxd2858er_write_reg(tuner, 0x04, 0xc0);
	if (ret)
		goto exit;

	tuner->system = system;

exit:
	i2c_comm_master_gate_ctrl(tuner->i2c, false);
	return ret;
}

static int cxd2858er_stop_t(struct cxd2858er_tuner *tuner)
{
	int ret = 0;
	u8 data[3];

	if (tuner->system != CXD2858ER_ISDB_T_SYSTEM)
		return -EINVAL;

	ret = cxd2858er_write_reg(tuner, 0x74, 0x02);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg_mask(tuner, 0x67, 0x00, 0xfe);
	if (ret)
		return ret;

	data[0] = 0x15;
	data[1] = 0x00;
	data[2] = 0x00;

	ret = cxd2858er_write_regs(tuner, 0x5e, data, 3);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x88, 0x00);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x87, 0xc0);
	if (ret)
		return ret;

	tuner->system = CXD2858ER_UNSPECIFIED_SYSTEM;
	return 0;
}

static int cxd2858er_stop_s(struct cxd2858er_tuner *tuner)
{
	int ret = 0;
	u8 data[3];

	if (tuner->system != CXD2858ER_ISDB_S_SYSTEM)
		return -EINVAL;

	ret = cxd2858er_write_reg(tuner, 0x15, 0x02);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x43,
				  0x05 | ((tuner->config.ter.lna) ? 0x02
								  : 0x00));
	if (ret)
		return ret;

	data[0] = 0x14;
	data[1] = 0x00;
	data[2] = 0x00;

	ret = cxd2858er_write_regs(tuner, 0x0c, data, 3);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x01, 0x00);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x05, 0x00);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x04, 0xc0);
	if (ret)
		return ret;

	tuner->system = CXD2858ER_UNSPECIFIED_SYSTEM;
	return 0;
}

int cxd2858er_stop(struct cxd2858er_tuner *tuner)
{
	int ret = 0;

	if (tuner->system == CXD2858ER_UNSPECIFIED_SYSTEM)
		return -EALREADY;

	ret = i2c_comm_master_gate_ctrl(tuner->i2c, true);
	if (ret)
		return ret;

	switch (tuner->system) {
	case CXD2858ER_ISDB_T_SYSTEM:
		cxd2858er_stop_t(tuner);
		break;

	case CXD2858ER_ISDB_S_SYSTEM:
		cxd2858er_stop_s(tuner);
		break;

	default:
		break;
	}

	i2c_comm_master_gate_ctrl(tuner->i2c, false);
	return 0;
}
