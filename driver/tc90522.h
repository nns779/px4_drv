// tc90522.h

#ifndef __TC90522_H__
#define __TC90522_H__

#include <linux/types.h>

#include "i2c_comm.h"

struct tc90522_demod {
	struct i2c_comm_master *i2c;
	u8 i2c_addr;
	struct i2c_comm_master i2c_master;
};

struct tc90522_regbuf {
	u8 reg;
	u8 *buf;
	union {
		u8 val;
		u8 len;
	} u;
};

static inline void tc90522_regbuf_set_val(struct tc90522_regbuf *regbuf, u8 reg, u8 val)
{
	regbuf->reg = reg;
	regbuf->buf = NULL;
	regbuf->u.val = val;
}

static inline void tc90522_regbuf_set_buf(struct tc90522_regbuf *regbuf, u8 reg, u8 *buf, u8 len)
{
	regbuf->reg = reg;
	regbuf->buf = buf;
	regbuf->u.len = len;
}

int tc90522_write_regs(struct tc90522_demod *demod, struct tc90522_regbuf *regbuf, int num);
int tc90522_write_reg(struct tc90522_demod *demod, u8 reg, u8 val);
int tc90522_read_regs(struct tc90522_demod *demod, struct tc90522_regbuf *regbuf, int num);
int tc90522_read_reg(struct tc90522_demod *demod, u8 reg, u8 *val);

int tc90522_init(struct tc90522_demod *demod);

int tc90522_sleep_s(struct tc90522_demod *demod, bool sleep);
int tc90522_set_agc_s(struct tc90522_demod *demod, bool on);
int tc90522_tmcc_get_tsid_s(struct tc90522_demod *demod, u8 idx, u16 *tsid);
int tc90522_set_tsid_s(struct tc90522_demod *demod, u16 tsid);
int tc90522_get_tsid_s(struct tc90522_demod *demod, u16 *tsid);
int tc90522_get_cn_s(struct tc90522_demod *demod, u16 *cn);
int tc90522_enable_ts_pins_s(struct tc90522_demod *demod, bool e);
int tc90522_is_signal_locked_s(struct tc90522_demod *demod, bool *lock);

int tc90522_sleep_t(struct tc90522_demod *demod, bool sleep);
int tc90522_set_agc_t(struct tc90522_demod *demod, bool on);
int tc90522_get_cndat_t(struct tc90522_demod *demod, u32 *cndat);
int tc90522_enable_ts_pins_t(struct tc90522_demod *demod, bool e);
int tc90522_is_signal_locked_t(struct tc90522_demod *demod, bool *lock);

#endif
