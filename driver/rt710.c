// rt710.c

// RafaelMicro RT710 driver

#include "print_format.h"

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/device.h>

#include "i2c_comm.h"
#include "rt710.h"

#define NUM_REGS	0x10

struct rt710_bandwidth_param {
	u8 coarse;
	u8 fine;
};

static const u8 init_regs[NUM_REGS] = {
	0x40, 0x1d, 0x20, 0x10, 0x41, 0x50, 0xed, 0x25,
	0x07, 0x58, 0x39, 0x64, 0x38, 0xe7, 0x90, 0x35
};

static const u8 sleep_regs[NUM_REGS] = {
	0xff, 0x5c, 0x88, 0x30, 0x41, 0xc8, 0xed, 0x25,
	0x47, 0xfc, 0x48, 0xa2, 0x08, 0x0f, 0xf3, 0x59
};

static const struct {
	u32 bandwidth;
	struct rt710_bandwidth_param param;
} bandwidth_params[] = {
	{ 50000, { 0, 0 } },
	{ 73000, { 0, 1 } },
	{ 96000, { 1, 0 } },
	{ 104000, { 1, 1 } },
	{ 116000, { 2, 0 } },
	{ 126000, { 2, 1 } },
	{ 134000, { 3, 0 } },
	{ 146000, { 3, 1 } },
	{ 158000, { 4, 0 } },
	{ 170000, { 4, 1 } },
	{ 178000, { 5, 0 } },
	{ 190000, { 5, 1 } },
	{ 202000, { 6, 0 } },
	{ 212000, { 6, 1 } },
	{ 218000, { 7, 0 } },
	{ 234000, { 7, 1 } },
	{ 244000, { 9, 1 } },
	{ 246000, { 10, 0 } },
	{ 262000, { 10, 1 } },
	{ 266000, { 11, 0 } },
	{ 282000, { 11, 1 } },
	{ 298000, { 12, 1 } },
	{ 318000, { 13, 1 } },
	{ 340000, { 14, 1 } },
	{ 358000, { 15, 1 } },
	{ 379999, { 16, 1 } },
};

static const u16 lna_acc_gain[] = {
	0, 26, 42, 74, 103, 129, 158, 181,
	188, 200, 220, 248, 280, 312, 341, 352,
	366, 389, 409
};

static u8 reverse_bit(u8 val)
{
	u8 t = val;

	t = (t & 0x55) << 1 | (t & 0xaa) >> 1;
	t = (t & 0x33) << 2 | (t & 0xcc) >> 2;
	t = (t & 0x0f) << 4 | (t & 0xf0) >> 4;

	return t;
}

