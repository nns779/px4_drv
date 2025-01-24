// SPDX-License-Identifier: GPL-2.0-only
/*
 * PTX driver for Digibest PLEX PX-S1UR device (s1ur_device.c)
 *
 * Copyright (c) 2023 techma.
 */

#include "print_format.h"
#include "s1ur_device.h"

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/slab.h>

#include "px4_device_params.h"
#include "firmware.h"

#define S1UR_DEVICE_TS_SYNC_COUNT	4
#define S1UR_DEVICE_TS_SYNC_SIZE	(188 * S1UR_DEVICE_TS_SYNC_COUNT)

struct s1ur_stream_context {
	struct ptx_chrdev *chrdev;
	u8 remain_buf[S1UR_DEVICE_TS_SYNC_SIZE];
	size_t remain_len;
};

static void s1ur_device_release(struct kref *kref);

static int s1ur_backend_set_power(struct s1ur_device *s1ur,
				      bool state)
{
	int ret = 0;
	struct it930x_bridge *it930x = &s1ur->it930x;

	dev_dbg(s1ur->dev,
		"s1ur_backend_set_power: %s\n", (state) ? "true" : "false");

	if (!state && !atomic_read(&s1ur->available))
		return 0;

	if (state) {
		ret = it930x_write_gpio(it930x, 3, false);
		if (ret)
			return ret;

		msleep(100);

		ret = it930x_write_gpio(it930x, 2, true);
		if (ret)
			return ret;

		msleep(20);
	} else {
		it930x_write_gpio(it930x, 2, false);
		it930x_write_gpio(it930x, 3, true);
	}

	return 0;
}

static int s1ur_backend_init(struct s1ur_device *s1ur)
{
	int ret = 0;
	struct s1ur_chrdev *chrdevs1ur = &s1ur->chrdevs1ur;

	ret = tc90522_init(&chrdevs1ur->tc90522_t);
	if (ret) {
		dev_err(s1ur->dev,
			"s1ur_backend_init: tc90522_init() (t) failed. (ret: %d)\n",
			ret);
		return ret;
	}

	ret = tc90522_init(&chrdevs1ur->tc90522_s);
	if (ret) {
		dev_err(s1ur->dev,
			"s1ur_backend_init: tc90522_init() (s) failed. (ret: %d)\n",
			ret);
		return ret;
	}

	ret = r850_init(&chrdevs1ur->r850);
	if (ret) {
		dev_err(s1ur->dev,
			"s1ur_backend_init: r850_init() failed. (ret: %d)\n",
			ret);
		return ret;
	}

#if 0
	ret = rt710_init(&chrdevs1ur->rt710);
	if (ret) {
		dev_err(s1ur->dev,
			"s1ur_backend_init: rt710_init() failed. (ret: %d)\n",
			ret);
		return ret;
	}
#endif
	return 0;
}

static int s1ur_backend_term(struct s1ur_device *s1ur)
{
	struct s1ur_chrdev *chrdevs1ur = &s1ur->chrdevs1ur;

	r850_term(&chrdevs1ur->r850);
#if 0
	rt710_term(&chrdevs1ur->rt710);
#endif
	tc90522_term(&chrdevs1ur->tc90522_t);
	tc90522_term(&chrdevs1ur->tc90522_s);

	return 0;
}

static void s1ur_device_stream_process(struct ptx_chrdev *chrdev,
					   u8 **buf, u32 *len)
{
	u8 *p = *buf;
	u32 remain = *len;

	while (likely(remain)) {
		u32 i = 0;
		bool sync_remain = false;

		while (true) {
			if (likely(((i + 1) * 188) <= remain)) {
				if (unlikely(p[i * 188] != 0x47))
					break;
			} else {
				sync_remain = true;
				break;
			}
			i++;
		}

		if (unlikely(i < S1UR_DEVICE_TS_SYNC_COUNT)) {
			p++;
			remain--;
			continue;
		}

		ptx_chrdev_put_stream(chrdev, p, 188 * i);

		p += 188 * i;
		remain -= 188 * i;

		if (unlikely(sync_remain))
			break;
	}

	*buf = p;
	*len = remain;

	return;
}

