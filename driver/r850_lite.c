// r850_lite.c

// Rafael Micro R850 driver (lite version)

// Some features are not implemented.

#include "print_format.h"

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/device.h>

#include "r850_lite.h"

/* Some versions, the first 8 bytes are zero. */
static const u8 init_regs[R850_NUM_REGS] = {
	0x35, 0xc8, 0x0f, 0x80, 0xfa, 0xff, 0xff, 0xf0,
	0xc0, 0x49, 0x3a, 0x90, 0x03, 0xc1, 0x61, 0x71,
	0x17, 0xf1, 0x18, 0x55, 0x30, 0x20, 0xf3, 0xed,
	0x1f, 0x1c, 0x81, 0x13, 0x00, 0x80, 0x0a, 0x07,
	0x21, 0x71, 0x54, 0xf1, 0xf2, 0xa9, 0xbb, 0x0b,
	0xa3, 0xf6, 0x0b, 0x44, 0x92, 0x17, 0xe6, 0x80
};

static u8 reverse_bit(u8 val)
{
	u8 t = val;

	t = (t & 0x55) << 1 | (t & 0xaa) >> 1;
	t = (t & 0x33) << 2 | (t & 0xcc) >> 2;
	t = (t & 0x0f) << 4 | (t & 0xf0) >> 4;

	return t;
}

static int _r850_write_regs(struct r850_tuner *t, u8 reg, const u8 *buf, int len)
{
	int ret = 0;
	u8 b[1 + R850_NUM_REGS];

	if (!t || !buf || !len)
		return -EINVAL;

	if (len > (R850_NUM_REGS - reg))
		return -EINVAL;

	b[0] = reg;
	memcpy(&b[1], buf, len);

	ret = i2c_comm_master_lock(t->i2c);
	if (ret)
		return ret;

	ret = i2c_comm_master_write(t->i2c, t->i2c_addr, b, len + 1);

	i2c_comm_master_unlock(t->i2c);

	return ret;
}

static int _r850_read_regs(struct r850_tuner *t, u8 reg, u8 *buf, int len)
{
	int ret = 0, i;
	u8 b[1 + R850_NUM_REGS];

	if (!t || !buf || !len)
		return -EINVAL;

	if (len > (R850_NUM_REGS - reg))
		return -EINVAL;

	b[0] = 0x00;

	ret = i2c_comm_master_lock(t->i2c);
	if (ret)
		return ret;

	ret = i2c_comm_master_write(t->i2c, t->i2c_addr, b, 1);
	if (ret)
		goto exit;

	ret = i2c_comm_master_read(t->i2c, t->i2c_addr, &b[0], len + reg);
	if (ret)
		goto exit;

	for (i = reg; i < (reg + len); i++)
		buf[i - reg] = reverse_bit(b[i]);

exit:
	i2c_comm_master_unlock(t->i2c);

	return ret;
}

int r850_init(struct r850_tuner *t)
{
	int ret = 0;

	if (t->priv.init)
		return 0;

	mutex_init(&t->priv.lock);

	// should check reg 0x00
	t->priv.chip = 1;

	memcpy(t->priv.regs, init_regs, sizeof(t->priv.regs));

#if 0
	ret = _r850_write_regs(t, 0x00, t->regs, R850_NUM_REGS);
#endif

	t->priv.init = true;

	return ret;
}

int r850_term(struct r850_tuner *t)
{
	if (!t->priv.init)
		return 0;

	memset(t->priv.regs, 0, sizeof(t->priv.regs));

	t->priv.chip = 0;

	mutex_destroy(&t->priv.lock);

	t->priv.init = false;

	return 0;
}

int r850_write_config_regs(struct r850_tuner *t, u8 *regs)
{
	int ret = 0;

	mutex_lock(&t->priv.lock);

	ret = _r850_write_regs(t, 0x08, regs, R850_NUM_REGS - 0x08);

	mutex_unlock(&t->priv.lock);

	return 0;
}

int r850_is_pll_locked(struct r850_tuner *t, bool *locked)
{
	int ret = 0;
	u8 tmp = 0;

	mutex_lock(&t->priv.lock);

	ret = _r850_read_regs(t, 0x02, &tmp, 1);

	mutex_unlock(&t->priv.lock);

	if (ret) {
		dev_err(t->dev, "r850_is_pll_locked: r850_read_regs() failed. (ret: %d)\n", ret);
		return ret;
	}

	*locked = (tmp & 0x40) ? true : false;

	return 0;
}
