// rt710.c

// RafaelMicro RT710 driver

#include "print_format.h"

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/delay.h>

#include "i2c_comm.h"
#include "rt710.h"

#define NUM_REGS	0x10

static const u8 init_regs[NUM_REGS] = {
	0x40, 0x1d, 0x20, 0x10, 0x41, 0x50, 0xed, 0x25,
	0x07, 0x58, 0x39, 0x64, 0x38, 0xf7/*0xe7*/, 0x90, 0x35
};

static const u8 sleep_regs[NUM_REGS] = {
	0xff, 0x5c, 0x88, 0x30, 0x41, 0xc8, 0xed, 0x25,
	0x47, 0xfc, 0x48, 0xa2, 0x08, 0x0f, 0xf3, 0x59
};

static u8 reverse_bit(u8 val)
{
	u8 t = val;

	t = (t & 0x55) << 1 | (t & 0xaa) >> 1;
	t = (t & 0x33) << 2 | (t & 0xcc) >> 2;
	t = (t & 0x0f) << 4 | (t & 0xf0) >> 4;

	return t;
}

static int rt710_write_regs(struct rt710_tuner *t, u8 reg, const u8 *buf, int len)
{
	u8 b[1 + NUM_REGS];

	if (!t || !buf || !len)
		return -EINVAL;

	if (len > (NUM_REGS - reg))
		return -EINVAL;

	b[0] = reg;
	memcpy(&b[1], buf, len);

	return i2c_comm_master_write(t->i2c, t->i2c_addr, b, len + 1);
}

static int rt710_read_regs(struct rt710_tuner *t, u8 reg, u8 *buf, int len)
{
	int ret = 0, i;
	u8 b[1 + NUM_REGS];

	if (!t || !buf || !len)
		return -EINVAL;

	if (len > (NUM_REGS - reg))
		return -EINVAL;

	b[0] = 0x00;

	ret = i2c_comm_master_write(t->i2c, t->i2c_addr, b, 1);
	if (ret)
		return ret;

	ret = i2c_comm_master_read(t->i2c, t->i2c_addr, &b[0], len + reg);
	if (ret)
		return ret;

	for (i = reg; i < (reg + len); i++)
		buf[i - reg] = reverse_bit(b[i]);

	return 0;
}

int rt710_init(struct rt710_tuner *t)
{
	int ret = 0;
	u8 tmp;

	ret = rt710_read_regs(t, 0x03, &tmp, 1);
	if (ret) {
		pr_debug("rt710_init: rt710_read_regs() failed.\n");
		return ret;
	}

	if ((tmp & 0xf0) != 0x70) {
		pr_debug("rt710_init: Unknown chip.\n");
		return -ENOSYS;
	}

	return 0;
}

int rt710_sleep(struct rt710_tuner *t, bool sleep)
{
	u8 regs[NUM_REGS];

	memcpy(regs, sleep_regs, sizeof(regs));

	if (sleep)
		regs[0x03] = 0x20;

	return rt710_write_regs(t, 0x00, regs, NUM_REGS);
}

static int rt710_set_pll_regs(struct rt710_tuner *t, u8 *regs, u32 freq)
{
	int ret = 0;
	u32 min, max, b, c;
	u16 e, g, h;
	u8 div, a, d, f;

	min = 2350000;
	max = min * 2;
	div = 2;
	a = 0;
	g = 2;
	h = 0;

	do {
		u32 q;
		q = freq * div;
		if (q >= min && q <= max) {
			switch(div) {
			case 2:
				a = 1;
				break;
			case 4:
				a = 0;
				break;
			case 8:
				a = 2;
				break;
			case 16:
				a = 3;
				break;
			default:
				return -ECANCELED;
			}
			break;
		}
		div *= 2;
	} while(div <= 16);

	regs[4] &= 0xfe;
	regs[4] |= (a & 1);

	ret = rt710_write_regs(t, 0x04, &regs[0x04], 1);
	if (ret)
		return ret;

	b = freq * div;
	c = (b / 2) / 24000;
	d = (c & 0xff);
	e = (d * 17536) + (b & 0xffff);

	if (e < 375) {
		e = 0;
	} else if (e > 47625) {
		e = 0;
		d++;
	} else if (e > 23812 && e < 24000) {
		e = 23812;
	} else if (e > 24000 && e < 24187) {
		e = 24187;
	}

	f = (d - 13) / 4;
	regs[0x05] = f + ((d - (f * 4) - 13) << 6);

	ret = rt710_write_regs(t, 0x05, &regs[0x05], 1);
	if (ret)
		return ret;

	if (!e)
		regs[0x04] |= 0x02;

	ret = rt710_write_regs(t, 0x04, &regs[0x04], 1);
	if (ret)
		return ret;

	while (e > 1) {
		u32 s;
		s = (24000 * 2) / g;
		if (e > s) {
			h += (32768 / (g / 2));
			e -= s;
			if (g >= 32768) {
				break;
			}
		}
		g *= 2;
	}

	regs[0x07] = ((h >> 8) & 0xff);
	regs[0x06] = (h & 0xff);

	ret = rt710_write_regs(t, 0x07, &regs[0x07], 1);
	if (ret)
		return ret;

	ret = rt710_write_regs(t, 0x06, &regs[0x06], 1);
	if (ret)
		return ret;

	return 0;
}

