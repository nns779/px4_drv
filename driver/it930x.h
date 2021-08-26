// SPDX-License-Identifier: GPL-2.0-only
/*
 * ITE IT930x driver definitions (it930x.h)
 *
 * Copyright (c) 2018-2021 nns779
 */

#ifndef __IT930X_H__
#define __IT930X_H__

#ifdef __linux__
#include <linux/types.h>
#include <linux/device.h>
#elif defined(_WIN32) || defined(_WIN64)
#include "misc_win.h"
#endif

#include "itedtv_bus.h"
#include "i2c_comm.h"

#define IT930X_CMD_REG_READ		0x00
#define IT930X_CMD_REG_WRITE		0x01
#define IT930X_CMD_QUERYINFO		0x22
#define IT930X_CMD_BOOT			0x23
#define IT930X_CMD_FW_SCATTER_WRITE	0x29
#define IT930X_CMD_I2C_READ		0x2a
#define IT930X_CMD_I2C_WRITE		0x2b

enum it930x_gpio_mode {
	IT930X_GPIO_UNDEFINED = 0,
	IT930X_GPIO_IN,
	IT930X_GPIO_OUT,
};

struct it930x_pid_filter {
	bool block;
	int num;
	u16 pid[64];
};

struct it930x_stream_input {
	bool enable;
	bool is_parallel;
	u8 port_number;		// internal port number
	u8 slave_number;
	u8 i2c_bus;
	u8 i2c_addr;
	u8 packet_len;
	u8 sync_byte;
};

struct it930x_config {
	u32 xfer_size;
	u8 i2c_speed;
	struct it930x_stream_input input[5];
};

struct it930x_bridge {
	struct device *dev;
	struct itedtv_bus bus;
	struct it930x_config config;
	struct i2c_comm_master i2c_master[3];
	void *priv;
};

#ifdef __cplusplus
extern "C" {
#endif
int it930x_read_regs(struct it930x_bridge *it930x,
		     u32 reg,
		     u8 *rbuf, u8 len);
int it930x_read_reg(struct it930x_bridge *it930x, u32 reg, u8 *val);
int it930x_write_regs(struct it930x_bridge *it930x,
		      u32 reg,
		      u8 *wbuf, u8 len);
int it930x_write_reg(struct it930x_bridge *it930x, u32 reg, u8 val);
int it930x_write_reg_mask(struct it930x_bridge *it930x,
			  u32 reg,
			  u8 val, u8 mask);

int it930x_init(struct it930x_bridge *it930x);
int it930x_term(struct it930x_bridge *it930x);

int it930x_raise(struct it930x_bridge *it930x);
int it930x_load_firmware(struct it930x_bridge *it930x, const char *filename);
int it930x_init_warm(struct it930x_bridge *it930x);
int it930x_set_gpio_mode(struct it930x_bridge *it930x,
			 int gpio,
			 enum it930x_gpio_mode mode,
			 bool enable);
int it930x_enable_gpio(struct it930x_bridge *it930x, int gpio, bool enable);
int it930x_read_gpio(struct it930x_bridge *it930x, int gpio, bool *high);
int it930x_write_gpio(struct it930x_bridge *it930x, int gpio, bool high);
int it930x_set_pid_filter(struct it930x_bridge *it930x, int input_idx,
			  struct it930x_pid_filter *filter);
int it930x_purge_psb(struct it930x_bridge *it930x, int timeout);
#ifdef __cplusplus
}
#endif

#endif
