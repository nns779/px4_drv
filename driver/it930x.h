// it930x.h

#ifndef __IT930X_H__
#define __IT930X_H__

#include <linux/types.h>
#include <linux/device.h>

#include "it930x-config.h"
#include "it930x-bus.h"
#include "i2c_comm.h"

#define IT930X_CMD_REG_READ		0x00
#define IT930X_CMD_REG_WRITE		0x01
#define IT930X_CMD_QUERYINFO		0x22
#define IT930X_CMD_BOOT			0x23
#define IT930X_CMD_FW_SCATTER_WRITE	0x29
#define IT930X_CMD_I2C_READ		0x2a
#define IT930X_CMD_I2C_WRITE		0x2b

struct it930x_i2c_master_info {
	struct it930x_bridge *it930x;
	u8 bus;
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

struct it930x_bridge {
	struct device *dev;
	struct it930x_bus bus;
	u32 fw_version;
	struct it930x_stream_input input[5];
	struct it930x_i2c_master_info i2c[2];
	struct i2c_comm_master i2c_master[2];
	u8 buf[255];
	u8 sequence;
};

struct it930x_regbuf {
	u32 reg;
	u8 *buf;
	union {
		u8 val;	// buf == NULL (write only)
		u8 len;	// buf != NULL
	} u;
};

static inline void it930x_regbuf_set_val(struct it930x_regbuf *regbuf, u32 reg, u8 val)
{
	regbuf->reg = reg;
	regbuf->buf = NULL;
	regbuf->u.val = val;
}

static inline void it930x_regbuf_set_buf(struct it930x_regbuf *regbuf, u32 reg, u8 *buf, u8 len)
{
	regbuf->reg = reg;
	regbuf->buf = buf;
	regbuf->u.len = len;
}

int it930x_write_regs(struct it930x_bridge *it930x, struct it930x_regbuf *regbuf, int num_regbuf);
int it930x_write_reg(struct it930x_bridge *it930x, u32 reg, u8 val);
int it930x_write_reg_bits(struct it930x_bridge *it930x, u32 reg, u8 val, u8 pos, u8 len);
int it930x_read_regs(struct it930x_bridge *it930x, struct it930x_regbuf *regbuf, int num_regbuf);
int it930x_read_reg(struct it930x_bridge *it930x, u32 reg, u8 *val);

int it930x_init(struct it930x_bridge *it930x);
int it930x_term(struct it930x_bridge *it930x);

int it930x_load_firmware(struct it930x_bridge *it930x, const char *filename);
int it930x_init_device(struct it930x_bridge *it930x);
int it930x_set_gpio(struct it930x_bridge *it930x, int gpio, bool h);
int it930x_enable_stream_input(struct it930x_bridge *it930x, u8 input_idx, bool enable);
int it930x_purge_psb(struct it930x_bridge *it930x);

#endif