int rt710_set_params(struct rt710_tuner *t, u32 freq, u32 symbol_rate, u32 rolloff)
{
	int ret = 0;
	u8 regs[NUM_REGS];
	u32 a;
	u8 b = 0, f = 0;
	struct {
		u32 a;
		u8 b;
		u8 f;
	} c[] = {
		{ 50000, 0, 0 },
		{ 73000, 0, 1 },
		{ 96000, 1, 0 },
		{ 104000, 1, 1 },
		{ 116000, 2, 0 },
		{ 126000, 2, 1 },
		{ 134000, 3, 0 },
		{ 146000, 3, 1 },
		{ 158000, 4, 0 },
		{ 170000, 4, 1 },
		{ 178000, 5, 0 },
		{ 190000, 5, 1 },
		{ 202000, 6, 0 },
		{ 212000, 6, 1 },
		{ 218000, 7, 0 },
		{ 234000, 7, 1 },
		{ 244000, 9, 1 },
		{ 246000, 10, 0 },
		{ 262000, 10, 1 },
		{ 266000, 11, 0 },
		{ 282000, 11, 1 },
		{ 298000, 12, 1 },
		{ 318000, 13, 1 },
		{ 340000, 14, 1 },
		{ 358000, 15, 1 },
		{ 379999, 16, 1 }
	};

	if (rolloff > 5)
		return -EINVAL;

	memcpy(regs, init_regs, sizeof(regs));

	ret = rt710_write_regs(t, 0x00, regs, NUM_REGS);
	if (ret) {
		pr_debug("rt710_set_params: rt710_write_regs(0x00, NUM_REGS) failed. (ret: %d)", ret);
		return ret;
	}

	ret = rt710_set_pll_regs(t, regs, freq);
	if (ret) {
		pr_debug("rt710_set_params: rt710_set_pll_regs() failed. (ret: %d)\n", ret);
		return ret;
	}

	msleep(10);

	if ((freq - 1600000) >= 350000) {
		regs[0x02] &= 0xbf;
		regs[0x08] &= 0x7f;
		if (freq >= 1950000)
			regs[0x0a] = 0x38;
	} else {
		regs[0x02] |= 0x40;
		regs[0x08] |= 0x80;
	}

	ret = rt710_write_regs(t, 0x0a, &regs[0x0a], 1);
	if (ret)
		return ret;

	ret = rt710_write_regs(t, 0x02, &regs[0x02], 1);
	if (ret)
		return ret;

	ret = rt710_write_regs(t, 0x08, &regs[0x08], 1);
	if (ret)
		return ret;

	regs[0x0e] &= 0xf3;

	if (freq >= 2000000)
		regs[0x0e] |= 0x08;

	ret = rt710_write_regs(t, 0x0e, &regs[0x0e], 1);
	if (ret)
		return ret;

	a = (symbol_rate * (0x73 + (rolloff * 5))) / 10;

	if (!a)
		return -ECANCELED;

	if (a >= 380000) {
		a -= 380000;
		if (a % 17400)
			b++;
		a /= 17400;
		b += (a & 0xff) + 0x10;
		f = 1;
	} else {
		int i;

		for (i = 0; i < (sizeof(c) / sizeof(c[0])); i++) {
			if (a <= c[i].a) {
				b = c[i].b;
				f = c[i].f;
				break;
			}
		}
	}

	regs[0x0f] = (b << 2) | f;

	ret = rt710_write_regs(t, 0x0f, &regs[0x0f], 1);
	if (ret)
		return ret;

	return ret;
}

int rt710_get_pll_locked(struct rt710_tuner *t, bool *locked)
{
	int ret = 0;
	u8 tmp;

	ret = rt710_read_regs(t, 0x02, &tmp, 1);
	if (ret) {
		pr_debug("rt710_get_pll_locked: rt710_read_regs() failed. (ret: %d)\n", ret);
		return ret;
	}

	*locked = (tmp & 0x80) ? true : false;

	return ret;
}
