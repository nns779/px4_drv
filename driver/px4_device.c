// SPDX-License-Identifier: GPL-2.0-only
/*
 * PTX driver for PLEX PX4/PX5 series devices (px4_device.c)
 *
 * Copyright (c) 2018-2021 nns779
 */

#include "print_format.h"
#include "px4_device.h"

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/slab.h>

#include "px4_device_params.h"
#include "firmware.h"

#define PX4_DEVICE_TS_SYNC_COUNT	4
#define PX4_DEVICE_TS_SYNC_SIZE		(188 * PX4_DEVICE_TS_SYNC_COUNT)

struct px4_stream_context {
	struct ptx_chrdev *chrdev[PX4_CHRDEV_NUM];
	u8 remain_buf[PX4_DEVICE_TS_SYNC_SIZE];
	size_t remain_len;
};

static int px4_chrdev_set_lnb_voltage_s(struct ptx_chrdev *chrdev, int voltage);
static void px4_device_release(struct kref *kref);

static int px4_backend_set_power(struct px4_device *px4, bool state)
{
	int ret = 0;
	struct it930x_bridge *it930x = &px4->it930x;

	dev_dbg(px4->dev,
		"px4_backend_set_power: %s\n", (state) ? "true" : "false");

	if (!state && !atomic_read(&px4->available))
		return 0;

	if (state) {
		ret = it930x_write_gpio(it930x, 7, false);
		if (ret)
			return ret;

		msleep(80);

		ret = it930x_write_gpio(it930x, 2, true);
		if (ret)
			return ret;

		msleep(20);
	} else {
		it930x_write_gpio(it930x, 2, false);
		it930x_write_gpio(it930x, 7, true);
	}

	return 0;
}

static int px4_backend_init(struct px4_device *px4)
{
	int ret = 0, i;

	for (i = 0; i < PX4_CHRDEV_NUM; i++) {
		struct px4_chrdev *chrdev4 = &px4->chrdev4[i];

		ret = tc90522_init(&chrdev4->tc90522);
		if (ret) {
			dev_err(px4->dev,
				"px4_backend_init: tc90522_init() failed. (i: %d, ret: %d)\n",
				i, ret);
			break;
		}

		switch (chrdev4->chrdev->system_cap) {
		case PTX_ISDB_T_SYSTEM:
			ret = r850_init(&chrdev4->tuner.r850);
			if (ret)
				dev_err(px4->dev,
					"px4_backend_init: r850_init() failed. (i: %d, ret: %d)\n",
					i, ret);

			break;

		case PTX_ISDB_S_SYSTEM:
			ret = rt710_init(&chrdev4->tuner.rt710);
			if (ret)
				dev_err(px4->dev,
					"px4_backend_init: rt710_init() failed. (i: %d, ret: %d)\n",
					i, ret);

			break;

		default:
			dev_err(px4->dev,
				"px4_backend_init: unknown system\n");
			break;
		}

		if (ret)
			break;
	}

	return ret;
}

static int px4_backend_term(struct px4_device *px4)
{
	int i;

	for (i = 0; i < PX4_CHRDEV_NUM; i++) {
		struct px4_chrdev *chrdev4 = &px4->chrdev4[i];

		switch (chrdev4->chrdev->system_cap) {
		case PTX_ISDB_T_SYSTEM:
			r850_term(&chrdev4->tuner.r850);
			break;

		case PTX_ISDB_S_SYSTEM:
			rt710_term(&chrdev4->tuner.rt710);
			break;

		default:
			break;
		}

		tc90522_term(&chrdev4->tc90522);
	}

	return 0;
}

static void px4_device_stream_process(struct ptx_chrdev **chrdev,
				      u8 **buf, u32 *len)
{
	u8 *p = *buf;
	u32 remain = *len;

	while (likely(remain)) {
		u32 i;
		bool sync_remain = false;

		for (i = 0; i < PX4_DEVICE_TS_SYNC_COUNT; i++) {
			if (likely(((i + 1) * 188) <= remain)) {
				if (unlikely((p[i * 188] & 0x8f) != 0x07))
					break;
			} else {
				sync_remain = true;
				break;
			}
		}

		if (unlikely(sync_remain))
			break;

		if (unlikely(i < PX4_DEVICE_TS_SYNC_COUNT)) {
			p++;
			remain--;
			continue;
		}

		while (likely(remain >= 188 && ((p[0] & 0x8f) == 0x07))) {
			u8 id = (p[0] & 0x70) >> 4;

			if (likely(id && id < 5)) {
				p[0] = 0x47;
				ptx_chrdev_put_stream(chrdev[id - 1], p, 188);
			}

			p += 188;
			remain -= 188;
		}
	}

	*buf = p;
	*len = remain;

	return;
}

static int px4_device_stream_handler(void *context, void *buf, u32 len)
{
	struct px4_stream_context *stream_ctx = context;
	u8 *ctx_remain_buf = stream_ctx->remain_buf;
	u32 ctx_remain_len = stream_ctx->remain_len;
	u8 *p = buf;
	u32 remain = len;

	if (unlikely(ctx_remain_len)) {
		if (likely((ctx_remain_len + len) >= PX4_DEVICE_TS_SYNC_SIZE)) {
			u32 t = PX4_DEVICE_TS_SYNC_SIZE - ctx_remain_len;

			memcpy(ctx_remain_buf + ctx_remain_len, p, t);
			ctx_remain_len = PX4_DEVICE_TS_SYNC_SIZE;

			px4_device_stream_process(stream_ctx->chrdev,
						  &ctx_remain_buf,
						  &ctx_remain_len);
			if (likely(!ctx_remain_len)) {
				p += t;
				remain -= t;
			}

			stream_ctx->remain_len = 0;
		} else {
			memcpy(ctx_remain_buf + ctx_remain_len, p, len);
			stream_ctx->remain_len += len;

			return 0;
		}
	}

	px4_device_stream_process(stream_ctx->chrdev, &p, &remain);

	if (unlikely(remain)) {
		memcpy(stream_ctx->remain_buf, p, remain);
		stream_ctx->remain_len = remain;
	}

	return 0;
}