static int _rt710_write_regs(struct rt710_tuner *t, u8 reg, const u8 *buf, int len)
{
	int ret = 0;
	u8 b[1 + NUM_REGS];

	if (!t || !buf || !len)
		return -EINVAL;

	if (len > (NUM_REGS - reg))
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

static int _rt710_read_regs(struct rt710_tuner *t, u8 reg, u8 *buf, int len)
{
	int ret = 0, i;
	u8 b[1 + NUM_REGS];

	if (!t || !buf || !len)
		return -EINVAL;

	if (len > (NUM_REGS - reg))
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

static int _rt710_set_pll(struct rt710_tuner *t, u8 *regs, u32 freq)
{
	int ret = 0;
	u32 vco_min, vco_max, vco_freq;
	u16 vco_fra, nsdm = 2, sdm = 0;
	u8 mix_div = 2, div_num, nint, ni, si;

	vco_min = 2350000;
	vco_max = vco_min * 2;
	vco_freq = freq * mix_div;

	t->priv.freq = 0;

	while (mix_div <= 16) {
		if (vco_freq >= vco_min && vco_freq <= vco_max)
			break;

		mix_div *= 2;
		vco_freq = freq * mix_div;
	}

	switch (mix_div) {
	case 2:
		div_num = 1;
		break;
	case 4:
		div_num = 0;
		break;
	case 8:
		div_num = 2;
		break;
	case 16:
		div_num = 3;
		break;
	default:
		div_num = 0;
		break;
	}

	regs[4] &= 0xfe;
	regs[4] |= (div_num & 1);

	ret = _rt710_write_regs(t, 0x04, &regs[0x04], 1);
	if (ret)
		return ret;

	nint = (vco_freq / 2) / 24000;
	vco_fra = vco_freq - (24000 * 2 * nint);

	if (vco_fra < 375) {
		vco_fra = 0;
	} else if (vco_fra > 47625) {
		vco_fra = 0;
		nint++;
	} else if (vco_fra > 23812 && vco_fra < 24000) {
		vco_fra = 23812;
	} else if (vco_fra > 24000 && vco_fra < 24187) {
		vco_fra = 24187;
	}

	ni = (nint - 13) / 4;
	si = nint - (ni * 4) - 13;

	regs[0x05] = ni + (si << 6);

	ret = _rt710_write_regs(t, 0x05, &regs[0x05], 1);
	if (ret)
		return ret;

	if (!vco_fra)
		regs[0x04] |= 0x02;

	ret = _rt710_write_regs(t, 0x04, &regs[0x04], 1);
	if (ret)
		return ret;

	while (vco_fra > 1) {
		u32 t;

		t = (24000 * 2) / nsdm;
		if (vco_fra > t) {
			sdm += (0x8000 / (nsdm / 2));
			vco_fra -= t;

			if (nsdm >= 0x8000)
				break;
		}

		nsdm *= 2;
	}

	regs[0x07] = ((sdm >> 8) & 0xff);
	regs[0x06] = (sdm & 0xff);

	ret = _rt710_write_regs(t, 0x07, &regs[0x07], 1);
	if (ret)
		return ret;

	ret = _rt710_write_regs(t, 0x06, &regs[0x06], 1);
	if (ret)
		return ret;

	t->priv.freq = freq;

	return 0;
}

int rt710_init(struct rt710_tuner *t)
{
	int ret = 0;
	u8 tmp;

	mutex_init(&t->priv.lock);

	t->priv.init = false;
	t->priv.freq = 0;

	ret = _rt710_read_regs(t, 0x03, &tmp, 1);
	if (ret) {
		dev_err(t->dev, "rt710_init: rt710_read_regs() failed. (ret: %d)\n", ret);
		return ret;
	}

	if ((tmp & 0xf0) != 0x70) {
		dev_err(t->dev, "rt710_init: Unknown chip.\n");
		return -ENOSYS;
	}

	t->priv.init = true;

	return 0;
}

int rt710_term(struct rt710_tuner *t)
{
	mutex_destroy(&t->priv.lock);

	t->priv.init = false;

	return 0;
}

int rt710_sleep(struct rt710_tuner *t)
{
	int ret = 0;
	u8 regs[NUM_REGS];

	if (!t->priv.init)
		return 0;

	memcpy(regs, sleep_regs, sizeof(regs));

	mutex_lock(&t->priv.lock);

	ret = _rt710_write_regs(t, 0x00, regs, NUM_REGS);

	mutex_unlock(&t->priv.lock);

	return ret;
}

int rt710_set_params(struct rt710_tuner *t, u32 freq, u32 symbol_rate, u32 rolloff)
{
	int ret = 0;
	u8 regs[NUM_REGS];
	u32 bandwidth;
	struct rt710_bandwidth_param bw_param = {};

	if (!t->priv.init)
		return -EINVAL;

	if (rolloff > 5)
		return -EINVAL;

	memcpy(regs, init_regs, sizeof(regs));

	if (t->config.loop_through)
		regs[0x01] |= 0xfb;

	switch (t->config.agc_mode) {
	case RT710_AGC_POSITIVE:
		regs[0x0d] |= 0x10;
		break;

	case RT710_AGC_NEGATIVE:
	default:
		break;
	}

	switch (t->config.vga_atten_mode) {
	case RT710_VGA_ATTEN_ON:
		regs[0x0b] |= 0x08;
		break;

	case RT710_VGA_ATTEN_OFF:
	default:
		break;
	}

	if (t->config.fine_gain >= RT710_FINE_GAIN_3DB && t->config.fine_gain <= RT710_FINE_GAIN_0DB)
		regs[0x0e] |= t->config.fine_gain;

	mutex_lock(&t->priv.lock);

	ret = _rt710_write_regs(t, 0x00, regs, NUM_REGS);
	if (ret) {
		dev_err(t->dev, "rt710_set_params: rt710_write_regs(0x00, NUM_REGS) failed. (ret: %d)", ret);
		goto fail;
	}

	ret = _rt710_set_pll(t, regs, freq);
	if (ret) {
		dev_err(t->dev, "rt710_set_params: rt710_set_pll() failed. (ret: %d)\n", ret);
		goto fail;
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

	ret = _rt710_write_regs(t, 0x0a, &regs[0x0a], 1);
	if (ret)
		goto fail;

	ret = _rt710_write_regs(t, 0x02, &regs[0x02], 1);
	if (ret)
		goto fail;

	ret = _rt710_write_regs(t, 0x08, &regs[0x08], 1);
	if (ret)
		goto fail;

	regs[0x0e] &= 0xf3;

	if (freq >= 2000000)
		regs[0x0e] |= 0x08;

	ret = _rt710_write_regs(t, 0x0e, &regs[0x0e], 1);
	if (ret)
		goto fail;

	bandwidth = (symbol_rate * (0x73 + (rolloff * 5))) / 10;

	if (!bandwidth) {
		ret = -ECANCELED;
		goto fail;
	}

	if (bandwidth >= 380000) {
		bandwidth -= 380000;
		if (bandwidth % 17400)
			bw_param.coarse++;
		bw_param.coarse += ((bandwidth / 17400) & 0xff) + 0x10;
		bw_param.fine = 1;
	} else {
		int i;

		for (i = 0; i < (sizeof(bandwidth_params) / sizeof(bandwidth_params[0])); i++) {
			if (bandwidth <= bandwidth_params[i].bandwidth) {
				bw_param = bandwidth_params[i].param;
				break;
			}
		}
	}

	regs[0x0f] = (bw_param.coarse << 2) | bw_param.fine;

	ret = _rt710_write_regs(t, 0x0f, &regs[0x0f], 1);
	if (ret)
		goto fail;

	mutex_unlock(&t->priv.lock);

	return 0;

fail:
	mutex_unlock(&t->priv.lock);

	return ret;
}

int rt710_is_pll_locked(struct rt710_tuner *t, bool *locked)
{
	int ret = 0;
	u8 tmp;

	if (!t->priv.init)
		return -EINVAL;

	mutex_lock(&t->priv.lock);

	ret = _rt710_read_regs(t, 0x02, &tmp, 1);

	mutex_unlock(&t->priv.lock);

	if (ret) {
		dev_err(t->dev, "rt710_is_pll_locked: _rt710_read_regs() failed. (ret: %d)\n", ret);
		return ret;
	}

	*locked = (tmp & 0x80) ? true : false;

	return 0;
}

int rt710_get_rf_gain(struct rt710_tuner *t, u8 *gain)
{
	int ret = 0;
	u8 tmp, g;

	if (!t->priv.init)
		return -EINVAL;

	mutex_lock(&t->priv.lock);

	ret = _rt710_read_regs(t, 0x01, &tmp, 1);

	mutex_unlock(&t->priv.lock);

	if (ret) {
		dev_err(t->dev, "rt710_get_rf_gain: _rt710_read_regs() failed. (ret: %d)\n", ret);
		return ret;
	}

	g = ((tmp & 0xf0) >> 4) | ((tmp & 0x01) << 4);

	if (g <= 2) {
		*gain = 0;
	} else if (g <= 9) {
		// 1 - 7
		*gain = g - 2;
	} else if (g <= 12) {
		*gain = 7;
	} else if (g <= 22) {
		// 8 - 17
		*gain = g - 5;
	} else {
		*gain = 18;
	}

	return 0;
}

int rt710_get_rf_signal_strength(struct rt710_tuner *t, s32 *ss)
{
	int ret = 0;
	u8 gain;
	s32 tmp;

	ret = rt710_get_rf_gain(t, &gain);
	if (ret) {
		dev_err(t->dev, "rt710_get_rf_signal_strength: rt710_get_rf_gain() failed. (ret: %d)\n", ret);
		return ret;
	}

	if (t->priv.freq < 1200000) {
		tmp = 190;
	} else if (t->priv.freq < 1800000) {
		tmp = 170;
	} else {
		tmp = 140;
	}

	*ss = (tmp + lna_acc_gain[gain]) * -100;

	return 0;
}
