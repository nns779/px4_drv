// SPDX-License-Identifier: GPL-2.0-only
/*
 * PTX driver for Digibest ISDBT2071 (DTV03A-1TU) device (isdbt2071_device.c)
 *
 * Copyright (c) 2023 techma.
 *               2025 hendecarows
 */

#include "print_format.h"
#include "isdbt2071_device.h"

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/slab.h>

#include "px4_device_params.h"
#include "firmware.h"
#include "ts_sync.h"

#define ISDBT2071_DEVICE_TS_SYNC_COUNT	4
#define ISDBT2071_DEVICE_TS_SYNC_SIZE	(188 * ISDBT2071_DEVICE_TS_SYNC_COUNT)

struct isdbt2071_stream_context {
	struct ptx_chrdev *chrdev;
	u8 remain_buf[ISDBT2071_DEVICE_TS_SYNC_SIZE];
	size_t remain_len;
};

static void isdbt2071_device_release(struct kref *kref);

static int isdbt2071_backend_set_power(struct isdbt2071_device *isdbt2071,
				      bool state)
{
	int ret = 0;
	struct it930x_bridge *it930x = &isdbt2071->it930x;

	dev_dbg(isdbt2071->dev,
		"isdbt2071_backend_set_power: %s\n", (state) ? "true" : "false");

	if (!state && !atomic_read(&isdbt2071->available))
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

static int isdbt2071_backend_init(struct isdbt2071_device *isdbt2071)
{
	int ret = 0;
	struct isdbt2071_chrdev *chrdev2071 = &isdbt2071->chrdev2071;

	ret = tc90522_init(&chrdev2071->tc90522_t);
	if (ret) {
		dev_err(isdbt2071->dev,
			"isdbt2071_backend_init: tc90522_init() (t) failed. (ret: %d)\n",
			ret);
		return ret;
	}

	tc90522_term(&chrdev2071->tc90522_s);

	ret = r850_init(&chrdev2071->r850);
	if (ret) {
		dev_err(isdbt2071->dev,
			"isdbt2071_backend_init: r850_init() failed. (ret: %d)\n",
			ret);
		return ret;
	}

	return 0;
}

static int isdbt2071_backend_term(struct isdbt2071_device *isdbt2071)
{
	struct isdbt2071_chrdev *chrdev2071 = &isdbt2071->chrdev2071;

	r850_term(&chrdev2071->r850);
	tc90522_term(&chrdev2071->tc90522_t);
	tc90522_term(&chrdev2071->tc90522_s);

	return 0;
}

static void isdbt2071_device_stream_process(struct ptx_chrdev *chrdev,
					   u8 **buf, u32 *len)
{
	u8 *p = *buf;
	u32 remain = *len;

	while (likely(remain)) {
		u32 i = 0;
		bool sync_remain = false;

		while (true) {
			if (likely(((i + 1) * 188) <= remain)) {
				if (unlikely(!px4_ts_has_plain_sync(p[i * 188])))
					break;
			} else {
				sync_remain = true;
				break;
			}
			i++;
		}

		if (unlikely(i < ISDBT2071_DEVICE_TS_SYNC_COUNT)) {
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

static int isdbt2071_device_stream_handler(void *context, void *buf, u32 len)
{
	struct isdbt2071_stream_context *stream_ctx = context;
	u8 *ctx_remain_buf = stream_ctx->remain_buf;
	u32 ctx_remain_len = stream_ctx->remain_len;
	u8 *p = buf;
	u32 remain = len;

	if (unlikely(ctx_remain_len)) {
		if (likely((ctx_remain_len + len) >= ISDBT2071_DEVICE_TS_SYNC_SIZE)) {
			u32 t = ISDBT2071_DEVICE_TS_SYNC_SIZE - ctx_remain_len;

			memcpy(ctx_remain_buf + ctx_remain_len, p, t);
			ctx_remain_len = ISDBT2071_DEVICE_TS_SYNC_SIZE;

			isdbt2071_device_stream_process(stream_ctx->chrdev,
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

	isdbt2071_device_stream_process(stream_ctx->chrdev, &p, &remain);

	if (unlikely(remain)) {
		memcpy(stream_ctx->remain_buf, p, remain);
		stream_ctx->remain_len = remain;
	}

	return 0;
}

static int isdbt2071_chrdev_init(struct ptx_chrdev *chrdev)
{
	dev_dbg(chrdev->parent->dev, "isdbt2071_chrdev_init\n");

	chrdev->params.system = PTX_ISDB_T_SYSTEM;
	return 0;
}

static int isdbt2071_chrdev_term(struct ptx_chrdev *chrdev)
{
	dev_dbg(chrdev->parent->dev, "isdbt2071_chrdev_term\n");
	return 0;
}

static struct tc90522_regbuf tc_init_isdbt2071t[] = {
	{ 0x04, NULL, { 0x00 } },
	{ 0x10, NULL, { 0x00 } },
	{ 0x11, NULL, { 0x2d } },
	{ 0x12, NULL, { 0x02 } },
	{ 0x13, NULL, { 0x62 } },
	{ 0x14, NULL, { 0x60 } },
	{ 0x15, NULL, { 0x00 } },
	{ 0x16, NULL, { 0x00 } },
	{ 0x1d, NULL, { 0x05 } },
	{ 0x1e, NULL, { 0x15 } },
	{ 0x1f, NULL, { 0x40 } },
	{ 0x30, NULL, { 0x20 } },
	{ 0x31, NULL, { 0x0b } },
	{ 0x32, NULL, { 0x8f } },
	{ 0x34, NULL, { 0x0f } },
	{ 0x38, NULL, { 0x01 } },
	{ 0x39, NULL, { 0x1c } },
};

static int isdbt2071_chrdev_open(struct ptx_chrdev *chrdev)
{
	int ret = 0;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct isdbt2071_chrdev *chrdev2071 = chrdev->priv;
	struct isdbt2071_device *isdbt2071 = container_of(chrdev2071,
							struct isdbt2071_device,
							chrdev2071);
	struct r850_system_config sys;

	dev_dbg(isdbt2071->dev,
		"isdbt2071_chrdev_open %u\n", chrdev_group->id);

	mutex_lock(&isdbt2071->lock);

	/* カードが基板の電源を使っている場合は入れ直さず、受信回路の初期化だけを行う */
	if (!isdbt2071->card_powered) {
		ret = isdbt2071_backend_set_power(isdbt2071, true);
		if (ret) {
			dev_err(isdbt2071->dev,
				"isdbt2071_chrdev_open %u: isdbt2071_backend_set_power(true) failed. (ret: %d)\n",
				chrdev_group->id, ret);
			goto fail_backend_power;
		}
	}

	ret = isdbt2071_backend_init(isdbt2071);
	if (ret) {
		dev_err(isdbt2071->dev,
			"isdbt2071_chrdev_open %u: isdbt2071_backend_init() failed. (ret: %d)\n",
			chrdev_group->id, ret);
		goto fail_backend_init;
	}

	/* Initialization for ISDB-T */
	ret = tc90522_write_multiple_regs(&chrdev2071->tc90522_t,
					  tc_init_isdbt2071t, ARRAY_SIZE(tc_init_isdbt2071t));

	if (ret) {
		dev_err(isdbt2071->dev,
			"isdbt2071_chrdev_open %u: tc90522_write_multiple_regs(tc_init_t) failed. (ret: %d)\n",
			chrdev_group->id, ret);
		goto fail_backend;
	}

	/* disable ts pins */
	ret = tc90522_enable_ts_pins_t(&chrdev2071->tc90522_t, false);
	if (ret) {
		dev_err(isdbt2071->dev,
			"isdbt2071_chrdev_open %u: tc90522_enable_ts_pins_t(false) failed. (ret: %d)\n",
			chrdev_group->id, ret);
		goto fail_backend;
	}

	/* wake up */
	ret = tc90522_sleep_t(&chrdev2071->tc90522_t, false);
	if (ret) {
		dev_err(isdbt2071->dev,
			"isdbt2071_chrdev_open %u: tc90522_sleep_t(true) failed. (ret: %d)\n",
			chrdev_group->id, ret);
		goto fail_backend;
	}

	ret = r850_wakeup(&chrdev2071->r850);
	if (ret) {
		dev_err(isdbt2071->dev,
			"isdbt2071_chrdev_tune %u: r850_wakeup() failed. (ret: %d)\n",
			chrdev_group->id, ret);
		goto fail_backend;
	}

	sys.system = R850_SYSTEM_ISDB_T;
	sys.bandwidth = R850_BANDWIDTH_6M;
	sys.if_freq = 4063;

	ret = r850_set_system(&chrdev2071->r850, &sys);
	if (ret) {
		dev_err(isdbt2071->dev,
			"isdbt2071_chrdev_open %u: r850_set_system() failed. (ret: %d)\n",
			chrdev_group->id, ret);
		goto fail_backend;
	}

	isdbt2071->tuner_open = true;
	kref_get(&isdbt2071->kref);

	mutex_unlock(&isdbt2071->lock);
	return 0;

fail_backend:
	isdbt2071_backend_term(isdbt2071);

fail_backend_init:
	if (!isdbt2071->card_powered)
		isdbt2071_backend_set_power(isdbt2071, false);

fail_backend_power:
	mutex_unlock(&isdbt2071->lock);
	return ret;
}

static int isdbt2071_chrdev_release(struct ptx_chrdev *chrdev)
{
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct isdbt2071_chrdev *chrdev2071 = chrdev->priv;
	struct isdbt2071_device *isdbt2071 = container_of(chrdev2071,
							struct isdbt2071_device,
							chrdev2071);

	dev_dbg(isdbt2071->dev,
		"isdbt2071_chrdev_release %u: kref count: %u\n",
		chrdev_group->id, kref_read(&isdbt2071->kref));

	mutex_lock(&isdbt2071->lock);

	isdbt2071_backend_term(isdbt2071);
	isdbt2071->tuner_open = false;
	if (!isdbt2071->card_powered)
		isdbt2071_backend_set_power(isdbt2071, false);

	mutex_unlock(&isdbt2071->lock);

	kref_put(&isdbt2071->kref, isdbt2071_device_release);
	return 0;
}

static int isdbt2071_chrdev_tune(struct ptx_chrdev *chrdev,
				struct ptx_tune_params *params)
{
	int ret = 0, i;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct isdbt2071_chrdev *chrdev2071 = chrdev->priv;
	struct isdbt2071_device *isdbt2071 = container_of(chrdev2071,
							struct isdbt2071_device,
							chrdev2071);
	bool tuner_locked;

	dev_dbg(isdbt2071->dev,
		"isdbt2071_chrdev_tune %u\n", chrdev_group->id);

	switch (params->system) {
	case PTX_ISDB_T_SYSTEM:
		ret = tc90522_write_reg(&chrdev2071->tc90522_t, 0x47, 0x30);
		if (ret)
			break;

		ret = tc90522_set_agc_t(&chrdev2071->tc90522_t, false);
		if (ret) {
			dev_err(isdbt2071->dev,
				"isdbt2071_chrdev_tune %u: tc90522_set_agc_t(false) failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		}

		ret = tc90522_write_reg(&chrdev2071->tc90522_t, 0x76, 0x03);
		if (ret)
			break;

		ret = tc90522_write_reg(&chrdev2071->tc90522_t, 0x77, 0x01);
		if (ret)
			break;

		ret = tc90522_write_reg(&chrdev2071->tc90522_t, 0x3b, 0x10);
		if (ret)
			break;

		ret = tc90522_write_reg(&chrdev2071->tc90522_t, 0x3c, 0x10);
		if (ret)
			break;

		ret = tc90522_write_reg(&chrdev2071->tc90522_t, 0x3d, 0x24);
		if (ret)
			break;

		ret = r850_set_frequency(&chrdev2071->r850, params->freq);
		if (ret) {
			dev_err(isdbt2071->dev,
				"isdbt2071_chrdev_tune %u: r850_set_frequency(%u) failed. (ret: %d)\n",
				chrdev_group->id, params->freq, ret);
			break;
		}

		i = 50;
		while (i--) {
			ret = r850_is_pll_locked(&chrdev2071->r850,
						 &tuner_locked);
			if (!ret && tuner_locked)
				break;

			msleep(10);
		}

		if (ret) {
			dev_err(isdbt2071->dev,
				"isdbt2071_chrdev_tune %u: r850_is_pll_locked() failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		} else if (!tuner_locked) {
			/* PLL error */
			dev_dbg(isdbt2071->dev,
				"isdbt2071_chrdev_tune %u: PLL is NOT locked.\n",
				chrdev_group->id);
			ret = -EAGAIN;
			break;
		}

		dev_dbg(isdbt2071->dev,
			"isdbt2071_chrdev_tune %u: PLL is locked. count: %d\n",
			chrdev_group->id, i);

		ret = tc90522_set_agc_t(&chrdev2071->tc90522_t, true);
		if (ret) {
			dev_err(isdbt2071->dev,
				"isdbt2071_chrdev_tune %u: tc90522_set_agc_t(true) failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		}

		msleep(100);

		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int isdbt2071_chrdev_check_lock(struct ptx_chrdev *chrdev, bool *locked)
{
	int ret = 0;
	struct isdbt2071_chrdev *chrdev2071 = chrdev->priv;

	switch (chrdev->current_system) {
	case PTX_ISDB_T_SYSTEM:
		ret = tc90522_is_signal_locked_t(&chrdev2071->tc90522_t,
						locked);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int isdbt2071_chrdev_start_capture(struct ptx_chrdev *chrdev)
{
	int ret = 0;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct isdbt2071_chrdev *chrdev2071 = chrdev->priv;
	struct isdbt2071_device *isdbt2071 = container_of(chrdev2071,
							struct isdbt2071_device,
							chrdev2071);
	struct isdbt2071_stream_context *stream_ctx = isdbt2071->stream_ctx;

	dev_dbg(isdbt2071->dev,
		"isdbt2071_chrdev_start_capture %u\n", chrdev_group->id);

	ret = it930x_purge_psb(&isdbt2071->it930x,
			       px4_device_params.psb_purge_timeout);
	if (ret) {
		dev_err(isdbt2071->dev,
			"isdbt2071_chrdev_start_capture %u: it930x_purge_psb() failed. (ret: %d)\n",
			chrdev_group->id, ret);
		goto fail;
	}

	switch (chrdev->current_system) {
	case PTX_ISDB_T_SYSTEM:
		ret = tc90522_enable_ts_pins_t(&chrdev2071->tc90522_t, true);
		if (ret)
			dev_err(isdbt2071->dev,
				"isdbt2071_chrdev_start_capture %u: tc90522_enable_ts_pins_t(true) failed. (ret: %d)\n",
				chrdev_group->id, ret);

		break;

	default:
		ret = -EINVAL;
		break;
	}

	if (ret)
		goto fail_tc;

	stream_ctx->remain_len = 0;

	ret = itedtv_bus_start_streaming(&isdbt2071->it930x.bus,
					 isdbt2071_device_stream_handler,
					 stream_ctx);
	if (ret) {
		dev_err(isdbt2071->dev,
			"isdbt2071_chrdev_start_capture %u: itedtv_bus_start_streaming() failed. (ret: %d)\n",
			chrdev_group->id, ret);
			goto fail_bus;
	}

	return 0;

fail_bus:

fail_tc:
	switch (chrdev->current_system) {
	case PTX_ISDB_T_SYSTEM:
		tc90522_enable_ts_pins_t(&chrdev2071->tc90522_t, false);
		break;

	default:
		ret = -EINVAL;
		break;
	}

fail:
	return ret;
}

static int isdbt2071_chrdev_stop_capture(struct ptx_chrdev *chrdev)
{
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct isdbt2071_chrdev *chrdev2071 = chrdev->priv;
	struct isdbt2071_device *isdbt2071 = container_of(chrdev2071,
							struct isdbt2071_device,
							chrdev2071);

	dev_dbg(isdbt2071->dev,
		"isdbt2071_chrdev_stop_capture %u\n", chrdev_group->id);

	itedtv_bus_stop_streaming(&isdbt2071->it930x.bus);

	if (!atomic_read(&isdbt2071->available))
		return 0;

	switch (chrdev->current_system) {
	case PTX_ISDB_T_SYSTEM:
		tc90522_enable_ts_pins_t(&chrdev2071->tc90522_t, false);
		break;

	default:
		break;
	}

	return 0;
}

static int isdbt2071_chrdev_set_capture(struct ptx_chrdev *chrdev, bool status)
{
	return (status) ? isdbt2071_chrdev_start_capture(chrdev)
			: isdbt2071_chrdev_stop_capture(chrdev);
}

static int isdbt2071_chrdev_read_cnr_raw(struct ptx_chrdev *chrdev, u32 *value)
{
	int ret = 0;
	struct isdbt2071_chrdev *chrdev2071 = chrdev->priv;

	switch (chrdev->current_system) {
	case PTX_ISDB_T_SYSTEM:
		ret = tc90522_get_cndat_t(&chrdev2071->tc90522_t, value);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static struct ptx_chrdev_operations isdbt2071_chrdev_ops = {
	.init = isdbt2071_chrdev_init,
	.term = isdbt2071_chrdev_term,
	.open = isdbt2071_chrdev_open,
	.release = isdbt2071_chrdev_release,
	.tune = isdbt2071_chrdev_tune,
	.check_lock = isdbt2071_chrdev_check_lock,
	.set_stream_id = NULL,
	.set_lnb_voltage = NULL,
	.set_capture = isdbt2071_chrdev_set_capture,
	.read_signal_strength = NULL,
	.read_cnr = NULL,
	.read_cnr_raw = isdbt2071_chrdev_read_cnr_raw
};

static int isdbt2071_device_load_config(struct isdbt2071_device *isdbt2071,
				       struct ptx_chrdev_config *chrdev_config)
{
	int ret = 0, i;
	struct device *dev = isdbt2071->dev;
	struct it930x_bridge *it930x = &isdbt2071->it930x;
	struct it930x_stream_input *input = &it930x->config.input[0];
	struct isdbt2071_chrdev *chrdev2071 = &isdbt2071->chrdev2071;
	u8 tmp;

	ret = it930x_read_reg(it930x, 0x4979, &tmp);
	if (ret) {
		dev_err(dev,
			"isdbt2071_load_config: it930x_read_reg(0x4979) failed.\n");
		return ret;
	} else if (!tmp) {
		dev_warn(dev, "EEPROM error.\n");
		return ret;
	}

	chrdev_config->system_cap = PTX_ISDB_T_SYSTEM;

	input->enable = true;
	input->is_parallel = false;
	input->port_number = 0;
	input->slave_number = 0;
	input->i2c_bus = 3;
	input->i2c_addr = 0x10;
	input->packet_len = 188;
	input->sync_byte = 0x47;

	chrdev2071->tc90522_t.dev = dev;
	chrdev2071->tc90522_t.i2c = &it930x->i2c_master[2];
	chrdev2071->tc90522_t.i2c_addr = 0x10;
	chrdev2071->tc90522_t.is_secondary = false;

	chrdev2071->tc90522_s.dev = dev;
	chrdev2071->tc90522_s.i2c = &it930x->i2c_master[2];
	chrdev2071->tc90522_s.i2c_addr = 0x11;
	chrdev2071->tc90522_s.is_secondary = false;

	chrdev2071->r850.dev = dev;
	chrdev2071->r850.i2c = &chrdev2071->tc90522_t.i2c_master;
	chrdev2071->r850.i2c_addr = 0x7c;
	chrdev2071->r850.config.xtal = 24000;
	chrdev2071->r850.config.loop_through = false;
	chrdev2071->r850.config.clock_out = false;
	chrdev2071->r850.config.no_imr_calibration = true;
	chrdev2071->r850.config.no_lpf_calibration = true;

	input->port_number = 4;
	input->i2c_addr = 0x18;
	chrdev2071->tc90522_t.i2c_addr = 0x18;

	for (i = 1; i < 5; i++) {
		it930x->config.input[i].enable = false;
		it930x->config.input[i].port_number = i - 1;
	}

	return 0;
}

static int isdbt2071_device_set_card_power(void *context, bool on)
{
	struct isdbt2071_device *isdbt2071 = context;
	int ret = 0;

	/* 受信中のチューナーがあれば、基板の電源はチューナー側で入ったままになっている */
	if (!isdbt2071->tuner_open)
		ret = isdbt2071_backend_set_power(isdbt2071, on);

	/* 停止は成否にかかわらず停止済みとして記録し、投入は成功したときだけ記録する */
	if (!on || !ret)
		isdbt2071->card_powered = on;

	return ret;
}

int isdbt2071_device_init(struct isdbt2071_device *isdbt2071, struct device *dev,
			struct ptx_chrdev_context *chrdev_ctx,
			struct completion *quit_completion)
{
	int ret = 0;
	struct it930x_bridge *it930x;
	struct itedtv_bus *bus;
	struct ptx_chrdev_config chrdev_config;
	struct ptx_chrdev_group_config chrdev_group_config;
	struct ptx_chrdev_group *chrdev_group;
	struct isdbt2071_stream_context *stream_ctx;

	if (!isdbt2071 || !dev || !chrdev_ctx || !quit_completion)
		return -EINVAL;

	dev_dbg(dev, "isdbt2071_device_init\n");

	get_device(dev);

	kref_init(&isdbt2071->kref);
	mutex_init(&isdbt2071->lock);
	isdbt2071->tuner_open = false;
	isdbt2071->card_powered = false;
	isdbt2071->card = NULL;
	isdbt2071->dev = dev;
	isdbt2071->quit_completion = quit_completion;

	stream_ctx = kzalloc(sizeof(*stream_ctx), GFP_KERNEL);
	if (!stream_ctx) {
		dev_err(isdbt2071->dev,
			"isdbt2071_device_init: kzalloc(sizeof(*stream_ctx), GFP_KERNEL) failed.\n");
		ret = -ENOMEM;
		goto fail;
	}
	isdbt2071->stream_ctx = stream_ctx;

	it930x = &isdbt2071->it930x;
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

	ret = isdbt2071_device_load_config(isdbt2071, &chrdev_config);
	if (ret)
		goto fail_device;

	chrdev_config.ops = &isdbt2071_chrdev_ops;
	chrdev_config.options = PTX_CHRDEV_WAIT_AFTER_LOCK_TC_T;
	chrdev_config.ringbuf_size = 188 * px4_device_params.tsdev_max_packets;
	chrdev_config.ringbuf_threshold_size = chrdev_config.ringbuf_size / 10;
	chrdev_config.priv = &isdbt2071->chrdev2071;

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

	if (px4_device_params.discard_null_packets) {
		struct it930x_pid_filter filter;

		filter.block = true;
		filter.num = 1;
		filter.pid[0] = 0x1fff;

		ret = it930x_set_pid_filter(it930x, 0, &filter);
		if (ret)
			goto fail_device;
	}

	chrdev_group_config.owner_kref = &isdbt2071->kref;
	chrdev_group_config.owner_kref_release = isdbt2071_device_release;
	chrdev_group_config.reserved = false;
	chrdev_group_config.minor_base = 0;	/* unused */
	chrdev_group_config.chrdev_num = 1;
	chrdev_group_config.chrdev_config = &chrdev_config;

	ret = ptx_chrdev_context_add_group(chrdev_ctx, dev,
					   &chrdev_group_config, &chrdev_group);
	if (ret)
		goto fail_chrdev;

	isdbt2071->chrdev_group = chrdev_group;
	isdbt2071->chrdev2071.chrdev = &chrdev_group->chrdev[0];
	stream_ctx->chrdev = &chrdev_group->chrdev[0];

	atomic_set(&isdbt2071->available, 1);

	{
		struct px4_card_backend card_backend = {
			.it930x = it930x,
			.lock = &isdbt2071->lock,
			.available = &isdbt2071->available,
			.set_power = isdbt2071_device_set_card_power,
			.context = isdbt2071,
		};

		/* カードリーダーを登録できなくても、チューナーは使えるようにする */
		ret = px4_card_register(dev, &card_backend, &isdbt2071->card);
		if (ret)
			dev_warn(dev, "Card reader registration failed: %d\n", ret);
	}
	return 0;

fail_chrdev:

fail_device:
	it930x_term(it930x);

fail_bridge:
	itedtv_bus_term(bus);

fail_bus:
	kfree(isdbt2071->stream_ctx);

fail:
	mutex_destroy(&isdbt2071->lock);
	put_device(dev);
	return ret;
}

static void isdbt2071_device_release(struct kref *kref)
{
	struct isdbt2071_device *isdbt2071 = container_of(kref,
							struct isdbt2071_device,
							kref);

	dev_dbg(isdbt2071->dev, "isdbt2071_device_release\n");

	it930x_term(&isdbt2071->it930x);
	itedtv_bus_term(&isdbt2071->it930x.bus);

	kfree(isdbt2071->stream_ctx);
	mutex_destroy(&isdbt2071->lock);
	put_device(isdbt2071->dev);

	complete(isdbt2071->quit_completion);
	return;
}

void isdbt2071_device_term(struct isdbt2071_device *isdbt2071)
{
	dev_dbg(isdbt2071->dev,
		"isdbt2071_device_term: kref count: %u\n",
		kref_read(&isdbt2071->kref));

	/* カードの操作と並行しても、登録の解除後にハードウェアへ触れないよう、ロックの下で切り替える */
	mutex_lock(&isdbt2071->lock);
	atomic_xchg(&isdbt2071->available, 0);
	mutex_unlock(&isdbt2071->lock);

	px4_card_unregister(isdbt2071->card);
	isdbt2071->card = NULL;
	ptx_chrdev_group_destroy(isdbt2071->chrdev_group);

	kref_put(&isdbt2071->kref, isdbt2071_device_release);
	return;
}