static int px4_chrdev_init(struct ptx_chrdev *chrdev)
{
	dev_dbg(chrdev->parent->dev, "px4_chrdev_init\n");

	chrdev->params.system = chrdev->system_cap;
	return 0;
}

static int px4_chrdev_term_t(struct ptx_chrdev *chrdev)
{
	dev_dbg(chrdev->parent->dev, "px4_chrdev_term_t\n");
	return 0;
}

static int px4_chrdev_term_s(struct ptx_chrdev *chrdev)
{
	dev_dbg(chrdev->parent->dev, "px4_chrdev_term_s\n");
	return 0;
}

static struct tc90522_regbuf tc_init_s[] = {
	{ 0x15, NULL, { 0x00 } },
	{ 0x1d, NULL, { 0x00 } },
	{ 0x04, NULL, { 0x02 } }
};

static struct tc90522_regbuf tc_init_t[] = {
	{ 0xb0, NULL, { 0xa0 } },
	{ 0xb2, NULL, { 0x3d } },
	{ 0xb3, NULL, { 0x25 } },
	{ 0xb4, NULL, { 0x8b } },
	{ 0xb5, NULL, { 0x4b } },
	{ 0xb6, NULL, { 0x3f } },
	{ 0xb7, NULL, { 0xff } },
	{ 0xb8, NULL, { 0xc0 } },
	{ 0x1f, NULL, { 0x00 } },
	{ 0x75, NULL, { 0x00 } }
};

static struct tc90522_regbuf tc_init_s0[] = {
	{ 0x07, NULL, { 0x31 } },
	{ 0x08, NULL, { 0x77 } }
};

static struct tc90522_regbuf tc_init_t0[] = {
	{ 0x0e, NULL, { 0x77 } },
	{ 0x0f, NULL, { 0x13 } }
};

