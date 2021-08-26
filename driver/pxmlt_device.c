// SPDX-License-Identifier: GPL-2.0-only
/*
 * PTX driver for PLEX PX-MLT series devices (pxmlt_device.c)
 *
 * Copyright (c) 2018-2021 nns779
 */

#include "print_format.h"
#include "pxmlt_device.h"

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/slab.h>

#include "px4_device_params.h"
#include "firmware.h"

#define PXMLT_DEVICE_TS_SYNC_COUNT	4
#define PXMLT_DEVICE_TS_SYNC_SIZE	(188 * PXMLT_DEVICE_TS_SYNC_COUNT)

struct pxmlt_stream_context {
	struct ptx_chrdev *chrdev[PXMLT_CHRDEV_MAX_NUM];
	u8 remain_buf[PXMLT_DEVICE_TS_SYNC_SIZE];
	size_t remain_len;
};

static int pxmlt_chrdev_set_lnb_voltage(struct ptx_chrdev *chrdev, int voltage);
static void pxmlt_device_release(struct kref *kref);

static int pxmlt_backend_set_power(struct pxmlt_device *pxmlt, bool state)
{
	int ret = 0;
	struct it930x_bridge *it930x = &pxmlt->it930x;

	dev_dbg(pxmlt->dev,
		"pxmlt_backend_set_power: %s\n", (state) ? "true" : "false");

	if (!state && !atomic_read(&pxmlt->available))
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

#if 0
static int pxmlt_backend_init(struct pxmlt_device *pxmlt)
{
	int ret = 0, i;

	for (i = 0; i < pxmlt->chrdevm_num; i++) {
		struct pxmlt_chrdev *chrdevm = &pxmlt->chrdevm[i];

		ret = cxd2856er_init(&chrdevm->cxd2856er);
		if (ret) {
			dev_err(pxmlt->dev,
				"pxmlt_backend_init: cxd2856er_init() failed. (i: %d, ret: %d)\n",
				i, ret);
			break;
		}

		mutex_lock(chrdevm->tuner_lock);
		ret = cxd2858er_init(&chrdevm->cxd2858er);
		mutex_unlock(chrdevm->tuner_lock);

		if (ret) {
			dev_err(pxmlt->dev,
				"pxmlt_backend_init: cxd2858er_init() failed. (i: %d, ret: %d)\n",
				i, ret);
			break;
		}
	}

	return ret;
}

static int pxmlt_backend_term(struct pxmlt_device *pxmlt)
{
	int i;

	for (i = 0; i < pxmlt->chrdevm_num; i++) {
		struct pxmlt_chrdev *chrdevm = &pxmlt->chrdevm[i];

		mutex_lock(chrdevm->tuner_lock);
		cxd2858er_term(&chrdevm->cxd2858er);
		mutex_unlock(chrdevm->tuner_lock);

		cxd2856er_term(&chrdevm->cxd2856er);
	}

	return 0;
}
#endif

static void pxmlt_device_stream_process(struct ptx_chrdev **chrdev,
				      u8 **buf, u32 *len)
{
	u8 *p = *buf;
	u32 remain = *len;

	while (likely(remain)) {
		u32 i;
		bool sync_remain = false;

		for (i = 0; i < PXMLT_DEVICE_TS_SYNC_COUNT; i++) {
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

		if (unlikely(i < PXMLT_DEVICE_TS_SYNC_COUNT)) {
			p++;
			remain--;
			continue;
		}

		while (likely(remain >= 188 && ((p[0] & 0x8f) == 0x07))) {
			u8 id = (p[0] & 0x70) >> 4;

			if (likely(id && id < 6)) {
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

static int pxmlt_device_stream_handler(void *context, void *buf, u32 len)
{
	struct pxmlt_stream_context *stream_ctx = context;
	u8 *ctx_remain_buf = stream_ctx->remain_buf;
	u32 ctx_remain_len = stream_ctx->remain_len;
	u8 *p = buf;
	u32 remain = len;

	if (unlikely(ctx_remain_len)) {
		if (likely((ctx_remain_len + len) >= PXMLT_DEVICE_TS_SYNC_SIZE)) {
			u32 t = PXMLT_DEVICE_TS_SYNC_SIZE - ctx_remain_len;

			memcpy(ctx_remain_buf + ctx_remain_len, p, t);
			ctx_remain_len = PXMLT_DEVICE_TS_SYNC_SIZE;

			pxmlt_device_stream_process(stream_ctx->chrdev,
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

	pxmlt_device_stream_process(stream_ctx->chrdev, &p, &remain);

	if (unlikely(remain)) {
		memcpy(stream_ctx->remain_buf, p, remain);
		stream_ctx->remain_len = remain;
	}

	return 0;
}

static int pxmlt_chrdev_init(struct ptx_chrdev *chrdev)
{
	dev_dbg(chrdev->parent->dev, "pxmlt_chrdev_init\n");

	chrdev->params.system = PTX_UNSPECIFIED_SYSTEM;
	return 0;
}

static int pxmlt_chrdev_term(struct ptx_chrdev *chrdev)
{
	dev_dbg(chrdev->parent->dev, "pxmlt_chrdev_term\n");
	return 0;
}

static int pxmlt_chrdev_open(struct ptx_chrdev *chrdev)
{
	int ret = 0;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct pxmlt_chrdev *chrdevm = chrdev->priv;
	struct pxmlt_device *pxmlt = chrdevm->parent;

	dev_dbg(pxmlt->dev,
		"pxmlt_chrdev_open %u:%u\n", chrdev_group->id, chrdev->id);

	mutex_lock(&pxmlt->lock);

	if (!pxmlt->open_count) {
		ret = pxmlt_backend_set_power(pxmlt, true);
		if (ret) {
			dev_err(pxmlt->dev,
				"pxmlt_chrdev_open %u:%u: pxmlt_backend_set_power(true) failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id, ret);
			goto fail_backend_power;
		}
	}

	ret = cxd2856er_init(&chrdevm->cxd2856er);
	if (ret) {
		dev_err(pxmlt->dev,
			"pxmlt_chrdev_open %u:%u: cxd2856er_init() failed. (ret: %d)\n",
			chrdev_group->id, chrdev->id, ret);
		goto fail_demod_init;
	}

	mutex_lock(chrdevm->tuner_lock);
	ret = cxd2858er_init(&chrdevm->cxd2858er);
	mutex_unlock(chrdevm->tuner_lock);

	if (ret) {
		dev_err(pxmlt->dev,
			"pxmlt_chrdev_open %u:%u: cxd2858er_init() failed. (ret: %d)\n",
			chrdev_group->id, chrdev->id, ret);
		goto fail_tuner_init;
	}

	ret = cxd2856er_write_slvt_reg(&chrdevm->cxd2856er, 0x00, 0x00);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg_mask(&chrdevm->cxd2856er,
					    0xc4, 0x80, 0x88);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg_mask(&chrdevm->cxd2856er,
					    0xc5, 0x01, 0x01);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg_mask(&chrdevm->cxd2856er,
					    0xc6, 0x03, 0x1f);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg(&chrdevm->cxd2856er, 0x00, 0x60);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg_mask(&chrdevm->cxd2856er,
					    0x52, 0x03, 0x1f);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg(&chrdevm->cxd2856er, 0x00, 0x00);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg_mask(&chrdevm->cxd2856er,
					    0xc8, 0x03, 0x1f);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg_mask(&chrdevm->cxd2856er,
					    0xc9, 0x03, 0x1f);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg(&chrdevm->cxd2856er, 0x00, 0xa0);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg_mask(&chrdevm->cxd2856er,
					    0xb9, 0x01, 0x01);
	if (ret)
		goto fail_backend;

	pxmlt->open_count++;
	kref_get(&pxmlt->kref);

	mutex_unlock(&pxmlt->lock);
	return 0;

fail_backend:
	mutex_lock(chrdevm->tuner_lock);
	cxd2858er_term(&chrdevm->cxd2858er);
	mutex_unlock(chrdevm->tuner_lock);

fail_tuner_init:
	cxd2856er_term(&chrdevm->cxd2856er);

fail_demod_init:
	if (!pxmlt->open_count)
		pxmlt_backend_set_power(pxmlt, false);

fail_backend_power:
	mutex_unlock(&pxmlt->lock);
	dev_dbg(pxmlt->dev,
		"pxmlt_chrdev_open %u:%u: ret: %d\n",
		chrdev_group->id, chrdev->id, ret);
	return ret;
}

static int pxmlt_chrdev_release(struct ptx_chrdev *chrdev)
{
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct pxmlt_chrdev *chrdevm = chrdev->priv;
	struct pxmlt_device *pxmlt = chrdevm->parent;

	dev_dbg(pxmlt->dev,
		"pxmlt_chrdev_release %u:%u\n", chrdev_group->id, chrdev->id);

	pxmlt_chrdev_set_lnb_voltage(chrdev, 0);

	mutex_lock(&pxmlt->lock);

	if (!pxmlt->open_count) {
		mutex_unlock(&pxmlt->lock);
		return -EALREADY;
	}

	mutex_lock(chrdevm->tuner_lock);
	cxd2858er_term(&chrdevm->cxd2858er);
	mutex_unlock(chrdevm->tuner_lock);

	cxd2856er_term(&chrdevm->cxd2856er);

	pxmlt->open_count--;
	if (!pxmlt->open_count)
		pxmlt_backend_set_power(pxmlt, false);

	if (kref_put(&pxmlt->kref, pxmlt_device_release))
		return 0;

	mutex_unlock(&pxmlt->lock);
	return 0;
}

static int pxmlt_chrdev_tune(struct ptx_chrdev *chrdev,
			     struct ptx_tune_params *params)
{
	int ret = 0;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct pxmlt_chrdev *chrdevm = chrdev->priv;
	struct pxmlt_device *pxmlt = chrdevm->parent;
	union cxd2856er_system_params demod_params;

	dev_dbg(pxmlt->dev,
		"pxmlt_chrdev_tune %u:%u\n", chrdev_group->id, chrdev->id);

	memset(&demod_params, 0, sizeof(demod_params));

	switch (params->system) {
	case PTX_ISDB_T_SYSTEM:
		demod_params.bandwidth = params->bandwidth;

		ret = cxd2856er_wakeup(&chrdevm->cxd2856er,
				       CXD2856ER_ISDB_T_SYSTEM, &demod_params);
		if (ret)
			dev_err(pxmlt->dev,
				"pxmlt_chrdev_tune %u:%u: cxd2856er_wakeup(CXD2856ER_ISDB_T_SYSTEM) failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id, ret);

		break;

	case PTX_ISDB_S_SYSTEM:
		demod_params.bandwidth = 0;

		ret = cxd2856er_wakeup(&chrdevm->cxd2856er,
				       CXD2856ER_ISDB_S_SYSTEM, &demod_params);
		if (ret)
			dev_err(pxmlt->dev,
				"pxmlt_chrdev_tune %u:%u: cxd2856er_wakeup(CXD2856ER_ISDB_S_SYSTEM) failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id, ret);

		break;

	default:
		ret = -EINVAL;
		break;
	}

	if (ret)
		return ret;

	mutex_lock(chrdevm->tuner_lock);

	switch (params->system) {
	case PTX_ISDB_T_SYSTEM:
		ret = cxd2858er_set_params_t(&chrdevm->cxd2858er,
					     CXD2858ER_ISDB_T_SYSTEM,
					     params->freq, 6);
		if (ret)
			dev_err(pxmlt->dev,
				"pxmlt_chrdev_tune %u:%u: cxd2858er_set_params_t(%u, 6) failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id,
				params->freq, ret);

		break;

	case PTX_ISDB_S_SYSTEM:
		ret = cxd2858er_set_params_s(&chrdevm->cxd2858er,
					     CXD2858ER_ISDB_S_SYSTEM,
					     params->freq, 28860);
		if (ret)
			dev_err(pxmlt->dev,
				"pxmlt_chrdev_tune %u:%u: cxd2858er_set_params_s(%u, 28860) failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id,
				params->freq, ret);

		break;

	default:
		break;
	}

	mutex_unlock(chrdevm->tuner_lock);

	if (ret)
		return ret;

	ret = cxd2856er_post_tune(&chrdevm->cxd2856er);
	if (ret) {
		dev_err(pxmlt->dev,
			"pxmlt_chrdev_tune %u:%u: cxd2856er_post_tune() failed. (ret: %d)\n",
			chrdev_group->id, chrdev->id, ret);
		return ret;
	}

	return 0;
}

static int pxmlt_chrdev_check_lock(struct ptx_chrdev *chrdev, bool *locked)
{
	int ret = 0;
	struct pxmlt_chrdev *chrdevm = chrdev->priv;
	bool unlocked = false;

	switch (chrdev->current_system) {
	case PTX_ISDB_T_SYSTEM:
		ret = cxd2856er_is_ts_locked_isdbt(&chrdevm->cxd2856er,
						   locked, &unlocked);
		if (!ret && unlocked)
			ret = -ECANCELED;

		break;

	case PTX_ISDB_S_SYSTEM:
		ret = cxd2856er_is_ts_locked_isdbs(&chrdevm->cxd2856er, locked);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int pxmlt_chrdev_set_stream_id(struct ptx_chrdev *chrdev, u16 stream_id)
{
	int ret = 0;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct pxmlt_chrdev *chrdevm = chrdev->priv;
	struct pxmlt_device *pxmlt = chrdevm->parent;

	dev_dbg(pxmlt->dev,
		"pxmlt_chrdev_set_stream_id %u:%u\n",
		chrdev_group->id, chrdev->id);

	if (stream_id < 12) {
		ret = cxd2856er_set_slot_isdbs(&chrdevm->cxd2856er, stream_id);
		if (ret)
			dev_err(pxmlt->dev,
				"pxmlt_chrdev_set_stream_id %u:%u: cxd2856er_set_slot_isdbs(%u) failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id, stream_id, ret);
	} else {
		ret = cxd2856er_set_tsid_isdbs(&chrdevm->cxd2856er, stream_id);
		if (ret)
			dev_err(pxmlt->dev,
				"pxmlt_chrdev_set_stream_id %u:%u: cxd2856er_set_tsid_isdbs(%u) failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id, stream_id, ret);
	}

	return ret;
}

static int pxmlt_chrdev_set_lnb_voltage(struct ptx_chrdev *chrdev, int voltage)
{
	int ret = 0;
	struct pxmlt_chrdev *chrdevm = chrdev->priv;
	struct pxmlt_device *pxmlt = chrdevm->parent;

	dev_dbg(pxmlt->dev,
		"pxmlt_chrdev_set_lnb_voltage %u:%u voltage: %d\n",
		chrdev->parent->id, chrdev->id, voltage);

	if (voltage != 0 && voltage != 15)
		return -EINVAL;

	if (chrdevm->lnb_power == !!voltage)
		return 0;

	if (!voltage && !atomic_read(&pxmlt->available))
		return 0;

	mutex_lock(&pxmlt->lock);

	if (!voltage)
		pxmlt->lnb_power_count--;

	if (!pxmlt->lnb_power_count) {
		ret = it930x_write_gpio(&pxmlt->it930x, 11, !!voltage);
		if (ret && voltage)
			goto exit;
	}

	if (voltage)
		pxmlt->lnb_power_count++;

	chrdevm->lnb_power = !!voltage;

exit:
	mutex_unlock(&pxmlt->lock);
	return ret;
}

static int pxmlt_chrdev_start_capture(struct ptx_chrdev *chrdev)
{
	int ret = 0;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct pxmlt_chrdev *chrdevm = chrdev->priv;
	struct pxmlt_device *pxmlt = chrdevm->parent;

	dev_dbg(pxmlt->dev,
		"pxmlt_chrdev_start_capture %u:%u\n",
		chrdev_group->id, chrdev->id);

	mutex_lock(&pxmlt->lock);

	if (!pxmlt->streaming_count) {
		struct pxmlt_stream_context *stream_ctx = pxmlt->stream_ctx;

		ret = it930x_purge_psb(&pxmlt->it930x,
				       px4_device_params.psb_purge_timeout);
		if (ret) {
			dev_err(pxmlt->dev,
				"pxmlt_chrdev_start_capture %u:%u: it930x_purge_psb() failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id, ret);
			goto exit;
		}

		stream_ctx->remain_len = 0;

		ret = itedtv_bus_start_streaming(&pxmlt->it930x.bus,
						 pxmlt_device_stream_handler,
						 stream_ctx);
		if (ret) {
			dev_err(pxmlt->dev,
				"pxmlt_chrdev_start_capture %u:%u: itedtv_bus_start_streaming() failed. (ret: %d)\n",
				chrdev_group->id, chrdev->id, ret);
			goto exit;
		}
	}

	pxmlt->streaming_count++;

	dev_dbg(pxmlt->dev,
		"pxmlt_chrdev_start_capture %u:%u: streaming_count: %u\n",
		chrdev_group->id, chrdev->id, pxmlt->streaming_count);

exit:
	mutex_unlock(&pxmlt->lock);
	return ret;
}

static int pxmlt_chrdev_stop_capture(struct ptx_chrdev *chrdev)
{
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct pxmlt_chrdev *chrdevm = chrdev->priv;
	struct pxmlt_device *pxmlt = chrdevm->parent;

	dev_dbg(pxmlt->dev,
		"pxmlt_chrdev_stop_capture %u:%u\n",
		chrdev_group->id, chrdev->id);

	mutex_lock(&pxmlt->lock);

	if (!pxmlt->streaming_count) {
		mutex_unlock(&pxmlt->lock);
		return -EALREADY;
	}

	pxmlt->streaming_count--;
	if (!pxmlt->streaming_count) {
		dev_dbg(pxmlt->dev,
			"pxmlt_chrdev_stop_capture %u:%u: stopping...\n",
			chrdev_group->id, chrdev->id);
		itedtv_bus_stop_streaming(&pxmlt->it930x.bus);
	} else {
		dev_dbg(pxmlt->dev,
			"pxmlt_chrdev_stop_capture %u:%u: streaming_count: %u\n",
			chrdev_group->id, chrdev->id, pxmlt->streaming_count);
	}

	mutex_unlock(&pxmlt->lock);
	return 0;
}

static int pxmlt_chrdev_set_capture(struct ptx_chrdev *chrdev, bool status)
{
	return (status) ? pxmlt_chrdev_start_capture(chrdev)
			: pxmlt_chrdev_stop_capture(chrdev);
}

/* 10 to 34 dB, 0.5 dB unit */
static const struct {
	u16 val;
	u32 cn;
} isdbt_cn_raw_table[] = {
	{ 0x51, 0xb19ff }, { 0x5a, 0x9eecd }, { 0x65, 0x8cd8b },
	{ 0x72, 0x7c302 }, { 0x7f, 0x6f132 }, { 0x8f, 0x6250d },
	{ 0xa0, 0x57a1c }, { 0xb4, 0x4db45 }, { 0xc9, 0x45725 },
	{ 0xe2, 0x3da59 }, { 0xfd, 0x36f9d }, { 0x11c, 0x30e58 },
	{ 0x13f, 0x2b76e }, { 0x166, 0x26abb }, { 0x191, 0x22794 },
	{ 0x1c2, 0x1eac7 }, { 0x1f9, 0x1b4a8 }, { 0x237, 0x1844d },
	{ 0x27c, 0x159a2 }, { 0x2ca, 0x13365 }, { 0x321, 0x11196 },
	{ 0x382, 0xf3ae }, { 0x3f0, 0xd8cb }, { 0x46b, 0xc0fd },
	{ 0x4f4, 0xabf9 }, { 0x58f, 0x9923 }, { 0x63d, 0x8868 },
	{ 0x700, 0x7995 }, { 0x7da, 0x6c78 }, { 0x8cf, 0x60cf },
	{ 0x9e2, 0x5675 }, { 0xb17, 0x4d43 }, { 0xc71, 0x4520 },
	{ 0xdf6, 0x3de4 }, { 0xfaa, 0x377b }, { 0x1193, 0x31cc },
	{ 0x13b8, 0x2cbf }, { 0x1620, 0x2843 }, { 0x18d3, 0x2447 },
	{ 0x1bdb, 0x20bb }, { 0x1f41, 0x1d95 }, { 0x2311, 0x1ac6 },
	{ 0x2758, 0x1846 }, { 0x2c25, 0x160a }, { 0x3188, 0x140c },
	{ 0x3793, 0x1243 }, { 0x3e5b, 0x10ab }, { 0x45f7, 0xf3c },
	{ 0x4e80, 0xdf3 }
};

/* 0 to 20 dB, 0.1 dB unit */
static const struct {
	u16 val;
	u32 cn;
} isdbs_cn_raw_table[] = {
	{ 0x5af, 0x9546 }, { 0x597, 0x94d9 }, { 0x57e, 0x946b },
	{ 0x567, 0x93fc }, { 0x550, 0x938c }, { 0x539, 0x931b },
	{ 0x522, 0x92a8 }, { 0x50c, 0x9235 }, { 0x4f6, 0x91c1 },
	{ 0x4e1, 0x914b }, { 0x4cc, 0x90d5 }, { 0x4b6, 0x905d },
	{ 0x4a1, 0x8fe4 }, { 0x48c, 0x8f6a }, { 0x477, 0x8eef },
	{ 0x463, 0x8e72 }, { 0x44f, 0x8df5 }, { 0x43c, 0x8d76 },
	{ 0x428, 0x8cf5 }, { 0x416, 0x8c74 }, { 0x403, 0x8bf1 },
	{ 0x3ef, 0x8b6c }, { 0x3dc, 0x8ae7 }, { 0x3c9, 0x8a60 },
	{ 0x3b6, 0x89d7 }, { 0x3a4, 0x894d }, { 0x392, 0x88c2 },
	{ 0x381, 0x8835 }, { 0x36f, 0x87a6 }, { 0x35f, 0x8716 },
	{ 0x34e, 0x8685 }, { 0x33d, 0x85f1 }, { 0x32d, 0x855d },
	{ 0x31d, 0x84c6 }, { 0x30d, 0x842e }, { 0x2fd, 0x8394 },
	{ 0x2ee, 0x82f9 }, { 0x2df, 0x825b }, { 0x2d0, 0x81bc },
	{ 0x2c2, 0x811c }, { 0x2b4, 0x8079 }, { 0x2a6, 0x7fd5 },
	{ 0x299, 0x7f2f }, { 0x28c, 0x7e87 }, { 0x27f, 0x7ddd },
	{ 0x272, 0x7d31 }, { 0x265, 0x7c83 }, { 0x259, 0x7bd4 },
	{ 0x24d, 0x7b22 }, { 0x241, 0x7a6f }, { 0x236, 0x79ba },
	{ 0x22b, 0x7903 }, { 0x220, 0x784a }, { 0x215, 0x778f },
	{ 0x20a, 0x76d3 }, { 0x200, 0x7614 }, { 0x1f6, 0x7554 },
	{ 0x1ec, 0x7492 }, { 0x1e2, 0x73ce }, { 0x1d8, 0x7308 },
	{ 0x1cf, 0x7241 }, { 0x1c6, 0x7178 }, { 0x1bc, 0x70ad },
	{ 0x1b3, 0x6fe1 }, { 0x1aa, 0x6f13 }, { 0x1a2, 0x6e44 },
	{ 0x199, 0x6d74 }, { 0x191, 0x6ca2 }, { 0x189, 0x6bcf },
	{ 0x181, 0x6afb }, { 0x179, 0x6a26 }, { 0x171, 0x6950 },
	{ 0x169, 0x687a }, { 0x161, 0x67a2 }, { 0x15a, 0x66ca },
	{ 0x153, 0x65f1 }, { 0x14b, 0x6517 }, { 0x144, 0x643e },
	{ 0x13d, 0x6364 }, { 0x137, 0x628a }, { 0x130, 0x61b0 },
	{ 0x12a, 0x60d6 }, { 0x124, 0x5ffc }, { 0x11e, 0x5f22 },
	{ 0x118, 0x5e49 }, { 0x112, 0x5d70 }, { 0x10c, 0x5c98 },
	{ 0x107, 0x5bc0 }, { 0x101, 0x5ae9 }, { 0xfc, 0x5a13 },
	{ 0xf7, 0x593e }, { 0xf2, 0x5869 }, { 0xec, 0x5796 },
	{ 0xe7, 0x56c4 }, { 0xe2, 0x55f3 }, { 0xdd, 0x5523 },
	{ 0xd8, 0x5454 }, { 0xd4, 0x5387 }, { 0xcf, 0x52bb },
	{ 0xca, 0x51f0 }, { 0xc6, 0x5126 }, { 0xc2, 0x505e },
	{ 0xbe, 0x4f98 }, { 0xb9, 0x4ed3 }, { 0xb5, 0x4e0f },
	{ 0xb1, 0x4d4d }, { 0xae, 0x4c8d }, { 0xaa, 0x4bce },
	{ 0xa6, 0x4b10 }, { 0xa3, 0x4a55 }, { 0x9f, 0x499a },
	{ 0x9b, 0x48e1 }, { 0x98, 0x482a }, { 0x95, 0x4774 },
	{ 0x91, 0x46c0 }, { 0x8e, 0x460d }, { 0x8b, 0x455c },
	{ 0x88, 0x44ac }, { 0x85, 0x43fe }, { 0x82, 0x4352 },
	{ 0x7f, 0x42a6 }, { 0x7c, 0x41fd }, { 0x7a, 0x4154 },
	{ 0x77, 0x40ae }, { 0x74, 0x4008 }, { 0x72, 0x3f64 },
	{ 0x6f, 0x3ec2 }, { 0x6d, 0x3e21 }, { 0x6b, 0x3d81 },
	{ 0x68, 0x3ce3 }, { 0x66, 0x3c46 }, { 0x64, 0x3baa },
	{ 0x61, 0x3b10 }, { 0x5f, 0x3a77 }, { 0x5d, 0x39e0 },
	{ 0x5b, 0x394a }, { 0x59, 0x38b5 }, { 0x57, 0x3821 },
	{ 0x55, 0x378f }, { 0x53, 0x36fe }, { 0x51, 0x366e },
	{ 0x4f, 0x35e0 }, { 0x4e, 0x3553 }, { 0x4c, 0x34c7 },
	{ 0x4a, 0x343c }, { 0x49, 0x33b3 }, { 0x47, 0x332b },
	{ 0x45, 0x32a4 }, { 0x44, 0x321e }, { 0x42, 0x319a },
	{ 0x41, 0x3116 }, { 0x3f, 0x3094 }, { 0x3e, 0x3014 },
	{ 0x3c, 0x2f94 }, { 0x3b, 0x2f16 }, { 0x3a, 0x2e99 },
	{ 0x38, 0x2e1d }, { 0x37, 0x2da2 }, { 0x36, 0x2d29 },
	{ 0x34, 0x2cb0 }, { 0x33, 0x2c39 }, { 0x32, 0x2bc4 },
	{ 0x31, 0x2b4f }, { 0x30, 0x2adc }, { 0x2f, 0x2a6a },
	{ 0x2e, 0x29f9 }, { 0x2d, 0x2989 }, { 0x2c, 0x291a },
	{ 0x2b, 0x28ad }, { 0x2a, 0x2841 }, { 0x29, 0x27d6 },
	{ 0x28, 0x276d }, { 0x27, 0x2705 }, { 0x26, 0x269e },
	{ 0x25, 0x2638 }, { 0x24, 0x25d3 }, { 0x23, 0x2570 },
	{ 0x22, 0x24ad }, { 0x21, 0x244d }, { 0x20, 0x23ef },
	{ 0x1f, 0x2336 }, { 0x1e, 0x22db }, { 0x1d, 0x222a },
	{ 0x1c, 0x217d }, { 0x1b, 0x20d5 }, { 0x1a, 0x2083 },
	{ 0x19, 0x1fe3 }, { 0x18, 0x1f94 }, { 0x17, 0x1efb },
	{ 0x16, 0x1e66 }, { 0x15, 0x1dd6 }, { 0x14, 0x1d90 }
};

static int pxmlt_chrdev_read_cnr_raw(struct ptx_chrdev *chrdev, u32 *value)
{
	int ret = 0;
	struct pxmlt_chrdev *chrdevm = chrdev->priv;

	switch (chrdev->current_system) {
	case PTX_ISDB_T_SYSTEM:
	{
		int i, i_min, i_max;
		u16 val;

		ret = cxd2856er_read_cnr_raw_isdbt(&chrdevm->cxd2856er, &val);
		if (ret)
			break;

		i_min = 0;
		i_max = ARRAY_SIZE(isdbt_cn_raw_table) - 1;

		if (isdbt_cn_raw_table[i_min].val >= val) {
			*value = isdbt_cn_raw_table[i_min].cn;
			break;
		}
		if (isdbt_cn_raw_table[i_max].val <= val) {
			*value = isdbt_cn_raw_table[i_max].cn;
			break;
		}

		while (1) {
			i = i_min + (i_max - i_min) / 2;

			if (isdbt_cn_raw_table[i].val == val) {
				*value = isdbt_cn_raw_table[i].cn;
				break;
			}

			if (isdbt_cn_raw_table[i].val < val)
				i_min = i + 1;
			else
				i_max = i - 1;

			if (i_max < i_min) {
				*value = isdbt_cn_raw_table[i_max].cn;
				break;
			}
		}

		break;
	}

	case PTX_ISDB_S_SYSTEM:
	{
		int i, i_min, i_max;
		u16 val;

		ret = cxd2856er_read_cnr_raw_isdbs(&chrdevm->cxd2856er, &val);
		if (ret)
			break;

		i_min = 0;
		i_max = ARRAY_SIZE(isdbs_cn_raw_table) - 1;

		if (isdbs_cn_raw_table[i_min].val <= val) {
			*value = isdbs_cn_raw_table[i_min].cn;
			break;
		}
		if (isdbs_cn_raw_table[i_max].val >= val) {
			*value = isdbs_cn_raw_table[i_max].cn;
			break;
		}

		while (1) {
			i = i_min + (i_max - i_min) / 2;

			if (isdbs_cn_raw_table[i].val == val) {
				*value = isdbs_cn_raw_table[i].cn;
				break;
			}

			if (isdbs_cn_raw_table[i].val > val)
				i_min = i + 1;
			else
				i_max = i - 1;

			if (i_max < i_min) {
				*value = isdbs_cn_raw_table[i_max].cn;
				break;
			}
		}

		break;
	}

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static struct ptx_chrdev_operations pxmlt_chrdev_ops = {
	.init = pxmlt_chrdev_init,
	.term = pxmlt_chrdev_term,
	.open = pxmlt_chrdev_open,
	.release = pxmlt_chrdev_release,
	.tune = pxmlt_chrdev_tune,
	.check_lock = pxmlt_chrdev_check_lock,
	.set_stream_id = pxmlt_chrdev_set_stream_id,
	.set_lnb_voltage = pxmlt_chrdev_set_lnb_voltage,
	.set_capture = pxmlt_chrdev_set_capture,
	.read_signal_strength = NULL,
	.read_cnr = NULL,
	.read_cnr_raw = pxmlt_chrdev_read_cnr_raw
};

static const struct {
	u8 i2c_addr;
	u8 i2c_bus;
	u8 port_number;
} pxmlt_device_params[][5] = {
	/* PX-MLT5U */
	{ { 0x65, 3, 4 }, { 0x6c, 1, 3 }, { 0x64, 1, 1 }, { 0x6c, 3, 2 }, { 0x64, 3, 0 } },
	/* PX-MLT5PE */
	{ { 0x65, 3, 0 }, { 0x6c, 1, 1 }, { 0x64, 1, 2 }, { 0x6c, 3, 3 }, { 0x64, 3, 4 } },
	/* PX-MLT8PE3 */
	{ { 0x65, 3, 0 }, { 0x6c, 3, 3 }, { 0x64, 3, 4 }, { 0x00, 0, 1 }, { 0x00, 0, 2 } },
	/* PX-MLT8PE5 */
	{ { 0x65, 1, 0 }, { 0x64, 1, 1 }, { 0x6c, 1, 2 }, { 0x6c, 3, 3 }, { 0x64, 3, 4 } },
	/* ISDB6014 V2.0 (4TS) */
	{ { 0x65, 3, 0 }, { 0x6c, 1, 1 }, { 0x64, 1, 2 }, { 0x64, 3, 4 }, { 0x00, 0, 3 } }
};

static int pxmlt_device_load_config(struct pxmlt_device *pxmlt,
				    enum pxmlt_model model, 
				    struct ptx_chrdev_config *chrdev_config)
{
	int ret = 0, i;
	struct device *dev = pxmlt->dev;
	struct it930x_bridge *it930x = &pxmlt->it930x;
	u8 tmp;

	ret = it930x_read_reg(it930x, 0x4979, &tmp);
	if (ret) {
		dev_err(dev,
			"pxmlt_device_load_config: it930x_read_reg(0x4979) failed. (ret: %d)\n",
			ret);
		return ret;
	} else if (!tmp) {
		dev_warn(dev, "EEPROM error.\n");
		return ret;
	}

	for (i = 0; i < pxmlt->chrdevm_num; i++) {
		struct it930x_stream_input *input = &it930x->config.input[i];
		struct pxmlt_chrdev *chrdevm = &pxmlt->chrdevm[i];

		chrdev_config[i].system_cap = PTX_ISDB_T_SYSTEM | PTX_ISDB_S_SYSTEM;

		input->enable = true;
		input->is_parallel = false;
		input->port_number = pxmlt_device_params[model][i].port_number;
		input->slave_number = i;
		input->i2c_bus = pxmlt_device_params[model][i].i2c_bus;
		input->i2c_addr = pxmlt_device_params[model][i].i2c_addr;
		input->packet_len = 188;
		input->sync_byte = ((i + 1) << 4) | 0x07;	/* 0x17 0x27 0x37 0x47 0x57 */

		chrdevm->cxd2856er.dev = dev;
		chrdevm->cxd2856er.i2c = &it930x->i2c_master[input->i2c_bus - 1];
		chrdevm->cxd2856er.i2c_addr.slvx = input->i2c_addr + 2;
		chrdevm->cxd2856er.i2c_addr.slvt = input->i2c_addr;
		chrdevm->cxd2856er.config.xtal = 24000;
		chrdevm->cxd2856er.config.tuner_i2c = true;

		chrdevm->cxd2858er.dev = dev;
		chrdevm->cxd2858er.i2c = &chrdevm->cxd2856er.i2c_master;
		chrdevm->cxd2858er.i2c_addr = 0x60;
		chrdevm->cxd2858er.config.xtal = 16000;
		chrdevm->cxd2858er.config.ter.lna = true;
		chrdevm->cxd2858er.config.sat.lna = true;
	}

	for (i = pxmlt->chrdevm_num; i < PXMLT_CHRDEV_MAX_NUM; i++) {
		it930x->config.input[i].enable = false;
		it930x->config.input[i].port_number = pxmlt_device_params[model][i].port_number;
	}

	return 0;
}

int pxmlt_device_init(struct pxmlt_device *pxmlt, struct device *dev,
		      enum pxmlt_model model,
		      struct ptx_chrdev_context *chrdev_ctx,
		      struct completion *quit_completion)
{
	int ret = 0, i;
	struct it930x_bridge *it930x;
	struct itedtv_bus *bus;
	struct ptx_chrdev_config chrdev_config[PXMLT_CHRDEV_MAX_NUM];
	struct ptx_chrdev_group_config chrdev_group_config;
	struct ptx_chrdev_group *chrdev_group;
	struct pxmlt_stream_context *stream_ctx;

	if (!pxmlt || !dev || !chrdev_ctx || !quit_completion)
		return -EINVAL;

	dev_dbg(dev, "pxmlt_device_init\n");

	get_device(dev);

	mutex_init(&pxmlt->lock);
	kref_init(&pxmlt->kref);
	pxmlt->dev = dev;
	pxmlt->quit_completion = quit_completion;
	pxmlt->open_count = 0;
	pxmlt->lnb_power_count = 0;
	pxmlt->streaming_count = 0;
	mutex_init(&pxmlt->tuner_lock[0]);
	mutex_init(&pxmlt->tuner_lock[1]);
	switch (model) {
	case PXMLT8PE3_MODEL:
		pxmlt->chrdevm_num = 3;
		break;

	case ISDB6014_4TS_MODEL:
		pxmlt->chrdevm_num = 4;
		break;

	default:
		pxmlt->chrdevm_num = 5;
		break;
	}

	for (i = 0; i < pxmlt->chrdevm_num; i++) {
		struct pxmlt_chrdev *chrdevm = &pxmlt->chrdevm[i];
		int tuner_lock_idx = (pxmlt_device_params[model][i].i2c_bus == 3) ? 0
										  : 1;

		chrdevm->chrdev = NULL;
		chrdevm->parent = pxmlt;
		chrdevm->lnb_power = false;
		chrdevm->tuner_lock = &pxmlt->tuner_lock[tuner_lock_idx];
	}

	stream_ctx = kzalloc(sizeof(*stream_ctx), GFP_KERNEL);
	if (!stream_ctx) {
		dev_err(pxmlt->dev,
			"pxmlt_device_init: kzalloc(sizeof(*stream_ctx), GFP_KERNEL) failed.\n");
		ret = -ENOMEM;
		goto fail;
	}
	pxmlt->stream_ctx = stream_ctx;

	it930x = &pxmlt->it930x;
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

	ret = pxmlt_device_load_config(pxmlt, model, chrdev_config);
	if (ret)
		goto fail_device;

	for (i = 0; i < pxmlt->chrdevm_num; i++) {
		chrdev_config[i].ops = &pxmlt_chrdev_ops;
		chrdev_config[i].options = PTX_CHRDEV_SAT_SET_STREAM_ID_BEFORE_TUNE;
		chrdev_config[i].ringbuf_size = 188 * px4_device_params.tsdev_max_packets;
		chrdev_config[i].ringbuf_threshold_size = chrdev_config[i].ringbuf_size / 10;
		chrdev_config[i].priv = &pxmlt->chrdevm[i];
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

	ret = it930x_write_gpio(it930x, 7, true);
	if (ret)
		goto fail_device;

	ret = it930x_set_gpio_mode(it930x, 2, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	ret = it930x_write_gpio(it930x, 2, false);
	if (ret)
		goto fail_device;

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

		for (i = 0; i < pxmlt->chrdevm_num; i++) {
			ret = it930x_set_pid_filter(it930x, i, &filter);
			if (ret)
				goto fail_device;
		}
	}

	chrdev_group_config.owner_kref = &pxmlt->kref;
	chrdev_group_config.owner_kref_release = pxmlt_device_release;
	chrdev_group_config.reserved = false;
	chrdev_group_config.minor_base = 0;	/* unused */
	chrdev_group_config.chrdev_num = pxmlt->chrdevm_num;
	chrdev_group_config.chrdev_config = chrdev_config;

	ret = ptx_chrdev_context_add_group(chrdev_ctx, dev,
					   &chrdev_group_config, &chrdev_group);
	if (ret)
		goto fail_chrdev;

	pxmlt->chrdev_group = chrdev_group;

	for (i = 0; i < pxmlt->chrdevm_num; i++) {
		pxmlt->chrdevm[i].chrdev = &chrdev_group->chrdev[i];
		stream_ctx->chrdev[i] = &chrdev_group->chrdev[i];
	}

	atomic_set(&pxmlt->available, 1);
	return 0;

fail_chrdev:

fail_device:
	it930x_term(it930x);

fail_bridge:
	itedtv_bus_term(bus);

fail_bus:
	kfree(pxmlt->stream_ctx);

fail:
	mutex_destroy(&pxmlt->tuner_lock[0]);
	mutex_destroy(&pxmlt->tuner_lock[1]);
	mutex_destroy(&pxmlt->lock);
	put_device(dev);

	return ret;
}

static void pxmlt_device_release(struct kref *kref)
{
	struct pxmlt_device *pxmlt = container_of(kref,
						  struct pxmlt_device, kref);

	dev_dbg(pxmlt->dev, "pxmlt_device_release\n");

	it930x_term(&pxmlt->it930x);
	itedtv_bus_term(&pxmlt->it930x.bus);

	kfree(pxmlt->stream_ctx);

	mutex_destroy(&pxmlt->tuner_lock[0]);
	mutex_destroy(&pxmlt->tuner_lock[1]);
	mutex_destroy(&pxmlt->lock);
	put_device(pxmlt->dev);

	complete(pxmlt->quit_completion);
	return;
}

void pxmlt_device_term(struct pxmlt_device *pxmlt)
{
	dev_dbg(pxmlt->dev, "pxmlt_device_term\n");

	atomic_xchg(&pxmlt->available, 0);
	ptx_chrdev_group_destroy(pxmlt->chrdev_group);

	kref_put(&pxmlt->kref, pxmlt_device_release);
	return;
}