static int s1ur_device_stream_handler(void *context, void *buf, u32 len)
{
	struct s1ur_stream_context *stream_ctx = context;
	u8 *ctx_remain_buf = stream_ctx->remain_buf;
	u32 ctx_remain_len = stream_ctx->remain_len;
	u8 *p = buf;
	u32 remain = len;

	if (unlikely(ctx_remain_len)) {
		if (likely((ctx_remain_len + len) >= S1UR_DEVICE_TS_SYNC_SIZE)) {
			u32 t = S1UR_DEVICE_TS_SYNC_SIZE - ctx_remain_len;

			memcpy(ctx_remain_buf + ctx_remain_len, p, t);
			ctx_remain_len = S1UR_DEVICE_TS_SYNC_SIZE;

			s1ur_device_stream_process(stream_ctx->chrdev,
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

	s1ur_device_stream_process(stream_ctx->chrdev, &p, &remain);

	if (unlikely(remain)) {
		memcpy(stream_ctx->remain_buf, p, remain);
		stream_ctx->remain_len = remain;
	}

	return 0;
}

static int s1ur_chrdev_init(struct ptx_chrdev *chrdev)
{
	dev_dbg(chrdev->parent->dev, "s1ur_chrdev_init\n");

	chrdev->params.system = PTX_UNSPECIFIED_SYSTEM;
	return 0;
}

static int s1ur_chrdev_term(struct ptx_chrdev *chrdev)
{
	dev_dbg(chrdev->parent->dev, "s1ur_chrdev_term\n");
	return 0;
}

static struct tc90522_regbuf tc_init_t[] = {
	{ 0xb0, NULL, { 0xa0 } },
	{ 0xb2, NULL, { 0x3d } },
	{ 0xb3, NULL, { 0x25 } },
	{ 0xb4, NULL, { 0x8b } },
	{ 0xb5, NULL, { 0x4b } },
	{ 0xb6, NULL, { 0x3f } },
	{ 0xb7, NULL, { 0xff } },
	{ 0xb8, NULL, { 0xc0 } },
};

#if 0
static struct tc90522_regbuf tc_init_s[] = {
	{ 0x15, NULL, { 0x00 } },
	{ 0x1d, NULL, { 0x00 } },
};
#endif

static int s1ur_chrdev_open(struct ptx_chrdev *chrdev)
{
	int ret = 0;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct s1ur_chrdev *chrdevs1ur = chrdev->priv;
	struct s1ur_device *s1ur = container_of(chrdevs1ur,
							struct s1ur_device,
							chrdevs1ur);
	struct r850_system_config sys;

	dev_dbg(s1ur->dev,
		"s1ur_chrdev_open %u\n", chrdev_group->id);

	ret = s1ur_backend_set_power(s1ur, true);
	if (ret) {
		dev_err(s1ur->dev,
			"s1ur_chrdev_open %u: s1ur_backend_set_power(true) failed. (ret: %d)\n",
			chrdev_group->id, ret);
		goto fail_backend_power;
	}

	ret = s1ur_backend_init(s1ur);
	if (ret) {
		dev_err(s1ur->dev,
			"s1ur_chrdev_open %u: s1ur_backend_init() failed. (ret: %d)\n",
			chrdev_group->id, ret);
		goto fail_backend_init;
	}

	/* Initialization for ISDB-T */

	ret = tc90522_write_multiple_regs(&chrdevs1ur->tc90522_t,
					  tc_init_t, ARRAY_SIZE(tc_init_t));
	if (ret) {
		dev_err(s1ur->dev,
			"s1ur_chrdev_open %u: tc90522_write_multiple_regs(tc_init_t) failed. (ret: %d)\n",
			chrdev_group->id, ret);
		goto fail_backend;
	}

	/* disable ts pins */
	ret = tc90522_enable_ts_pins_t(&chrdevs1ur->tc90522_t, false);
	if (ret) {
		dev_err(s1ur->dev,
			"s1ur_chrdev_open %u: tc90522_enable_ts_pins_t(false) failed. (ret: %d)\n",
			chrdev_group->id, ret);
		return ret;
	}

	/* sleep */
	ret = tc90522_sleep_t(&chrdevs1ur->tc90522_t, true);
	if (ret) {
		dev_err(s1ur->dev,
			"s1ur_chrdev_open %u: tc90522_sleep_t(true) failed. (ret: %d)\n",
			chrdev_group->id, ret);
		return ret;
	}

	sys.system = R850_SYSTEM_ISDB_T;
	sys.bandwidth = R850_BANDWIDTH_6M;
	sys.if_freq = 4063;

	ret = r850_set_system(&chrdevs1ur->r850, &sys);
	if (ret) {
		dev_err(s1ur->dev,
			"s1ur_chrdev_open %u: r850_set_system() failed. (ret: %d)\n",
			chrdev_group->id, ret);
		return ret;
	}

#if 0
	/* Initialization for ISDB-S */

	ret = tc90522_write_multiple_regs(&chrdevs1ur->tc90522_s,
					  tc_init_s, ARRAY_SIZE(tc_init_s));
	if (ret) {
		dev_err(s1ur->dev,
			"s1ur_chrdev_open %u: tc90522_write_multiple_regs(tc_init_s) failed. (ret: %d)\n",
			chrdev_group->id, ret);
		return ret;
	}

	/* disable ts pins */
	ret = tc90522_enable_ts_pins_s(&chrdevs1ur->tc90522_s, false);
	if (ret) {
		dev_err(s1ur->dev,
			"s1ur_chrdev_open %u: tc90522_enable_ts_pins_s(false) failed. (ret: %d)\n",
			chrdev_group->id, ret);
		return ret;
	}

	/* sleep */
	ret = tc90522_sleep_s(&chrdevs1ur->tc90522_s, true);
	if (ret) {
		dev_err(s1ur->dev,
			"s1ur_chrdev_open %u: tc90522_sleep_s(true) failed. (ret: %d)\n",
			chrdev_group->id, ret);
		return ret;
	}
#endif
	kref_get(&s1ur->kref);
	return 0;

fail_backend:
	s1ur_backend_term(s1ur);

fail_backend_init:
	s1ur_backend_set_power(s1ur, false);

fail_backend_power:
	return ret;
}

static int s1ur_chrdev_release(struct ptx_chrdev *chrdev)
{
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct s1ur_chrdev *chrdevs1ur = chrdev->priv;
	struct s1ur_device *s1ur = container_of(chrdevs1ur,
							struct s1ur_device,
							chrdevs1ur);

	dev_dbg(s1ur->dev,
		"s1ur_chrdev_release %u: kref count: %u\n",
		chrdev_group->id, kref_read(&s1ur->kref));

	s1ur_backend_term(s1ur);
	s1ur_backend_set_power(s1ur, false);

	kref_put(&s1ur->kref, s1ur_device_release);
	return 0;
}

static int s1ur_chrdev_tune(struct ptx_chrdev *chrdev,
				struct ptx_tune_params *params)
{
	int ret = 0, i;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct s1ur_chrdev *chrdevs1ur = chrdev->priv;
	struct s1ur_device *s1ur = container_of(chrdevs1ur,
							struct s1ur_device,
							chrdevs1ur);
	bool tuner_locked;
	/* s32 ss; */

	dev_dbg(s1ur->dev,
		"s1ur_chrdev_tune %u\n", chrdev_group->id);

	switch (params->system) {
	case PTX_ISDB_T_SYSTEM:
		ret = tc90522_write_reg(&chrdevs1ur->tc90522_t, 0x47, 0x30);
		if (ret)
			break;

		ret = tc90522_set_agc_t(&chrdevs1ur->tc90522_t, false);
		if (ret) {
			dev_err(s1ur->dev,
				"s1ur_chrdev_tune %u: tc90522_set_agc_t(false) failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		}

		ret = tc90522_sleep_s(&chrdevs1ur->tc90522_s, true);
		if (ret) {
			dev_err(s1ur->dev,
				"s1ur_chrdev_tune %u: tc90522_sleep_s(true) failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		}

		ret = tc90522_write_reg(&chrdevs1ur->tc90522_t, 0x0e, 0x77);
		if (ret)
			break;

		ret = tc90522_write_reg(&chrdevs1ur->tc90522_t, 0x0f, 0x10);
		if (ret)
			break;

		ret = tc90522_write_reg(&chrdevs1ur->tc90522_t, 0x71, 0x20);
		if (ret)
			break;

		ret = tc90522_sleep_t(&chrdevs1ur->tc90522_t, false);
		if (ret) {
			dev_err(s1ur->dev,
				"s1ur_chrdev_tune %u: tc90522_sleep_t(false) failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		}

		ret = tc90522_write_reg(&chrdevs1ur->tc90522_t, 0x76, 0x0c);
		if (ret)
			break;

		ret = tc90522_write_reg(&chrdevs1ur->tc90522_t, 0x1f, 0x30);
		if (ret)
			break;

		ret = r850_wakeup(&chrdevs1ur->r850);
		if (ret) {
			dev_err(s1ur->dev,
				"s1ur_chrdev_tune %u: r850_wakeup() failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		}

		ret = r850_set_frequency(&chrdevs1ur->r850, params->freq);
		if (ret) {
			dev_err(s1ur->dev,
				"s1ur_chrdev_tune %u: r850_set_frequency(%u) failed. (ret: %d)\n",
				chrdev_group->id, params->freq, ret);
			break;
		}

		i = 50;
		while (i--) {
			ret = r850_is_pll_locked(&chrdevs1ur->r850,
						 &tuner_locked);
			if (!ret && tuner_locked)
				break;

			msleep(10);
		}

		if (ret) {
			dev_err(s1ur->dev,
				"s1ur_chrdev_tune %u: r850_is_pll_locked() failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		} else if (!tuner_locked) {
			/* PLL error */
			dev_dbg(s1ur->dev,
				"s1ur_chrdev_tune %u: PLL is NOT locked.\n",
				chrdev_group->id);
			ret = -EAGAIN;
			break;
		}

		dev_dbg(s1ur->dev,
			"s1ur_chrdev_tune %u: PLL is locked. count: %d\n",
			chrdev_group->id, i);

		ret = tc90522_set_agc_t(&chrdevs1ur->tc90522_t, true);
		if (ret) {
			dev_err(s1ur->dev,
				"s1ur_chrdev_tune %u: tc90522_set_agc_t(true) failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		}

		ret = tc90522_write_reg(&chrdevs1ur->tc90522_t, 0x71, 0x01);
		if (ret)
			break;

		ret = tc90522_write_reg(&chrdevs1ur->tc90522_t, 0x72, 0x25);
		if (ret)
			break;

		ret = tc90522_write_reg(&chrdevs1ur->tc90522_t, 0x75, 0x00);
		if (ret)
			break;

		msleep(100);

		break;

#if 0
	case PTX_ISDB_S_SYSTEM:
		ret = tc90522_set_agc_s(&chrdevs1ur->tc90522_s, false);
		if (ret) {
			dev_err(s1ur->dev,
				"s1ur_chrdev_tune %u: tc90522_set_agc_s(false) failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		}

		ret = tc90522_write_reg(&chrdevs1ur->tc90522_t, 0x0e, 0x11);
		if (ret)
			break;

		ret = tc90522_write_reg(&chrdevs1ur->tc90522_t, 0x0f, 0x70);
		if (ret)
			break;

		ret = tc90522_sleep_t(&chrdevs1ur->tc90522_t, true);
		if (ret) {
			dev_err(s1ur->dev,
				"s1ur_chrdev_tune %u: tc90522_sleep_t(true) failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		}

		ret = tc90522_write_reg(&chrdevs1ur->tc90522_s, 0x07, 0x77);
		if (ret)
			break;

		ret = tc90522_write_reg(&chrdevs1ur->tc90522_s, 0x08, 0x10);
		if (ret)
			break;

		ret = tc90522_sleep_s(&chrdevs1ur->tc90522_s, false);
		if (ret) {
			dev_err(s1ur->dev,
				"s1ur_chrdev_tune %u: tc90522_sleep_s(false) failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		}

		ret = tc90522_write_reg(&chrdevs1ur->tc90522_s, 0x04, 0x02);
		if (ret)
			break;

		ret = tc90522_write_reg(&chrdevs1ur->tc90522_s, 0x8e, 0x02);
		if (ret)
			break;

		ret = tc90522_write_reg(&chrdevs1ur->tc90522_t, 0x1f, 0x20);
		if (ret)
			break;

		ret = rt710_set_params(&chrdevs1ur->rt710,
				       params->freq, 28860, 4);
		if (ret) {
			dev_err(s1ur->dev,
				"s1ur_chrdev_tune %u: rt710_set_params(%u, 28860, 4) failed. (ret: %d)\n",
				chrdev_group->id, params->freq, ret);
			break;
		}

		i = 50;
		while (i--) {
			ret = rt710_is_pll_locked(&chrdevs1ur->rt710,
						  &tuner_locked);
			if (!ret && tuner_locked)
				break;

			msleep(10);
		}

		if (ret) {
			dev_err(s1ur->dev,
				"s1ur_chrdev_tune %u: rt710_is_pll_locked() failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		} else if (!tuner_locked) {
			/* PLL error */
			dev_err(s1ur->dev,
				"s1ur_chrdev_tune %u: PLL is NOT locked.\n",
				chrdev_group->id);
			ret = -EAGAIN;
			break;
		}

		rt710_get_rf_signal_strength(&chrdevs1ur->rt710, &ss);
		dev_dbg(s1ur->dev,
			"s1ur_chrdev_tune %u: PLL is locked. count: %d, signal strength: %d.%03ddBm\n",
			chrdev_group->id, i, ss / 1000, -ss % 1000);

		ret = tc90522_set_agc_s(&chrdevs1ur->tc90522_s, true);
		if (ret) {
			dev_err(s1ur->dev,
				"s1ur_chrdev_tune %u: tc90522_set_agc_s(true) failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		}

		break;
#endif
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int s1ur_chrdev_check_lock(struct ptx_chrdev *chrdev, bool *locked)
{
	int ret = 0;
	struct s1ur_chrdev *chrdevs1ur = chrdev->priv;

	switch (chrdev->current_system) {
	case PTX_ISDB_T_SYSTEM:
		ret = tc90522_is_signal_locked_t(&chrdevs1ur->tc90522_t,
						locked);
		break;
#if 0
	case PTX_ISDB_S_SYSTEM:
		ret = tc90522_is_signal_locked_s(&chrdevs1ur->tc90522_s,
						locked);
		break;
#endif
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int s1ur_chrdev_set_stream_id(struct ptx_chrdev *chrdev,
					 u16 stream_id)
{
	int ret = 0, i;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct s1ur_chrdev *chrdevs1ur = chrdev->priv;
	struct s1ur_device *s1ur = container_of(chrdevs1ur,
							struct s1ur_device,
							chrdevs1ur);
	struct tc90522_demod *tc90522_s = &chrdevs1ur->tc90522_s;
	u16 tsid, tsid2;

	dev_dbg(s1ur->dev,
		"s1ur_chrdev_set_stream_id %u\n", chrdev_group->id);

	if (chrdev->current_system != PTX_ISDB_S_SYSTEM)
		return -EINVAL;

	if (stream_id < 12) {
		i = 100;
		while (i--) {
			ret = tc90522_tmcc_get_tsid_s(tc90522_s,
						      stream_id, &tsid);
			if ((!ret && tsid) || ret == -EINVAL)
				break;

			msleep(10);
		}

		if (ret) {
			dev_err(s1ur->dev,
				"s1ur_chrdev_set_stream_id_s %u: tc90522_tmcc_get_tsid_s() failed. (ret: %d)\n",
				chrdev_group->id, ret);
			return ret;
		}

		if (!tsid) {
			ret = -EAGAIN;
			return ret;
		}
	} else {
		tsid = stream_id;
	}

	ret = tc90522_set_tsid_s(tc90522_s, tsid);
	if (ret) {
		dev_err(s1ur->dev,
			"s1ur_chrdev_set_stream_id_s %u: tc90522_set_tsid_s(0x%x) failed. (ret: %d)\n",
			chrdev_group->id, tsid, ret);
		return ret;
	}

	/* check slot */

	i = 100;
	while(i--) {
		ret = tc90522_get_tsid_s(tc90522_s, &tsid2);
		if (!ret && tsid2 == tsid)
			break;

		msleep(10);
	}

	if (tsid2 != tsid)
		ret = -EAGAIN;

	return ret;
}

static int s1ur_chrdev_start_capture(struct ptx_chrdev *chrdev)
{
	int ret = 0;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct s1ur_chrdev *chrdevs1ur = chrdev->priv;
	struct s1ur_device *s1ur = container_of(chrdevs1ur,
							struct s1ur_device,
							chrdevs1ur);
	struct s1ur_stream_context *stream_ctx = s1ur->stream_ctx;


	dev_dbg(s1ur->dev,
		"s1ur_chrdev_start_capture %u\n", chrdev_group->id);

	ret = it930x_purge_psb(&s1ur->it930x,
			       px4_device_params.psb_purge_timeout);
	if (ret) {
		dev_err(s1ur->dev,
			"s1ur_chrdev_start_capture %u: it930x_purge_psb() failed. (ret: %d)\n",
			chrdev_group->id, ret);
		goto fail;
	}

	switch (chrdev->current_system) {
	case PTX_ISDB_T_SYSTEM:
		ret = tc90522_enable_ts_pins_t(&chrdevs1ur->tc90522_t, true);
		if (ret)
			dev_err(s1ur->dev,
				"s1ur_chrdev_start_capture %u: tc90522_enable_ts_pins_t(true) failed. (ret: %d)\n",
				chrdev_group->id, ret);

		break;

#if 0
	case PTX_ISDB_S_SYSTEM:
		ret = tc90522_enable_ts_pins_s(&chrdevs1ur->tc90522_s, true);
		if (ret)
			dev_err(s1ur->dev,
				"s1ur_chrdev_start_capture %u: tc90522_enable_ts_pins_s(true) failed. (ret: %d)\n",
				chrdev_group->id, ret);

		break;
#endif

	default:
		break;
	}

	if (ret)
		goto fail_tc;

	stream_ctx->remain_len = 0;

	ret = itedtv_bus_start_streaming(&s1ur->it930x.bus,
					 s1ur_device_stream_handler,
					 stream_ctx);
	if (ret) {
		dev_err(s1ur->dev,
			"s1ur_chrdev_start_capture %u: itedtv_bus_start_streaming() failed. (ret: %d)\n",
			chrdev_group->id, ret);
			goto fail_bus;
	}

	return 0;

fail_bus:

fail_tc:
	switch (chrdev->current_system) {
	case PTX_ISDB_T_SYSTEM:
		tc90522_enable_ts_pins_t(&chrdevs1ur->tc90522_t, false);
		break;

#if 0
	case PTX_ISDB_S_SYSTEM:
		tc90522_enable_ts_pins_s(&chrdevs1ur->tc90522_s, false);
		break;
#endif

	default:
		break;
	}

fail:
	return ret;
}

static int s1ur_chrdev_stop_capture(struct ptx_chrdev *chrdev)
{
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct s1ur_chrdev *chrdevs1ur = chrdev->priv;
	struct s1ur_device *s1ur = container_of(chrdevs1ur,
							struct s1ur_device,
							chrdevs1ur);

	dev_dbg(s1ur->dev,
		"s1ur_chrdev_stop_capture %u\n", chrdev_group->id);

	itedtv_bus_stop_streaming(&s1ur->it930x.bus);

	if (!atomic_read(&s1ur->available))
		return 0;

	switch (chrdev->current_system) {
	case PTX_ISDB_T_SYSTEM:
		tc90522_enable_ts_pins_t(&chrdevs1ur->tc90522_t, false);
		break;

#if 0
	case PTX_ISDB_S_SYSTEM:
		tc90522_enable_ts_pins_s(&chrdevs1ur->tc90522_s, false);
		break;
#endif
	default:
		break;
	}

	return 0;
}

static int s1ur_chrdev_set_capture(struct ptx_chrdev *chrdev, bool status)
{
	return (status) ? s1ur_chrdev_start_capture(chrdev)
			: s1ur_chrdev_stop_capture(chrdev);
}

static int s1ur_chrdev_read_cnr_raw(struct ptx_chrdev *chrdev, u32 *value)
{
	int ret = 0;
	struct s1ur_chrdev *chrdevs1ur = chrdev->priv;

	switch (chrdev->current_system) {
	case PTX_ISDB_T_SYSTEM:
		ret = tc90522_get_cndat_t(&chrdevs1ur->tc90522_t, value);
		break;

#if 0
	case PTX_ISDB_S_SYSTEM:
		ret = tc90522_get_cn_s(&chrdevs1ur->tc90522_s, (u16 *)value);
		break;
#endif
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static struct ptx_chrdev_operations s1ur_chrdev_ops = {
	.init = s1ur_chrdev_init,
	.term = s1ur_chrdev_term,
	.open = s1ur_chrdev_open,
	.release = s1ur_chrdev_release,
	.tune = s1ur_chrdev_tune,
	.check_lock = s1ur_chrdev_check_lock,
	.set_stream_id = s1ur_chrdev_set_stream_id,
	.set_lnb_voltage = NULL,
	.set_capture = s1ur_chrdev_set_capture,
	.read_signal_strength = NULL,
	.read_cnr = NULL,
	.read_cnr_raw = s1ur_chrdev_read_cnr_raw
};

static int s1ur_device_load_config(struct s1ur_device *s1ur,
				       struct ptx_chrdev_config *chrdev_config)
{
	int ret = 0, i;
	struct device *dev = s1ur->dev;
	struct it930x_bridge *it930x = &s1ur->it930x;
	struct it930x_stream_input *input = &it930x->config.input[0];
	struct s1ur_chrdev *chrdevs1ur = &s1ur->chrdevs1ur;
	u8 tmp;

	ret = it930x_read_reg(it930x, 0x4979, &tmp);
	if (ret) {
		dev_err(dev,
			"s1ur_load_config: it930x_read_reg(0x4979) failed.\n");
		return ret;
	} else if (!tmp) {
		dev_warn(dev, "EEPROM error.\n");
		return ret;
	}

	chrdev_config->system_cap = PTX_ISDB_T_SYSTEM | PTX_ISDB_S_SYSTEM;

	input->enable = true;
	input->is_parallel = false;
	input->port_number = 0;
	input->slave_number = 0;
	input->i2c_bus = 3;
	input->i2c_addr = 0x10;
	input->packet_len = 188;
	input->sync_byte = 0x47;

	chrdevs1ur->tc90522_t.dev = dev;
	chrdevs1ur->tc90522_t.i2c = &it930x->i2c_master[2];
	chrdevs1ur->tc90522_t.i2c_addr = 0x10;
	chrdevs1ur->tc90522_t.is_secondary = false;

	chrdevs1ur->tc90522_s.dev = dev;
	chrdevs1ur->tc90522_s.i2c = &it930x->i2c_master[2];
	chrdevs1ur->tc90522_s.i2c_addr = 0x11;
	chrdevs1ur->tc90522_s.is_secondary = false;

	chrdevs1ur->r850.dev = dev;
	chrdevs1ur->r850.i2c = &chrdevs1ur->tc90522_t.i2c_master;
	chrdevs1ur->r850.i2c_addr = 0x7c;
	chrdevs1ur->r850.config.xtal = 24000;
	chrdevs1ur->r850.config.loop_through = false;
	chrdevs1ur->r850.config.clock_out = false;
	chrdevs1ur->r850.config.no_imr_calibration = true;
	chrdevs1ur->r850.config.no_lpf_calibration = true;

#if 0
	chrdevs1ur->rt710.dev = dev;
	chrdevs1ur->rt710.i2c = &chrdevs1ur->tc90522_s.i2c_master;
	chrdevs1ur->rt710.i2c_addr = 0x7a;
	chrdevs1ur->rt710.config.xtal = 24000;
	chrdevs1ur->rt710.config.loop_through = false;
	chrdevs1ur->rt710.config.clock_out = false;
	chrdevs1ur->rt710.config.signal_output_mode = RT710_SIGNAL_OUTPUT_DIFFERENTIAL;
	chrdevs1ur->rt710.config.agc_mode = RT710_AGC_POSITIVE;
	chrdevs1ur->rt710.config.vga_atten_mode = RT710_VGA_ATTEN_OFF;
	chrdevs1ur->rt710.config.fine_gain = RT710_FINE_GAIN_3DB;
	chrdevs1ur->rt710.config.scan_mode = RT710_SCAN_MANUAL;
#endif

	for (i = 1; i < 5; i++) {
		it930x->config.input[i].enable = false;
		it930x->config.input[i].port_number = i;
	}

	return 0;
}

int s1ur_device_init(struct s1ur_device *s1ur, struct device *dev,
			 struct ptx_chrdev_context *chrdev_ctx,
			 struct completion *quit_completion)
{
	int ret = 0;
	struct it930x_bridge *it930x;
	struct itedtv_bus *bus;
	struct ptx_chrdev_config chrdev_config;
	struct ptx_chrdev_group_config chrdev_group_config;
	struct ptx_chrdev_group *chrdev_group;
	struct s1ur_stream_context *stream_ctx;

	if (!s1ur || !dev || !chrdev_ctx || !quit_completion)
		return -EINVAL;

	dev_dbg(dev, "s1ur_device_init\n");

	get_device(dev);

	kref_init(&s1ur->kref);
	s1ur->dev = dev;
	s1ur->quit_completion = quit_completion;

	stream_ctx = kzalloc(sizeof(*stream_ctx), GFP_KERNEL);
	if (!stream_ctx) {
		dev_err(s1ur->dev,
			"s1ur_device_init: kzalloc(sizeof(*stream_ctx), GFP_KERNEL) failed.\n");
		ret = -ENOMEM;
		goto fail;
	}
	s1ur->stream_ctx = stream_ctx;

	it930x = &s1ur->it930x;
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

	ret = s1ur_device_load_config(s1ur, &chrdev_config);
	if (ret)
		goto fail_device;

	chrdev_config.ops = &s1ur_chrdev_ops;
	chrdev_config.options = PTX_CHRDEV_WAIT_AFTER_LOCK_TC_T;
	chrdev_config.ringbuf_size = 188 * px4_device_params.tsdev_max_packets;
	chrdev_config.ringbuf_threshold_size = chrdev_config.ringbuf_size / 10;
	chrdev_config.priv = &s1ur->chrdevs1ur;

	ret = it930x_load_firmware(it930x, IT930X_FIRMWARE_FILENAME);
	if (ret)
		goto fail_device;

	ret = it930x_init_warm(it930x);
	if (ret)
		goto fail_device;

	/* GPIO */
	ret = it930x_set_gpio_mode(it930x, 3, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	ret = it930x_write_gpio(it930x, 3, true);
	if (ret)
		goto fail_device;

	ret = it930x_set_gpio_mode(it930x, 2, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	ret = it930x_write_gpio(it930x, 2, false);
	if (ret)
		goto fail_device;

#if 0
	ret = it930x_set_gpio_mode(it930x, 11, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	/* LNB power supply: off */
	ret = it930x_write_gpio(it930x, 11, false);
	if (ret)
		goto fail_device;
#endif

	if (px4_device_params.discard_null_packets) {
		struct it930x_pid_filter filter;

		filter.block = true;
		filter.num = 1;
		filter.pid[0] = 0x1fff;

		ret = it930x_set_pid_filter(it930x, 0, &filter);
		if (ret)
			goto fail_device;
	}

	chrdev_group_config.owner_kref = &s1ur->kref;
	chrdev_group_config.owner_kref_release = s1ur_device_release;
	chrdev_group_config.reserved = false;
	chrdev_group_config.minor_base = 0;	/* unused */
	chrdev_group_config.chrdev_num = 1;
	chrdev_group_config.chrdev_config = &chrdev_config;

	ret = ptx_chrdev_context_add_group(chrdev_ctx, dev,
					   &chrdev_group_config, &chrdev_group);
	if (ret)
		goto fail_chrdev;

	s1ur->chrdev_group = chrdev_group;
	s1ur->chrdevs1ur.chrdev = &chrdev_group->chrdev[0];
	stream_ctx->chrdev = &chrdev_group->chrdev[0];

	atomic_set(&s1ur->available, 1);
	return 0;

fail_chrdev:

fail_device:
	it930x_term(it930x);

fail_bridge:
	itedtv_bus_term(bus);

fail_bus:
	kfree(s1ur->stream_ctx);

fail:
	put_device(dev);
	return ret;
}

static void s1ur_device_release(struct kref *kref)
{
	struct s1ur_device *s1ur = container_of(kref,
							struct s1ur_device,
							kref);

	dev_dbg(s1ur->dev, "s1ur_device_release\n");

	it930x_term(&s1ur->it930x);
	itedtv_bus_term(&s1ur->it930x.bus);

	kfree(s1ur->stream_ctx);
	put_device(s1ur->dev);

	complete(s1ur->quit_completion);
	return;
}

void s1ur_device_term(struct s1ur_device *s1ur)
{
	dev_dbg(s1ur->dev,
		"s1ur_device_term: kref count: %u\n",
		kref_read(&s1ur->kref));

	atomic_xchg(&s1ur->available, 0);
	ptx_chrdev_group_destroy(s1ur->chrdev_group);

	kref_put(&s1ur->kref, s1ur_device_release);
	return;
}