static int px4_chrdev_open(struct ptx_chrdev *chrdev)
{
	int ret = 0, i;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct px4_chrdev *chrdev4 = chrdev->priv;
	struct px4_device *px4 = chrdev4->parent;
	bool need_init = false;

	dev_dbg(px4->dev,
		"px4_chrdev_open %u:%u\n", chrdev_group->id, chrdev->id);

	mutex_lock(&px4->lock);

	if (px4->mldev) {
		ret = px4_mldev_set_power(px4->mldev, px4, chrdev->id, true, &need_init);
		if (ret) {
			dev_err(px4->dev,
				"px4_chrdev_open %u:%u: px4_mldev_set_power(true) failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id, ret);
			goto fail_backend_power;
		}
	} else if (!px4->open_count) {
		ret = px4_backend_set_power(px4, true);
		if (ret) {
			dev_err(px4->dev,
				"px4_chrdev_open %u:%u: px4_backend_set_power(true) failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id, ret);
			goto fail_backend_power;
		}
		need_init = true;
	}

	if (need_init) {
		dev_dbg(px4->dev,
			"px4_chrdev_open %u:%u: init\n",
			chrdev_group->id, chrdev->id);

		ret = px4_backend_init(px4);
		if (ret) {
			dev_err(px4->dev,
				"px4_chrdev_open %u:%u: px4_backend_init() failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id, ret);
			goto fail_backend_init;
		}

		for (i = 0; i < PX4_CHRDEV_NUM; i++) {
			struct px4_chrdev *c = &px4->chrdev4[i];

			if (i == chrdev->id)
				continue;

			if (atomic_read(&c->chrdev->open))
				continue;

			switch (c->chrdev->system_cap) {
			case PTX_ISDB_T_SYSTEM:
				ret = r850_sleep(&c->tuner.r850);
				if (ret) {
					dev_err(px4->dev,
						"px4_chrdev_open %u:%u: rt850_sleep(%d) failed. (ret: %d)\n",
						chrdev_group->id, chrdev->id,
						i, ret);
					break;
				}

				ret = tc90522_sleep_t(&c->tc90522, true);
				if (ret) {
					dev_err(px4->dev,
						"px4_chrdev_open %u:%u: tc90522_sleep_t(%d, true) failed. (ret: %d)\n",
						chrdev_group->id, chrdev->id,
						i, ret);
					break;
				}

				break;

			case PTX_ISDB_S_SYSTEM:
				if (!px4_device_params.s_tuner_no_sleep) {
					ret = rt710_sleep(&c->tuner.rt710);
					if (ret) {
						dev_err(px4->dev,
							"px4_chrdev_open %u:%u: rt710_sleep(%d) failed. (ret: %d)\n",
							chrdev_group->id,
							chrdev->id,
							i, ret);
						break;
					}
				}

				ret = tc90522_sleep_s(&c->tc90522, true);
				if (ret) {
					dev_err(px4->dev,
						"px4_chrdev_open %u:%u: tc90522_sleep_s(%d, true) failed. (ret: %d)\n",
						chrdev_group->id, chrdev->id,
						i, ret);
					break;
				}

				break;

			default:
				break;
			}

			if (ret)
				goto fail_backend;
		}
	}

	/* wake up */
	switch (chrdev->system_cap) {
	case PTX_ISDB_T_SYSTEM:
	{
		struct r850_system_config sys;

		ret = tc90522_write_multiple_regs(&chrdev4->tc90522,
						  tc_init_t,
						  ARRAY_SIZE(tc_init_t));
		if (ret) {
			dev_err(px4->dev,
				"px4_chrdev_open %u:%u: tc90522_write_multiple_regs(tc_init_t) failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id, ret);
			break;
		}

		/* disable ts pins */
		ret = tc90522_enable_ts_pins_t(&chrdev4->tc90522, false);
		if (ret) {
			dev_err(px4->dev,
				"px4_chrdev_open %u:%u: tc90522_enable_ts_pins_t(false) failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id, ret);
			break;
		}

		/* wake up */
		ret = tc90522_sleep_t(&chrdev4->tc90522, false);
		if (ret) {
			dev_err(px4->dev,
				"px4_chrdev_open %u:%u: tc90522_sleep_t(false) failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id, ret);
			break;
		}

		ret = r850_wakeup(&chrdev4->tuner.r850);
		if (ret) {
			dev_err(px4->dev,
				"px4_chrdev_open %u:%u: r850_wakeup() failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id, ret);
			break;
		}

		sys.system = R850_SYSTEM_ISDB_T;
		sys.bandwidth = R850_BANDWIDTH_6M;
		sys.if_freq = 4063;

		ret = r850_set_system(&chrdev4->tuner.r850, &sys);
		if (ret) {
			dev_err(px4->dev,
				"px4_chrdev_open %u:%u: r850_set_system() failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id, ret);
			break;
		}

		break;
	}

	case PTX_ISDB_S_SYSTEM:
	{
		ret = tc90522_write_multiple_regs(&chrdev4->tc90522,
						  tc_init_s,
						  ARRAY_SIZE(tc_init_s));
		if (ret) {
			dev_err(px4->dev,
				"px4_chrdev_open %u:%u: tc90522_write_multiple_regs(tc_init_s) failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id, ret);
			break;
		}

		/* disable ts pins */
		ret = tc90522_enable_ts_pins_s(&chrdev4->tc90522, false);
		if (ret) {
			dev_err(px4->dev,
				"px4_chrdev_open %u:%u: tc90522_enable_ts_pins_s(false) failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id, ret);
			break;
		}

		/* wake up */
		ret = tc90522_sleep_s(&chrdev4->tc90522, false);
		if (ret) {
			dev_err(px4->dev,
				"px4_chrdev_open %u:%u: tc90522_sleep_s(false) failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id, ret);
			break;
		}

		break;
	}

	default:
		break;
	}

	if (ret)
		goto fail_backend;

	if (!px4->open_count) {
		/* S0 */
		ret = tc90522_write_multiple_regs(&px4->chrdev4[0].tc90522,
						  tc_init_s0,
						  ARRAY_SIZE(tc_init_s0));
		if (ret) {
			dev_err(px4->dev,
				"px4_chrdev_open %u:%u: tc90522_write_multiple_regs(tc_init_s0) failed.\n",
				chrdev_group->id, chrdev->id);
			goto fail_backend;
		}

		/* T0 */
		ret = tc90522_write_multiple_regs(&px4->chrdev4[2].tc90522,
						  tc_init_t0,
						  ARRAY_SIZE(tc_init_t0));
		if (ret) {
			dev_err(px4->dev,
				"px4_chrdev_open %u:%u: tc90522_write_multiple_regs(tc_init_t0) failed.\n",
				chrdev_group->id, chrdev->id);
			goto fail_backend;
		}
	}

	px4->open_count++;
	kref_get(&px4->kref);

	mutex_unlock(&px4->lock);
	return 0;

fail_backend:
	if (!px4->open_count)
		px4_backend_term(px4);

fail_backend_init:
	if (px4->mldev)
		px4_mldev_set_power(px4->mldev, px4, chrdev->id, false, NULL);
	else if (!px4->open_count)
		px4_backend_set_power(px4, false);

fail_backend_power:
	mutex_unlock(&px4->lock);
	dev_dbg(px4->dev,
		"px4_chrdev_open %u:%u: ret: %d\n",
		chrdev_group->id, chrdev->id, ret);
	return ret;
}

static int px4_chrdev_release(struct ptx_chrdev *chrdev)
{
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct px4_chrdev *chrdev4 = chrdev->priv;
	struct px4_device *px4 = chrdev4->parent;

	dev_dbg(px4->dev,
		"px4_chrdev_release %u:%u: kref count: %u\n",
		chrdev_group->id, chrdev->id, kref_read(&px4->kref));

	px4_chrdev_set_lnb_voltage_s(chrdev, 0);

	mutex_lock(&px4->lock);

	if (!px4->open_count) {
		mutex_unlock(&px4->lock);
		return -EALREADY;
	}

	px4->open_count--;
	if (!px4->open_count) {
		px4_backend_term(px4);
		if (!px4->mldev)
			px4_backend_set_power(px4, false);
	} else if (atomic_read(&px4->available)) {
		/* sleep tuners */
		switch (chrdev->system_cap) {
		case PTX_ISDB_T_SYSTEM:
			r850_sleep(&chrdev4->tuner.r850);
			tc90522_sleep_t(&chrdev4->tc90522, true);
			break;

		case PTX_ISDB_S_SYSTEM:
			if (!px4_device_params.s_tuner_no_sleep)
				rt710_sleep(&chrdev4->tuner.rt710);

			tc90522_sleep_s(&chrdev4->tc90522, true);
			break;

		default:
			break;
		}
	}

	if (px4->mldev)
		px4_mldev_set_power(px4->mldev, px4, chrdev->id, false, NULL);

	if (kref_put(&px4->kref, px4_device_release))
		return 0;

	mutex_unlock(&px4->lock);
	return 0;
}

static int px4_chrdev_tune_t(struct ptx_chrdev *chrdev,
			     struct ptx_tune_params *params)
{
	int ret = 0, i;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct px4_chrdev *chrdev4 = chrdev->priv;
	struct px4_device *px4 = chrdev4->parent;
	struct tc90522_demod *tc90522 = &chrdev4->tc90522;
	struct r850_tuner *r850 = &chrdev4->tuner.r850;
	bool tuner_locked;

	dev_dbg(px4->dev,
		"px4_chrdev_tune_t %u:%u\n", chrdev_group->id, chrdev->id);

	if (params->system != PTX_ISDB_T_SYSTEM)
		return -EINVAL;

	ret = tc90522_write_reg(tc90522, 0x47, 0x30);
	if (ret)
		return ret;

	ret = tc90522_set_agc_t(tc90522, false);
	if (ret) {
		dev_err(px4->dev,
			"px4_chrdev_tune_t %u:%u: tc90522_set_agc_t(false) failed. (ret: %d)\n",
			chrdev_group->id, chrdev->id, ret);
		return ret;
	}

	ret = tc90522_write_reg(tc90522, 0x76, 0x0c);
	if (ret)
		return ret;

	ret = r850_set_frequency(r850, params->freq);
	if (ret) {
		dev_err(px4->dev,
			"px4_chrdev_tune_t %u:%u: r850_set_frequency(%u) failed. (ret: %d)\n",
			chrdev_group->id, chrdev->id, params->freq, ret);
		return ret;
	}

	i = 50;
	while (i--) {
		ret = r850_is_pll_locked(r850, &tuner_locked);
		if (!ret && tuner_locked)
			break;

		msleep(10);
	}

	if (ret) {
		dev_err(px4->dev,
			"px4_chrdev_tune_t %u:%u: r850_is_pll_locked() failed. (ret: %d)\n",
			chrdev_group->id, chrdev->id, ret);
		return ret;
	} else if (!tuner_locked) {
		/* PLL error */
		dev_dbg(px4->dev,
			"px4_chrdev_tune_t %u:%u: PLL is NOT locked.\n",
			chrdev_group->id, chrdev->id);
		ret = -EAGAIN;
		return ret;
	}

	dev_dbg(px4->dev,
		"px4_chrdev_tune_t %u:%u: PLL is locked. count: %d\n",
		chrdev_group->id, chrdev->id, i);

	ret = tc90522_set_agc_t(tc90522, true);
	if (ret) {
		dev_err(px4->dev,
			"px4_chrdev_tune_t %u:%u: tc90522_set_agc_t(true) failed. (ret: %d)\n",
			chrdev_group->id, chrdev->id, ret);
		return ret;
	}

	ret = tc90522_write_reg(tc90522, 0x71, 0x21);
	if (ret)
		return ret;

	ret = tc90522_write_reg(tc90522, 0x72, 0x25);
	if (ret)
		return ret;

	ret = tc90522_write_reg(tc90522, 0x75, 0x08);
	if (ret)
		return ret;

	return 0;
}

static int px4_chrdev_tune_s(struct ptx_chrdev *chrdev,
			     struct ptx_tune_params *params)
{
	int ret = 0, i;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct px4_chrdev *chrdev4 = chrdev->priv;
	struct px4_device *px4 = chrdev4->parent;
	struct tc90522_demod *tc90522 = &chrdev4->tc90522;
	struct rt710_tuner *rt710 = &chrdev4->tuner.rt710;
	bool tuner_locked;
	s32 ss = 0;

	dev_dbg(px4->dev,
		"px4_chrdev_tune_s %u:%u\n", chrdev_group->id, chrdev->id);

	if (params->system != PTX_ISDB_S_SYSTEM)
		return -EINVAL;

	/* set frequency */

	ret = tc90522_set_agc_s(tc90522, false);
	if (ret) {
		dev_err(px4->dev,
			"px4_chrdev_tune_s %u:%u: tc90522_set_agc_s(false) failed. (ret: %d)\n",
			chrdev_group->id, chrdev->id, ret);
		return ret;
	}

	ret = tc90522_write_reg(tc90522, 0x8e, 0x06/*0x02*/);
	if (ret)
		return ret;

	ret = tc90522_write_reg(tc90522, 0xa3, 0xf7);
	if (ret)
		return ret;

	ret = rt710_set_params(rt710, params->freq, 28860, 4);
	if (ret) {
		dev_err(px4->dev,
			"px4_chrdev_tune_s %u:%u: rt710_set_params(%u, 28860, 4) failed. (ret: %d)\n",
			chrdev_group->id, chrdev->id, params->freq, ret);
		return ret;
	}

	i = 50;
	while (i--) {
		ret = rt710_is_pll_locked(rt710, &tuner_locked);
		if (!ret && tuner_locked)
			break;

		msleep(10);
	}

	if (ret) {
		dev_err(px4->dev,
			"px4_chrdev_tune_s %u:%u: rt710_is_pll_locked() failed. (ret: %d)\n",
			chrdev_group->id, chrdev->id, ret);
		return ret;
	} else if (!tuner_locked) {
		/* PLL error */
		dev_err(px4->dev,
			"px4_chrdev_tune_s %u:%u: PLL is NOT locked.\n",
			chrdev_group->id, chrdev->id);
		ret = -EAGAIN;
		return ret;
	}

	rt710_get_rf_signal_strength(rt710, &ss);
	dev_dbg(px4->dev,
		"px4_chrdev_tune_s %u:%u: PLL is locked. count: %d, signal strength: %d.%03ddBm\n",
		chrdev_group->id, chrdev->id, i, ss / 1000, -ss % 1000);

	ret = tc90522_set_agc_s(tc90522, true);
	if (ret) {
		dev_err(px4->dev,
			"px4_chrdev_tune_s %u:%u: tc90522_set_agc_s(true) failed. (ret: %d)\n",
			chrdev_group->id, chrdev->id, ret);
		return ret;
	}

	return 0;
}

static int px4_chrdev_check_lock_t(struct ptx_chrdev *chrdev, bool *locked)
{
	struct px4_chrdev *chrdev4 = chrdev->priv;

	return tc90522_is_signal_locked_t(&chrdev4->tc90522, locked);
}

static int px4_chrdev_check_lock_s(struct ptx_chrdev *chrdev, bool *locked)
{
	struct px4_chrdev *chrdev4 = chrdev->priv;

	return tc90522_is_signal_locked_s(&chrdev4->tc90522, locked);
}

static int px4_chrdev_set_stream_id_s(struct ptx_chrdev *chrdev, u16 stream_id)
{
	int ret = 0, i;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct px4_chrdev *chrdev4 = chrdev->priv;
	struct px4_device *px4 = chrdev4->parent;
	struct tc90522_demod *tc90522 = &chrdev4->tc90522;
	u16 tsid, tsid2;

	dev_dbg(px4->dev,
		"px4_chrdev_set_stream_id_s %u:%u\n",
		chrdev_group->id, chrdev->id);

	if (stream_id < 12) {
		i = 100;
		while (i--) {
			ret = tc90522_tmcc_get_tsid_s(tc90522,
						      stream_id, &tsid);
			if ((!ret && tsid) || ret == -EINVAL)
				break;

			msleep(10);
		}

		if (ret) {
			dev_err(px4->dev,
				"px4_chrdev_set_stream_id_s %u:%u: tc90522_tmcc_get_tsid_s() failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id, ret);
			return ret;
		}

		if (!tsid) {
			ret = -EAGAIN;
			return ret;
		}
	} else {
		tsid = stream_id;
	}

	ret = tc90522_set_tsid_s(tc90522, tsid);
	if (ret) {
		dev_err(px4->dev,
			"px4_chrdev_set_stream_id_s %u:%u: tc90522_set_tsid_s(0x%x) failed. (ret: %d)\n",
			chrdev_group->id, chrdev->id, tsid, ret);
		return ret;
	}

	/* check slot */

	i = 100;
	while(i--) {
		ret = tc90522_get_tsid_s(tc90522, &tsid2);
		if (!ret && tsid2 == tsid)
			break;

		msleep(10);
	}

	if (tsid2 != tsid)
		ret = -EAGAIN;

	return ret;
}

static int px4_chrdev_set_lnb_voltage_s(struct ptx_chrdev *chrdev, int voltage)
{
	int ret = 0;
	struct px4_chrdev *chrdev4 = chrdev->priv;
	struct px4_device *px4 = chrdev4->parent;

	dev_dbg(px4->dev,
		"px4_chrdev_set_lnb_voltage_s %u:%u voltage: %d\n",
		chrdev->parent->id, chrdev->id, voltage);

	if (voltage != 0 && voltage != 15)
		return -EINVAL;

	if (chrdev4->lnb_power == !!voltage)
		return 0;

	if (!voltage && !atomic_read(&px4->available))
		return 0;

	mutex_lock(&px4->lock);

	if (!voltage)
		px4->lnb_power_count--;

	if (!px4->lnb_power_count) {
		ret = it930x_write_gpio(&px4->it930x, 11, !!voltage);
		if (ret && voltage)
			goto exit;
	}

	if (voltage)
		px4->lnb_power_count++;

	chrdev4->lnb_power = !!voltage;

exit:
	mutex_unlock(&px4->lock);
	return ret;
}

static int px4_chrdev_start_capture(struct ptx_chrdev *chrdev)
{
	int ret = 0;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct px4_chrdev *chrdev4 = chrdev->priv;
	struct px4_device *px4 = chrdev4->parent;
	struct tc90522_demod *tc90522 = &chrdev4->tc90522;

	dev_dbg(px4->dev,
		"px4_chrdev_start_capture %u:%u\n",
		chrdev_group->id, chrdev->id);

	mutex_lock(&px4->lock);

	if (!px4->streaming_count) {
		ret = it930x_purge_psb(&px4->it930x,
				       px4_device_params.psb_purge_timeout);
		if (ret) {
			dev_err(px4->dev,
				"px4_chrdev_start_capture %u:%u: it930x_purge_psb() failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id, ret);
			goto fail;
		}
	}

	switch (chrdev->system_cap) {
	case PTX_ISDB_T_SYSTEM:
		ret = tc90522_enable_ts_pins_t(tc90522, true);
		if (ret)
			dev_err(px4->dev,
				"px4_chrdev_start_capture %u:%u: tc90522_enable_ts_pins_t(true) failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id, ret);

		break;

	case PTX_ISDB_S_SYSTEM:
		ret = tc90522_enable_ts_pins_s(tc90522, true);
		if (ret)
			dev_err(px4->dev,
				"px4_chrdev_start_capture %u:%u: tc90522_enable_ts_pins_s(true) failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id, ret);

		break;

	default:
		break;
	}

	if (ret)
		goto fail;

	if (!px4->streaming_count) {
		struct px4_stream_context *stream_ctx = px4->stream_ctx;

		stream_ctx->remain_len = 0;

		ret = itedtv_bus_start_streaming(&px4->it930x.bus,
						 px4_device_stream_handler,
						 stream_ctx);
		if (ret) {
			dev_err(px4->dev,
				"px4_chrdev_start_capture %u:%u: itedtv_bus_start_streaming() failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id, ret);
				goto fail_bus;
		}
	}

	px4->streaming_count++;

	dev_dbg(px4->dev,
		"px4_chrdev_start_capture %u:%u: streaming_count: %u\n",
		chrdev_group->id, chrdev->id, px4->streaming_count);

	mutex_unlock(&px4->lock);
	return 0;

fail_bus:
	switch (chrdev->system_cap) {
	case PTX_ISDB_T_SYSTEM:
		tc90522_enable_ts_pins_t(tc90522, false);
		break;

	case PTX_ISDB_S_SYSTEM:
		tc90522_enable_ts_pins_s(tc90522, false);
		break;

	default:
		break;
	}

fail:
	mutex_unlock(&px4->lock);
	return ret;
}

static int px4_chrdev_stop_capture(struct ptx_chrdev *chrdev)
{
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct px4_chrdev *chrdev4 = chrdev->priv;
	struct px4_device *px4 = chrdev4->parent;
	struct tc90522_demod *tc90522 = &chrdev4->tc90522;

	dev_dbg(px4->dev,
		"px4_chrdev_stop_capture %u:%u\n",
		chrdev_group->id, chrdev->id);

	mutex_lock(&px4->lock);

	if (!px4->streaming_count) {
		mutex_unlock(&px4->lock);
		return -EALREADY;
	}

	px4->streaming_count--;
	if (!px4->streaming_count) {
		dev_dbg(px4->dev,
			"px4_chrdev_stop_capture %u:%u: stopping...\n",
			chrdev_group->id, chrdev->id);
		itedtv_bus_stop_streaming(&px4->it930x.bus);
	} else {
		dev_dbg(px4->dev,
			"px4_chrdev_stop_capture %u:%u: streaming_count: %u\n",
			chrdev_group->id, chrdev->id, px4->streaming_count);
	}

	mutex_unlock(&px4->lock);

	if (!atomic_read(&px4->available))
		return 0;

	switch (chrdev->system_cap) {
	case PTX_ISDB_T_SYSTEM:
		tc90522_enable_ts_pins_t(tc90522, false);
		break;

	case PTX_ISDB_S_SYSTEM:
		tc90522_enable_ts_pins_s(tc90522, false);
		break;

	default:
		break;
	}

	return 0;
}

static int px4_chrdev_set_capture(struct ptx_chrdev *chrdev, bool status)
{
	return (status) ? px4_chrdev_start_capture(chrdev)
			: px4_chrdev_stop_capture(chrdev);
}

static int px4_chrdev_read_cnr_raw_t(struct ptx_chrdev *chrdev, u32 *value)
{
	struct px4_chrdev *chrdev4 = chrdev->priv;

	return tc90522_get_cndat_t(&chrdev4->tc90522, value);
}

static int px4_chrdev_read_cnr_raw_s(struct ptx_chrdev *chrdev, u32 *value)
{
	struct px4_chrdev *chrdev4 = chrdev->priv;

	return tc90522_get_cn_s(&chrdev4->tc90522, (u16 *)value);
}

static struct ptx_chrdev_operations px4_chrdev_t_ops = {
	.init = px4_chrdev_init,
	.term = px4_chrdev_term_t,
	.open = px4_chrdev_open,
	.release = px4_chrdev_release,
	.tune = px4_chrdev_tune_t,
	.check_lock = px4_chrdev_check_lock_t,
	.set_stream_id = NULL,
	.set_lnb_voltage = NULL,
	.set_capture = px4_chrdev_set_capture,
	.read_signal_strength = NULL,
	.read_cnr = NULL,
	.read_cnr_raw = px4_chrdev_read_cnr_raw_t
};

static struct ptx_chrdev_operations px4_chrdev_s_ops = {
	.init = px4_chrdev_init,
	.term = px4_chrdev_term_s,
	.open = px4_chrdev_open,
	.release = px4_chrdev_release,
	.tune = px4_chrdev_tune_s,
	.check_lock = px4_chrdev_check_lock_s,
	.set_stream_id = px4_chrdev_set_stream_id_s,
	.set_lnb_voltage = px4_chrdev_set_lnb_voltage_s,
	.set_capture = px4_chrdev_set_capture,
	.read_signal_strength = NULL,
	.read_cnr = NULL,
	.read_cnr_raw = px4_chrdev_read_cnr_raw_s
};

static int px4_parse_serial_number(struct px4_serial_number *serial,
				   const char *dev_serial)
{
	int ret = 0;

	if (strlen(dev_serial) != 15)
		return -EINVAL;

	ret = kstrtoull(dev_serial, 10, &serial->serial_number);
	if (ret)
		return ret;

	serial->dev_id = do_div(serial->serial_number, 10);

	return 0;
}

static int px4_device_load_config(struct px4_device *px4,
				  struct ptx_chrdev_config *chrdev_config)
{
	int ret = 0, i;
	struct device *dev = px4->dev;
	struct it930x_bridge *it930x = &px4->it930x;
	u8 tmp;

	ret = it930x_read_reg(it930x, 0x4979, &tmp);
	if (ret) {
		dev_err(dev,
			"px4_device_load_config: it930x_read_reg(0x4979) failed. (ret: %d)\n",
			ret);
		return ret;
	} else if (!tmp) {
		dev_warn(dev, "EEPROM error.\n");
		return ret;
	}

	chrdev_config[0].system_cap = PTX_ISDB_S_SYSTEM;
	chrdev_config[1].system_cap = PTX_ISDB_S_SYSTEM;
	chrdev_config[2].system_cap = PTX_ISDB_T_SYSTEM;
	chrdev_config[3].system_cap = PTX_ISDB_T_SYSTEM;

	it930x->config.input[0].i2c_addr = 0x11;
	it930x->config.input[1].i2c_addr = 0x13;
	it930x->config.input[2].i2c_addr = 0x10;
	it930x->config.input[3].i2c_addr = 0x12;

	for (i = 0; i < PX4_CHRDEV_NUM; i++) {
		struct it930x_stream_input *input = &it930x->config.input[i];
		struct px4_chrdev *chrdev4 = &px4->chrdev4[i];

		input->enable = true;
		input->is_parallel = false;
		input->port_number = i + 1;
		input->slave_number = i;
		input->i2c_bus = 2;
		input->packet_len = 188;
		input->sync_byte = ((i + 1) << 4) | 0x07;	/* 0x17 0x27 0x37 0x47 */

		chrdev4->tc90522.dev = dev;
		chrdev4->tc90522.i2c = &it930x->i2c_master[1];
		chrdev4->tc90522.i2c_addr = input->i2c_addr;
		chrdev4->tc90522.is_secondary = (i % 2) ? true : false;

		switch (chrdev_config[i].system_cap) {
		case PTX_ISDB_S_SYSTEM:
			chrdev4->tuner.rt710.dev = dev;
			chrdev4->tuner.rt710.i2c = &chrdev4->tc90522.i2c_master;
			chrdev4->tuner.rt710.i2c_addr = 0x7a;
			chrdev4->tuner.rt710.config.xtal = 24000;
			chrdev4->tuner.rt710.config.loop_through = false;
			chrdev4->tuner.rt710.config.clock_out = false;
			chrdev4->tuner.rt710.config.signal_output_mode = RT710_SIGNAL_OUTPUT_DIFFERENTIAL;
			chrdev4->tuner.rt710.config.agc_mode = RT710_AGC_POSITIVE;
			chrdev4->tuner.rt710.config.vga_atten_mode = RT710_VGA_ATTEN_OFF;
			chrdev4->tuner.rt710.config.fine_gain = RT710_FINE_GAIN_3DB;
			chrdev4->tuner.rt710.config.scan_mode = RT710_SCAN_MANUAL;
			break;

		case PTX_ISDB_T_SYSTEM:
			chrdev4->tuner.r850.dev = dev;
			chrdev4->tuner.r850.i2c = &chrdev4->tc90522.i2c_master;
			chrdev4->tuner.r850.i2c_addr = 0x7c;
			chrdev4->tuner.r850.config.xtal = 24000;
			chrdev4->tuner.r850.config.loop_through = (i % 2) ? false
									  : true;
			chrdev4->tuner.r850.config.clock_out = false;
			chrdev4->tuner.r850.config.no_imr_calibration = true;
			chrdev4->tuner.r850.config.no_lpf_calibration = true;
			break;

		default:
			break;
		}
	}

	it930x->config.input[4].enable = false;
	it930x->config.input[4].port_number = 0;

	return 0;
}

int px4_device_init(struct px4_device *px4, struct device *dev,
		    const char *dev_serial, bool use_mldev,
		    struct ptx_chrdev_context *chrdev_ctx,
		    struct completion *quit_completion)
{
	int ret = 0, i;
	struct it930x_bridge *it930x;
	struct itedtv_bus *bus;
	struct ptx_chrdev_config chrdev_config[PX4_CHRDEV_NUM];
	struct ptx_chrdev_group_config chrdev_group_config;
	struct ptx_chrdev_group *chrdev_group;
	struct px4_stream_context *stream_ctx;

	if (!px4 || !dev || !dev_serial || !chrdev_ctx || !quit_completion)
		return -EINVAL;

	dev_dbg(dev,
		"px4_device_init: use_mldev: %s\n",
		(use_mldev) ? "true" : "false");

	get_device(dev);

	mutex_init(&px4->lock);
	kref_init(&px4->kref);
	px4->dev = dev;
	px4->mldev = NULL;
	px4->quit_completion = quit_completion;
	px4->open_count = 0;
	px4->lnb_power_count = 0;
	px4->streaming_count = 0;

	for (i = 0; i < PX4_CHRDEV_NUM; i++) {
		struct px4_chrdev *chrdev4 = &px4->chrdev4[i];

		chrdev4->chrdev = NULL;
		chrdev4->parent = px4;
		chrdev4->lnb_power = false;
	}

	ret = px4_parse_serial_number(&px4->serial, dev_serial);
	if (ret) {
		dev_err(px4->dev,
			"px4_device_init: px4_parse_serial_number() failed. (ret: %d)\n",
			ret);
		goto fail;
	}

	dev_dbg(px4->dev, "px4_device_init: serial_number: %014llu\n",
		px4->serial.serial_number);
	dev_dbg(px4->dev, "px4_device_init: dev_id: %u\n", px4->serial.dev_id);

	if (!px4->serial.dev_id || px4->serial.dev_id > 2)
		dev_warn(px4->dev,
			 "px4_device_init: Unexpected device id: %u\n",
			 px4->serial.dev_id);

	stream_ctx = kzalloc(sizeof(*stream_ctx), GFP_KERNEL);
	if (!stream_ctx) {
		dev_err(px4->dev,
			"px4_device_init: kzalloc(sizeof(*stream_ctx), GFP_KERNEL) failed.\n");
		ret = -ENOMEM;
		goto fail;
	}
	px4->stream_ctx = stream_ctx;

	it930x = &px4->it930x;
	bus = &it930x->bus;

	ret = itedtv_bus_init(bus);
	if (ret)
		goto fail_bus;

	ret = it930x_init(it930x);
	if (ret)
		goto fail_bridge;

	ret = it930x_raise(it930x);
	if (ret)
		goto fail_device;

	ret = px4_device_load_config(px4, chrdev_config);
	if (ret)
		goto fail_device;

	for (i = 0; i < PX4_CHRDEV_NUM; i++) {
		switch (chrdev_config[i].system_cap) {
		case PTX_ISDB_T_SYSTEM:
			chrdev_config[i].ops = &px4_chrdev_t_ops;
			chrdev_config[i].options = PTX_CHRDEV_WAIT_AFTER_LOCK_TC_T;
			break;

		case PTX_ISDB_S_SYSTEM:
			chrdev_config[i].ops = &px4_chrdev_s_ops;
			chrdev_config[i].options = 0;
			break;

		default:
			ret = -EINVAL;
			break;
		}

		if (ret)
			goto fail_device;

		chrdev_config[i].ringbuf_size = 188 * px4_device_params.tsdev_max_packets;
		chrdev_config[i].ringbuf_threshold_size = chrdev_config[i].ringbuf_size / 10;
		chrdev_config[i].priv = &px4->chrdev4[i];
	}

	ret = it930x_load_firmware(it930x, IT930X_FIRMWARE_FILENAME);
	if (ret)
		goto fail_device;

	ret = it930x_init_warm(it930x);
	if (ret)
		goto fail_device;

	/* GPIO */
	ret = it930x_set_gpio_mode(it930x, 7, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	ret = it930x_set_gpio_mode(it930x, 2, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	if (use_mldev) {
		if (px4_mldev_search(px4->serial.serial_number, &px4->mldev))
			ret = px4_mldev_add(px4->mldev, px4);
		else
			ret = px4_mldev_alloc(&px4->mldev,
					      px4_device_params.multi_device_power_control_mode,
					      px4, px4_backend_set_power);

		if (ret)
			goto fail_device;
	} else {
		ret = it930x_write_gpio(it930x, 7, true);
		if (ret)
			goto fail_device;

		ret = it930x_write_gpio(it930x, 2, false);
		if (ret)
			goto fail_device;
	}

	ret = it930x_set_gpio_mode(it930x, 11, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	/* LNB power supply: off */
	ret = it930x_write_gpio(it930x, 11, false);
	if (ret)
		goto fail_device;

	if (px4_device_params.discard_null_packets) {
		struct it930x_pid_filter filter;

		filter.block = true;
		filter.num = 1;
		filter.pid[0] = 0x1fff;

		for (i = 0; i < PX4_CHRDEV_NUM; i++) {
			ret = it930x_set_pid_filter(it930x, i, &filter);
			if (ret)
				goto fail_device;
		}
	}

	chrdev_group_config.owner_kref = &px4->kref;
	chrdev_group_config.owner_kref_release = px4_device_release;
	chrdev_group_config.reserved = false;
	chrdev_group_config.minor_base = 0;	/* unused */
	chrdev_group_config.chrdev_num = 4;
	chrdev_group_config.chrdev_config = chrdev_config;

	ret = ptx_chrdev_context_add_group(chrdev_ctx, dev,
					   &chrdev_group_config, &chrdev_group);
	if (ret)
		goto fail_chrdev;

	px4->chrdev_group = chrdev_group;

	for (i = 0; i < PX4_CHRDEV_NUM; i++) {
		px4->chrdev4[i].chrdev = &chrdev_group->chrdev[i];
		stream_ctx->chrdev[i] = &chrdev_group->chrdev[i];
	}

	atomic_set(&px4->available, 1);
	return 0;

fail_chrdev:

fail_device:
	if (px4->mldev)
		px4_mldev_remove(px4->mldev, px4);

	it930x_term(it930x);

fail_bridge:
	itedtv_bus_term(bus);

fail_bus:
	kfree(px4->stream_ctx);

fail:
	mutex_destroy(&px4->lock);
	put_device(dev);

	return ret;
}

static void px4_device_release(struct kref *kref)
{
	struct px4_device *px4 = container_of(kref, struct px4_device, kref);

	dev_dbg(px4->dev, "px4_device_release\n");

	if (px4->mldev)
		px4_mldev_remove(px4->mldev, px4);

	it930x_term(&px4->it930x);
	itedtv_bus_term(&px4->it930x.bus);

	kfree(px4->stream_ctx);

	mutex_destroy(&px4->lock);
	put_device(px4->dev);

	complete(px4->quit_completion);
	return;
}

void px4_device_term(struct px4_device *px4)
{
	dev_dbg(px4->dev,
		"px4_device_term: kref count: %u\n", kref_read(&px4->kref));

	atomic_xchg(&px4->available, 0);
	ptx_chrdev_group_destroy(px4->chrdev_group);

	kref_put(&px4->kref, px4_device_release);
	return;
}
