// SPDX-License-Identifier: GPL-2.0-only
/*
 * Digibest ISDB2056 driver (isdb2056.c)
 *
 * Copyright (c) 2019 nns779
 */

#include "print_format.h"

#include <linux/version.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/usb.h>
#include <linux/cdev.h>

#include "isdb2056.h"
#include "firmware.h"
#include "ptx_ioctl.h"
#include "module_param.h"
#include "i2c_comm.h"
#include "it930x-bus.h"
#include "it930x.h"
#include "tc90522.h"
#include "r850.h"
#include "rt710.h"
#include "ringbuffer.h"

#if !defined(MAX_DEVICE) || !MAX_DEVICE
#undef MAX_DEVICE
#define MAX_DEVICE	16
#endif
#define TSDEV_NUM	1
#define MAX_TSDEV	(MAX_DEVICE * TSDEV_NUM)
#define DEVICE_NAME	"isdb2056"

#define PID_ISDB2056	0x004b

#define ISDB_NONE	0
#define ISDB_T		1
#define ISDB_S		2

#define TS_SYNC_COUNT	4
#define TS_SYNC_SIZE	(188 * TS_SYNC_COUNT)

struct isdb2056_tsdev {
	struct mutex lock;
	unsigned int id;
	int isdb;				// ISDB_NONE or ISDB_T or ISDB_S
	bool init;
	bool open;
	struct tc90522_demod tc90522_t;		// for ISDB-T
	struct tc90522_demod tc90522_s;		// for ISDB-S
	struct r850_tuner r850;			// for ISDB-T
	struct rt710_tuner rt710;		// for ISDB-S
	atomic_t streaming;			// 0: not streaming, !0: streaming
	struct ringbuffer *ringbuf;
};

struct isdb2056_stream_context {
	struct ringbuffer *ringbuf[TSDEV_NUM];
	u8 remain_buf[TS_SYNC_SIZE];
	size_t remain_len;
};

struct isdb2056_device {
	struct device *dev;
	struct mutex lock;
	int dev_idx;
	atomic_t ref;				// reference counter
	atomic_t avail;				// availability flag
	wait_queue_head_t wait;
	unsigned long long serial_number;
	unsigned int dev_id;			// 1 or 2
	struct it930x_bridge it930x;
	unsigned int streaming_count;
	struct isdb2056_tsdev tsdev[TSDEV_NUM];
	struct isdb2056_stream_context *stream_context;
	struct cdev cdev;
	struct workqueue_struct *dev_wq;
	struct delayed_work dev_work;
#ifdef PSB_DEBUG
	struct workqueue_struct *psb_wq;
	struct delayed_work psb_work;
#endif
};

static DEFINE_MUTEX(glock);
static struct class *isdb2056_class = NULL;
static dev_t isdb2056_dev_first;
static struct isdb2056_device *devs[MAX_DEVICE];

static const struct usb_device_id isdb2056_usb_ids[] = {
	{ USB_DEVICE(0x0511, PID_ISDB2056) },
	{ 0 },
};

MODULE_DEVICE_TABLE(usb, isdb2056_usb_ids);

static int isdb2056_init(struct isdb2056_device *isdb2056)
{
	int ret = 0;
	int i;

	atomic_set(&isdb2056->ref, 1);
	atomic_set(&isdb2056->avail, 1);
	init_waitqueue_head(&isdb2056->wait);
	mutex_init(&isdb2056->lock);
	isdb2056->streaming_count = 0;

	for (i = 0; i < TSDEV_NUM; i++) {
		struct isdb2056_tsdev *tsdev = &isdb2056->tsdev[i];

		mutex_init(&tsdev->lock);
		tsdev->id = i;
		tsdev->init = false;
		tsdev->open = false;
		atomic_set(&tsdev->streaming, 0);

		ret = ringbuffer_create(&tsdev->ringbuf);
		if (ret)
			break;

		ret = ringbuffer_alloc(tsdev->ringbuf, 188 * tsdev_max_packets);
		if (ret)
			break;

		isdb2056->stream_context->ringbuf[i] = tsdev->ringbuf;
	}

	return ret;
}

static int isdb2056_term(struct isdb2056_device *isdb2056)
{
	int i;

	for (i = 0; i < TSDEV_NUM; i++) {
		struct isdb2056_tsdev *tsdev = &isdb2056->tsdev[i];

		ringbuffer_destroy(tsdev->ringbuf);
	}

	return 0;
}

static int isdb2056_ref(struct isdb2056_device *isdb2056)
{
	return atomic_add_return(1, &isdb2056->ref);
}

static int isdb2056_unref(struct isdb2056_device *isdb2056)
{
	return atomic_sub_return(1, &isdb2056->ref);
}

static int isdb2056_load_config(struct isdb2056_device *isdb2056)
{
	int ret = 0;
	struct device *dev = isdb2056->dev;
	struct it930x_bridge *it930x = &isdb2056->it930x;
	u8 tmp;
	int i;

	ret = it930x_read_reg(it930x, 0x4979, &tmp);
	if (ret) {
		dev_err(dev, "isdb2056_load_config: it930x_read_reg(0x4979) failed.\n");
		return ret;
	} else if (!tmp) {
		dev_warn(dev, "EEPROM is invalid.\n");
		return ret;
	}

	isdb2056->tsdev[0].isdb = ISDB_NONE;

	it930x->config.input[0].i2c_addr = 0x10;

	for (i = 0; i < TSDEV_NUM; i++) {
		struct it930x_stream_input *input = &it930x->config.input[i];
		struct isdb2056_tsdev *tsdev = &isdb2056->tsdev[i];

		input->enable = true;
		input->is_parallel = false;
		input->port_number = i;
		input->slave_number = i;
		input->i2c_bus = 3;
		input->packet_len = 188;
		input->sync_byte = ((i + 1) << 4) | 0x07;	// 0x17

		tsdev->tc90522_t.dev = dev;
		tsdev->tc90522_t.i2c = &it930x->i2c_master[2];
		tsdev->tc90522_t.i2c_addr = 0x10;
		tsdev->tc90522_t.is_secondary = false;

		tsdev->tc90522_s.dev = dev;
		tsdev->tc90522_s.i2c = &it930x->i2c_master[2];
		tsdev->tc90522_s.i2c_addr = 0x11;
		tsdev->tc90522_s.is_secondary = false;

		tsdev->r850.dev = dev;
		tsdev->r850.i2c = &tsdev->tc90522_t.i2c_master;
		tsdev->r850.i2c_addr = 0x7c;
		tsdev->r850.config.xtal = 24000;
		tsdev->r850.config.loop_through = false;
		tsdev->r850.config.clock_out = false;
		tsdev->r850.config.no_imr_calibration = true;
		tsdev->r850.config.no_lpf_calibration = true;

		tsdev->rt710.dev = dev;
		tsdev->rt710.i2c = &tsdev->tc90522_s.i2c_master;
		tsdev->rt710.i2c_addr = 0x7a;
		tsdev->rt710.config.xtal = 24000;
		tsdev->rt710.config.loop_through = false;
		tsdev->rt710.config.clock_out = false;
		tsdev->rt710.config.signal_output_mode = RT710_SIGNAL_OUTPUT_DIFFERENTIAL;
		tsdev->rt710.config.agc_mode = RT710_AGC_POSITIVE;
		tsdev->rt710.config.vga_atten_mode = RT710_VGA_ATTEN_OFF;
		tsdev->rt710.config.fine_gain = RT710_FINE_GAIN_3DB;
		tsdev->rt710.config.scan_mode = RT710_SCAN_MANUAL;
	}

	for (i = TSDEV_NUM; i < 5; i++) {
		it930x->config.input[i].enable = false;
		it930x->config.input[i].port_number = i;
	}

	return 0;
}

static int isdb2056_set_power(struct isdb2056_device *isdb2056, bool on)
{
	int ret = 0, i;
	struct it930x_bridge *it930x = &isdb2056->it930x;

	dev_dbg(isdb2056->dev, "isdb2056_set_power: %s\n", on ? "on" : "off");

	if (on) {
		ret = it930x_write_gpio(it930x, 3, false);
		if (ret)
			goto exit;

		msleep(100);

		ret = it930x_write_gpio(it930x, 2, true);
		if (ret)
			goto exit;

		msleep(20);

		for (i = 0; i < TSDEV_NUM; i++) {
			struct isdb2056_tsdev *t = &isdb2056->tsdev[i];

			ret = tc90522_init(&t->tc90522_t);
			if (ret) {
				dev_err(isdb2056->dev, "isdb2056_set_power: tc90522_init(%d) (t) failed. (ret: %d)\n", i, ret);
				break;
			}

			ret = tc90522_init(&t->tc90522_s);
			if (ret) {
				dev_err(isdb2056->dev, "isdb2056_set_power: tc90522_init(%d) (s) failed. (ret: %d)\n", i, ret);
				break;
			}

			ret = r850_init(&t->r850);
			if (ret) {
				dev_err(isdb2056->dev, "isdb2056_set_power: r850_init(%d) failed. (ret: %d)\n", i, ret);
				break;
			}

			ret = rt710_init(&t->rt710);
			if (ret) {
				dev_err(isdb2056->dev, "isdb2056_set_power: rt710_init(%d) failed. (ret: %d)\n", i, ret);
				break;
			}
		}
	} else {
		for (i = 0; i < TSDEV_NUM; i++) {
			struct isdb2056_tsdev *t = &isdb2056->tsdev[i];

			r850_term(&t->r850);
			rt710_term(&t->rt710);

			tc90522_term(&t->tc90522_t);
			tc90522_term(&t->tc90522_s);
		}

		it930x_write_gpio(it930x, 2, false);
		it930x_write_gpio(it930x, 3, true);

		msleep(50);
	}

exit:
	if (ret)
		dev_err(isdb2056->dev, "isdb2056_set_power: failed.\n");
	else
		dev_dbg(isdb2056->dev, "isdb2056_set_power: ok\n");

	return ret;
}

static bool isdb2056_ts_sync(u8 **buf, u32 *len, bool *sync_remain)
{
	bool ret = false;
	u8 *p = *buf;
	u32 remain = *len;
	bool b = false;

	while (remain) {
		u32 i;

		for (i = 0; i < TS_SYNC_COUNT; i++) {
			if (((i + 1) * 188) <= remain) {
				if ((p[i * 188] & 0x8f) != 0x07)
					break;
			} else {
				b = true;
				break;
			}
		}

		if (i == TS_SYNC_COUNT) {
			// ok
			ret = true;
			break;
		}

		if (b)
			break;

		p += 1;
		remain -= 1;
	}

	*buf = p;
	*len = remain;
	*sync_remain = b;

	return ret;
}

static void isdb2056_ts_write(struct ringbuffer **ringbuf, u8 **buf, u32 *len)
{
	u8 *p = *buf;
	u32 remain = *len;

	while (remain >= 188 && ((p[0] & 0x8f) == 0x07)) {
		u8 id = (p[0] & 0x70) >> 4;

		if (id == 1) {
			p[0] = 0x47;
			ringbuffer_write_atomic(ringbuf[id - 1], p, 188);
		} else {
			pr_debug("isdb2056_ts_write: unknown id %d\n", id);
		}

		p += 188;
		remain -= 188;
	}

	*buf = p;
	*len = remain;

	return;
}

static int isdb2056_stream_handler(void *context, void *buf, u32 len)
{
	struct isdb2056_stream_context *stream_context = context;
	u8 *context_remain_buf = stream_context->remain_buf;
	u32 context_remain_len = stream_context->remain_len;
	u8 *p = buf;
	u32 remain = len;
	bool sync_remain = false;

	if (context_remain_len) {
		if ((context_remain_len + len) >= TS_SYNC_SIZE) {
			u32 l;

			l = TS_SYNC_SIZE - context_remain_len;

			memcpy(context_remain_buf + context_remain_len, p, l);
			context_remain_len = TS_SYNC_SIZE;

			if (isdb2056_ts_sync(&context_remain_buf, &context_remain_len, &sync_remain)) {
				isdb2056_ts_write(stream_context->ringbuf, &context_remain_buf, &context_remain_len);

				p += l;
				remain -= l;
			}

			stream_context->remain_len = 0;
		} else {
			memcpy(context_remain_buf + context_remain_len, p, len);
			stream_context->remain_len += len;

			return 0;
		}
	}

	while (remain) {
		if (!isdb2056_ts_sync(&p, &remain, &sync_remain))
			break;

		isdb2056_ts_write(stream_context->ringbuf, &p, &remain);
	}

	if (sync_remain) {
		memcpy(stream_context->remain_buf, p, remain);
		stream_context->remain_len = remain;
	}

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

static struct tc90522_regbuf tc_init_s[] = {
	{ 0x15, NULL, { 0x00 } },
	{ 0x1d, NULL, { 0x00 } },
};

// This function must be called after power on.
static int isdb2056_tsdev_init(struct isdb2056_tsdev *tsdev)
{
	int ret = 0;
	struct isdb2056_device *isdb2056 = container_of(tsdev, struct isdb2056_device, tsdev[tsdev->id]);
	struct tc90522_demod *tc90522_t = &tsdev->tc90522_t, *tc90522_s = &tsdev->tc90522_s;
	struct r850_system_config sys;

	if (tsdev->init)
		// already initialized
		return 0;

	// Initialization for ISDB-T

	ret = tc90522_write_multiple_regs(tc90522_t, tc_init_t, ARRAY_SIZE(tc_init_t));
	if (ret) {
		dev_err(isdb2056->dev, "isdb2056_tsdev_init %d:%u: tc90522_write_multiple_regs(tc_init_t) failed. (ret: %d)\n", isdb2056->dev_idx, tsdev->id, ret);
		goto exit;
	}

	// disable ts pins
	ret = tc90522_enable_ts_pins_t(tc90522_t, false);
	if (ret) {
		dev_err(isdb2056->dev, "isdb2056_tsdev_init %d:%u: tc90522_enable_ts_pins_t(false) failed. (ret: %d)\n", isdb2056->dev_idx, tsdev->id, ret);
		goto exit;
	}

	// sleep
	ret = tc90522_sleep_t(tc90522_t, true);
	if (ret) {
		dev_err(isdb2056->dev, "isdb2056_tsdev_init %d:%u: tc90522_sleep_t(true) failed. (ret: %d)\n", isdb2056->dev_idx, tsdev->id, ret);
		goto exit;
	}

	sys.system = R850_SYSTEM_ISDB_T;
	sys.bandwidth = R850_BANDWIDTH_6M;
	sys.if_freq = 4063;

	ret = r850_set_system(&tsdev->r850, &sys);
	if (ret) {
		dev_err(isdb2056->dev, "isdb2056_tsdev_init %d:%u: r850_set_system() failed. (ret: %d)\n", isdb2056->dev_idx, tsdev->id, ret);
		goto exit;
	}

	// Initialization for ISDB-S

	ret = tc90522_write_multiple_regs(tc90522_s, tc_init_s, ARRAY_SIZE(tc_init_s));
	if (ret) {
		dev_err(isdb2056->dev, "isdb2056_tsdev_init %d:%u: tc90522_write_multiple_regs(tc_init_s) failed. (ret: %d)\n", isdb2056->dev_idx, tsdev->id, ret);
		goto exit;
	}

	// disable ts pins
	ret = tc90522_enable_ts_pins_s(tc90522_s, false);
	if (ret) {
		dev_err(isdb2056->dev, "isdb2056_tsdev_init %d:%u: tc90522_enable_ts_pins_s(false) failed. (ret: %d)\n", isdb2056->dev_idx, tsdev->id, ret);
		goto exit;
	}

	// sleep
	ret = tc90522_sleep_s(tc90522_s, true);
	if (ret) {
		dev_err(isdb2056->dev, "isdb2056_tsdev_init %d:%u: tc90522_sleep_s(true) failed. (ret: %d)\n", isdb2056->dev_idx, tsdev->id, ret);
		goto exit;
	}

exit:
	if (!ret)
		tsdev->init = true;

	return ret;
}

static void isdb2056_tsdev_term(struct isdb2056_tsdev *tsdev)
{
	struct tc90522_demod *tc90522_t = &tsdev->tc90522_t, *tc90522_s = &tsdev->tc90522_s;

	if (!tsdev->init)
		return;

	r850_sleep(&tsdev->r850);

	if (!s_tuner_no_sleep)
		rt710_sleep(&tsdev->rt710);

	tc90522_sleep_t(tc90522_t, true);
	tc90522_sleep_s(tc90522_s, true);

	tsdev->init = false;

	return;
}

static int isdb2056_tsdev_set_channel(struct isdb2056_tsdev *tsdev, struct ptx_freq *freq)
{
	struct isdb2056_device *isdb2056 = container_of(tsdev, struct isdb2056_device, tsdev[tsdev->id]);
	int ret = 0, dev_idx = isdb2056->dev_idx;
	unsigned int tsdev_id = tsdev->id;
	struct tc90522_demod *tc90522_t = &tsdev->tc90522_t, *tc90522_s = &tsdev->tc90522_s;
	int mode;
	u32 real_freq;

	dev_dbg(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: freq_no: %d, slot: %d\n", dev_idx, tsdev_id, freq->freq_no, freq->slot);

	if (freq->freq_no < 0) {
		ret = -EINVAL;
	} else if (freq->freq_no < 12) {
		// BS
		if (freq->slot >= 8) {
			ret = -EINVAL;
		} else {
			real_freq = 1049480 + (38360 * freq->freq_no);
			mode = ISDB_S;
		}
	} else if (freq->freq_no < 24) {
		// CS
		real_freq = 1613000 + (40000 * (freq->freq_no - 12));
		mode = ISDB_S;
	} else if ((freq->freq_no >= 3 && freq->freq_no <= 12) || (freq->freq_no >= 22 && freq->freq_no <= 62)) {
		// CATV C13-C22ch, C23-63ch
		real_freq = 93143 + freq->freq_no * 6000 + freq->slot/* addfreq */;
		mode = ISDB_T;

		if (freq->freq_no == 12)
			real_freq += 2000;
	} else if (freq->freq_no >= 63 && freq->freq_no <= 102) {
		// UHF 13-52ch
		real_freq = 95143 + freq->freq_no * 6000 + freq->slot/* addfreq */;
		mode = ISDB_T;
	} else {
		// Unknown channel
		ret = -EINVAL;
	}

	if (ret)
		goto exit;

	switch (mode) {
	case ISDB_T:
	{
		int i;
		bool tuner_locked, demod_locked;

		ret = tc90522_write_reg(tc90522_t, 0x47, 0x30);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_write_reg(0x47, 0x30) (t) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = tc90522_set_agc_t(tc90522_t, false);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_set_agc_t(false) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = tc90522_sleep_s(tc90522_s, true);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_sleep_s(true) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = tc90522_write_reg(tc90522_t, 0x0e, 0x77);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_write_reg(0x0e, 0x77) (t) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = tc90522_write_reg(tc90522_t, 0x0f, 0x10);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_write_reg(0x0f, 0x10) (t) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = tc90522_write_reg(tc90522_t, 0x71, 0x20);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_write_reg(0x71, 0x20) (t) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = tc90522_sleep_t(tc90522_t, false);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_sleep_t(false) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = tc90522_write_reg(tc90522_t, 0x76, 0x0c);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_write_reg(0x76, 0x0c) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = tc90522_write_reg(tc90522_t, 0x1f, 0x30);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_write_reg(0x1f, 0x30) (t) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = r850_wakeup(&tsdev->r850);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: r850_wakeup() failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = r850_set_frequency(&tsdev->r850, real_freq);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: r850_set_frequency(%u) failed. (ret: %d)\n", dev_idx, tsdev_id, real_freq, ret);
			break;
		}

		i = 50;
		while (i--) {
			ret = r850_is_pll_locked(&tsdev->r850, &tuner_locked);
			if (!ret && tuner_locked)
				break;

			msleep(10);
		}

		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: r850_is_pll_locked() failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		} else if (!tuner_locked) {
			// PLL error
			dev_dbg(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: PLL is NOT locked.\n", dev_idx, tsdev_id);
			ret = -EAGAIN;
			break;
		}

		dev_dbg(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: PLL is locked. count: %d\n", dev_idx, tsdev_id, i);

		ret = tc90522_set_agc_t(tc90522_t, true);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_set_agc_t(true) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = tc90522_write_reg(tc90522_t, 0x71, 0x01);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_write_reg(0x71, 0x01) (t) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = tc90522_write_reg(tc90522_t, 0x72, 0x25);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_write_reg(0x72, 0x25) (t) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = tc90522_write_reg(tc90522_t, 0x75, 0x00);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_write_reg(0x75, 0x00) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		// check lock

		i = 300;
		while (i--) {
			ret = tc90522_is_signal_locked_t(tc90522_t, &demod_locked);
			if (!ret && demod_locked)
				break;

			msleep(10);
		}

		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_is_signal_locked_t() failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		dev_dbg(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_is_signal_locked_t() locked: %d, count: %d\n", dev_idx, tsdev_id, demod_locked, i);

		if (!demod_locked) {
			ret = -EAGAIN;
			break;
		}

		if (i > 265)
			msleep((i - 265) * 10);

		tsdev->isdb = ISDB_T;

		break;
	}

	case ISDB_S:
	{
		int i;
		struct rt710_tuner *rt710 = &tsdev->rt710;
		bool tuner_locked, demod_locked;
		s32 ss = 0;
		u16 tsid, tsid2;

		// set frequency

		ret = tc90522_set_agc_s(tc90522_s, false);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_set_agc_s(false) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = tc90522_write_reg(tc90522_t, 0x0e, 0x11);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_write_reg(0x0e, 0x11) (t) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = tc90522_write_reg(tc90522_t, 0x0f, 0x70);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_write_reg(0x0f, 0x70) (t) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = tc90522_sleep_t(tc90522_t, true);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_sleep_t(true) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = tc90522_write_reg(tc90522_s, 0x07, 0x77);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_write_reg(0x07, 0x77) (s) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = tc90522_write_reg(tc90522_s, 0x08, 0x10);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_write_reg(0x08, 0x10) (s) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = tc90522_sleep_s(tc90522_s, false);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_sleep_s(false) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = tc90522_write_reg(tc90522_s, 0x04, 0x02);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_write_reg(0x04, 0x02) (s) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = tc90522_write_reg(tc90522_s, 0x8e, 0x02);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_write_reg(0x8e, 0x02) (s) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = tc90522_write_reg(tc90522_t, 0x1f, 0x20);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_write_reg(0x1f, 0x20) (t) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = rt710_set_params(rt710, real_freq, 28860, 4);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: rt710_set_params(%u, 28860, 4) failed. (ret: %d)\n", dev_idx, tsdev_id, real_freq, ret);
			break;
		}

		i = 50;
		while (i--) {
			ret = rt710_is_pll_locked(rt710, &tuner_locked);
			if (!ret && tuner_locked)
				break;

			msleep(10);
		}

		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: rt710_is_pll_locked() failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		} else if (!tuner_locked) {
			// PLL error
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: PLL is NOT locked.\n", dev_idx, tsdev_id);
			ret = -EAGAIN;
			break;
		}

		rt710_get_rf_signal_strength(rt710, &ss);

		dev_dbg(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: PLL is locked. count: %d, signal strength: %ddBm\n", dev_idx, tsdev_id, i, ss);

		ret = tc90522_set_agc_s(tc90522_s, true);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_set_agc_s(true) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		// check lock

		i = 300;
		while (i--) {
			ret = tc90522_is_signal_locked_s(tc90522_s, &demod_locked);
			if (!ret && demod_locked)
				break;

			msleep(10);
		}

		if (ret) {
			dev_dbg(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_is_signal_locked_s() failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		dev_dbg(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_is_signal_locked_s() locked: %d, count: %d\n", dev_idx, tsdev_id, demod_locked, i);

		if (!demod_locked) {
			ret = -EAGAIN;
			break;
		}

		// set slot

		i = 100;
		while (i--) {
			ret = tc90522_tmcc_get_tsid_s(tc90522_s, freq->slot, &tsid);
			if ((!ret && tsid) || ret == -EINVAL)
				break;

			msleep(10);
		}

		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_tmcc_get_tsid_s() failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		dev_dbg(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_tmcc_get_tsid_s() tsid: 0x%04x, count: %d\n", dev_idx, tsdev_id, tsid, i);

		if (!tsid) {
			ret = -EAGAIN;
			break;
		}

		ret = tc90522_set_tsid_s(tc90522_s, tsid);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_set_tsid_s(0x%x) failed. (ret: %d)\n", dev_idx, tsdev_id, tsid, ret);
			break;
		}

		// check slot

		i = 100;
		while(i--) {
			ret = tc90522_get_tsid_s(tc90522_s, &tsid2);
			if (!ret && tsid2 == tsid)
				break;

			msleep(10);
		}

		dev_dbg(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: tc90522_get_tsid_s() tsid2: 0x%04x, count: %d\n", dev_idx, tsdev_id, tsid2, i);

		if (tsid2 != tsid) {
			ret = -EAGAIN;
			break;
		}

		tsdev->isdb = ISDB_S;

		break;
	}

	default:
		ret = -EIO;
		break;
	}

exit:
	if (!ret)
		dev_dbg(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: succeeded.\n", dev_idx, tsdev_id);
	else
		dev_dbg(isdb2056->dev, "isdb2056_tsdev_set_channel %d:%u: failed. (ret: %d)\n", dev_idx, tsdev_id, ret);

	return ret;
}

#ifdef PSB_DEBUG
static void isdb2056_psb_workqueue_handler(struct work_struct *w)
{
	int ret = 0;
	struct isdb2056_device *isdb2056 = container_of(to_delayed_work(w), struct isdb2056_device, psb_work);
	struct it930x_bridge *it930x = &isdb2056->it930x;
	u8 val[2];

	ret = it930x_read_regs(it930x, 0xda98, val, 2);
	if (ret)
		return;

	dev_info(isdb2056->dev, "psb count: 0x%x\n", (val[0] | (val[1] << 8)));

	mutex_lock(&isdb2056->lock);

	if (isdb2056->streaming_count)
		queue_delayed_work(isdb2056->psb_wq, to_delayed_work(w), msecs_to_jiffies(1000));

	mutex_unlock(&isdb2056->lock);

	return;
}
#endif

static int isdb2056_tsdev_start_streaming(struct isdb2056_tsdev *tsdev)
{
	int ret = 0;
	struct isdb2056_device *isdb2056 = container_of(tsdev, struct isdb2056_device, tsdev[tsdev->id]);
	struct it930x_bus *bus = &isdb2056->it930x.bus;
	struct tc90522_demod *tc90522_t = &tsdev->tc90522_t, *tc90522_s = &tsdev->tc90522_s;
	unsigned int ringbuffer_size;
	unsigned int streaming_count;

	if (atomic_read(&tsdev->streaming))
		// already started
		return 0;

	atomic_set(&tsdev->streaming, 1);

	mutex_lock(&isdb2056->lock);

	if (!isdb2056->streaming_count) {
		bus->usb.streaming_urb_buffer_size = 188 * urb_max_packets;
		bus->usb.streaming_urb_num = max_urbs;
		bus->usb.streaming_no_dma = no_dma;

		dev_dbg(isdb2056->dev, "isdb2056_tsdev_start_streaming %d:%u: urb_buffer_size: %u, urb_num: %u, no_dma: %c\n", isdb2056->dev_idx, tsdev->id, bus->usb.streaming_urb_buffer_size, bus->usb.streaming_urb_num, (bus->usb.streaming_no_dma) ? 'Y' : 'N');

		ret = it930x_purge_psb(&isdb2056->it930x, psb_purge_timeout);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_start_streaming %d:%u: it930x_purge_psb() failed. (ret: %d)\n", isdb2056->dev_idx, tsdev->id, ret);
			goto fail;
		}
	}

	ringbuffer_size = 188 * tsdev_max_packets;
	dev_dbg(isdb2056->dev, "isdb2056_tsdev_start_streaming %d:%u: size of ringbuffer: %u\n", isdb2056->dev_idx, tsdev->id, ringbuffer_size);

	switch (tsdev->isdb) {
	case ISDB_T:
		// enable ts pins
		ret = tc90522_enable_ts_pins_t(tc90522_t, true);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_start_streaming %d:%u: tc90522_enable_ts_pins_t(true) failed. (ret: %d)\n", isdb2056->dev_idx, tsdev->id, ret);

			// disable ts pins
			tc90522_enable_ts_pins_t(tc90522_t, false);
		}
		break;

	case ISDB_S:
		// enable ts pins
		ret = tc90522_enable_ts_pins_s(tc90522_s, true);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_start_streaming %d:%u: tc90522_enable_ts_pins_s(true) failed. (ret: %d)\n", isdb2056->dev_idx, tsdev->id, ret);

			// disable ts pins
			tc90522_enable_ts_pins_s(tc90522_s, false);
		}
		break;

	default:
		ret = -EIO;
		break;
	}

	if (ret)
		goto fail;

	ret = ringbuffer_alloc(tsdev->ringbuf, ringbuffer_size);
	if (ret)
		goto fail;

	ret = ringbuffer_start(tsdev->ringbuf);
	if (ret)
		goto fail;

	if (!isdb2056->streaming_count) {
		isdb2056->stream_context->remain_len = 0;

		dev_dbg(isdb2056->dev, "isdb2056_tsdev_start_streaming %d:%u: starting...\n", isdb2056->dev_idx, tsdev->id);
		ret = it930x_bus_start_streaming(bus, isdb2056_stream_handler, isdb2056->stream_context);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_start_streaming %d:%u: it930x_bus_start_streaming() failed. (ret: %d)\n", isdb2056->dev_idx, tsdev->id, ret);
			goto fail_after_ringbuffer;
		}

#ifdef PSB_DEBUG
		INIT_DELAYED_WORK(&isdb2056->psb_work, isdb2056_psb_workqueue_handler);

		if (!isdb2056->psb_wq)
			isdb2056->psb_wq = create_singlethread_workqueue("isdb2056_psb_workqueue");

		if (isdb2056->psb_wq)
			queue_delayed_work(isdb2056->psb_wq, &isdb2056->psb_work, msecs_to_jiffies(1000));
#endif
	}

	isdb2056->streaming_count++;
	streaming_count = isdb2056->streaming_count;

	mutex_unlock(&isdb2056->lock);

	dev_dbg(isdb2056->dev, "isdb2056_tsdev_start_streaming %d:%u: streaming_count: %u\n", isdb2056->dev_idx, tsdev->id, streaming_count);

	return ret;

fail_after_ringbuffer:
	ringbuffer_stop(tsdev->ringbuf);
fail:
	mutex_unlock(&isdb2056->lock);
	atomic_set(&tsdev->streaming, 0);

	dev_err(isdb2056->dev, "isdb2056_tsdev_start_streaming %d:%u: failed. (ret: %d)\n", isdb2056->dev_idx, tsdev->id, ret);

	return ret;
}

static int isdb2056_tsdev_stop_streaming(struct isdb2056_tsdev *tsdev, bool avail)
{
	int ret = 0;
	struct isdb2056_device *isdb2056 = container_of(tsdev, struct isdb2056_device, tsdev[tsdev->id]);
	struct tc90522_demod *tc90522_t = &tsdev->tc90522_t, *tc90522_s = &tsdev->tc90522_s;
	unsigned int streaming_count;

	if (!atomic_read(&tsdev->streaming))
		// already stopped
		return 0;

	atomic_set(&tsdev->streaming, 0);

	mutex_lock(&isdb2056->lock);

	isdb2056->streaming_count--;
	if (!isdb2056->streaming_count) {
		dev_dbg(isdb2056->dev, "isdb2056_tsdev_stop_streaming %d:%u: stopping...\n", isdb2056->dev_idx, tsdev->id);
		it930x_bus_stop_streaming(&isdb2056->it930x.bus);

#ifdef PSB_DEBUG
		if (isdb2056->psb_wq) {
			cancel_delayed_work_sync(&isdb2056->psb_work);
			flush_workqueue(isdb2056->psb_wq);
			destroy_workqueue(isdb2056->psb_wq);
			isdb2056->psb_wq = NULL;
		}
#endif
	}
	streaming_count = isdb2056->streaming_count;

	mutex_unlock(&isdb2056->lock);

	ringbuffer_stop(tsdev->ringbuf);

	if (!avail)
		return 0;

	switch (tsdev->isdb) {
	case ISDB_T:
		// disable ts pins
		ret = tc90522_enable_ts_pins_t(tc90522_t, false);
		break;

	case ISDB_S:
		// disable ts pins
		ret = tc90522_enable_ts_pins_s(tc90522_s, false);
		break;

	default:
		ret = -EIO;
		break;
	}

	dev_dbg(isdb2056->dev, "isdb2056_tsdev_stop_streaming %d:%u: streaming_count: %u\n", isdb2056->dev_idx, tsdev->id, streaming_count);

	return ret;
}

static int isdb2056_tsdev_get_cn(struct isdb2056_tsdev *tsdev, u32 *cn)
{
	int ret = 0;
	struct tc90522_demod *tc90522_t = &tsdev->tc90522_t, *tc90522_s = &tsdev->tc90522_s;

	switch (tsdev->isdb) {
	case ISDB_T:
		ret = tc90522_get_cndat_t(tc90522_t, cn);
		break;

	case ISDB_S:
		ret = tc90522_get_cn_s(tc90522_s, (u16 *)cn);
		break;

	default:
		ret = -EIO;
		break;
	}

	return ret;
}

static int isdb2056_tsdev_open(struct inode *inode, struct file *file)
{
	int ret = 0, ref;
	int minor = (iminor(inode) - MINOR(isdb2056_dev_first));
	int dev_idx = (minor / TSDEV_NUM);
	unsigned int tsdev_id = (minor % TSDEV_NUM);
	struct isdb2056_device *isdb2056;
	struct isdb2056_tsdev *tsdev;

	mutex_lock(&glock);

	isdb2056 = devs[dev_idx];
	if (!isdb2056) {
		pr_err("isdb2056_tsdev_open %d:%u: isdb2056 is NULL.\n", dev_idx, tsdev_id);
		mutex_unlock(&glock);
		return -EFAULT;
	}

	if (!atomic_read(&isdb2056->avail)) {
		// not available
		mutex_unlock(&glock);
		return -EIO;
	}

	tsdev = &isdb2056->tsdev[tsdev_id];

	mutex_lock(&tsdev->lock);
	mutex_unlock(&glock);

	if (tsdev->open) {
		// already used by another
		ret = -EALREADY;
		mutex_unlock(&tsdev->lock);
		goto fail;
	}

	mutex_lock(&isdb2056->lock);

	ref = isdb2056_ref(isdb2056);
	if (ref <= 1) {
		ret = -ECANCELED;
		mutex_unlock(&isdb2056->lock);
		mutex_unlock(&tsdev->lock);
		goto fail;
	}

	dev_dbg(isdb2056->dev, "isdb2056_tsdev_open %d:%u: ref count: %d\n", dev_idx, tsdev_id, ref);

	if (ref == 2) {
		int i;

		ret = isdb2056_set_power(isdb2056, true);
		if (ret) {
			dev_err(isdb2056->dev, "isdb2056_tsdev_open %d:%u: isdb2056_set_power(true) failed.\n", dev_idx, tsdev_id);
			goto fail_after_ref;
		}

		for (i = 0; i < TSDEV_NUM; i++) {
			struct isdb2056_tsdev *t = &isdb2056->tsdev[i];

			if (i == tsdev->id)
				continue;

			if (!t->open) {
				ret = r850_sleep(&t->r850);
				if (ret) {
					dev_err(isdb2056->dev, "isdb2056_tsdev_open %d:%u: rt850_sleep(%d) failed. (ret: %d)\n", dev_idx, tsdev_id, i, ret);
					break;
				}

				ret = tc90522_sleep_t(&t->tc90522_t, true);
				if (ret) {
					dev_err(isdb2056->dev, "isdb2056_tsdev_open %d:%u: tc90522_sleep_t(%d, true) failed. (ret: %d)\n", dev_idx, tsdev_id, i, ret);
					break;
				}

				if (!s_tuner_no_sleep) {
					ret = rt710_sleep(&t->rt710);
					if (ret) {
						dev_err(isdb2056->dev, "isdb2056_tsdev_open %d:%u: rt710_sleep(%d) failed. (ret: %d)\n", dev_idx, tsdev_id, i, ret);
						break;
					}
				}

				ret = tc90522_sleep_s(&t->tc90522_s, true);
				if (ret) {
					dev_err(isdb2056->dev, "isdb2056_tsdev_open %d:%u: tc90522_sleep_s(%d, true) failed. (ret: %d)\n", dev_idx, tsdev_id, i, ret);
					break;
				}
			}
		}

		if (i < TSDEV_NUM)
			goto fail_after_power;
	}

	ret = isdb2056_tsdev_init(tsdev);
	if (ret) {
		dev_err(isdb2056->dev, "isdb2056_tsdev_open %d:%u: isdb2056_tsdev_init() failed.\n", dev_idx, tsdev_id);
		goto fail_after_power;
	}

	tsdev->open = true;
	tsdev->isdb = ISDB_NONE;

	file->private_data = tsdev;

	mutex_unlock(&isdb2056->lock);
	mutex_unlock(&tsdev->lock);

	dev_dbg(isdb2056->dev, "isdb2056_tsdev_open %d:%u: ok\n", dev_idx, tsdev_id);

	return 0;

fail_after_power:
	if (ref == 2)
		isdb2056_set_power(isdb2056, false);
fail_after_ref:
	isdb2056_unref(isdb2056);
	mutex_unlock(&isdb2056->lock);
	mutex_unlock(&tsdev->lock);
fail:
	dev_dbg(isdb2056->dev, "isdb2056_tsdev_open %d:%u: failed. (ret: %d)\n", dev_idx, tsdev_id, ret);

	return ret;
}

static ssize_t isdb2056_tsdev_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	int ret = 0;
	struct isdb2056_device *isdb2056;
	struct isdb2056_tsdev *tsdev;
	size_t rd;

	tsdev = file->private_data;
	if (!tsdev) {
		pr_err("isdb2056_tsdev_read: tsdev is NULL.\n");
		return -EFAULT;
	}

	isdb2056 = container_of(tsdev, struct isdb2056_device, tsdev[tsdev->id]);

	rd = count;
	ret = ringbuffer_read_user(tsdev->ringbuf, buf, &rd);

	return (ret) ? (ret) : (rd);
}

static int isdb2056_tsdev_release(struct inode *inode, struct file *file)
{
	int avail, ref;
	struct isdb2056_device *isdb2056;
	struct isdb2056_tsdev *tsdev;

	tsdev = file->private_data;
	if (!tsdev) {
		pr_err("isdb2056_tsdev_release: tsdev is NULL.\n");
		return -EFAULT;
	}

	isdb2056 = container_of(tsdev, struct isdb2056_device, tsdev[tsdev->id]);
	avail = atomic_read(&isdb2056->avail);

	dev_dbg(isdb2056->dev, "isdb2056_tsdev_release %d:%u: avail: %d\n", isdb2056->dev_idx, tsdev->id, avail);

	mutex_lock(&tsdev->lock);

	isdb2056_tsdev_stop_streaming(tsdev, (avail) ? true : false);

	mutex_lock(&isdb2056->lock);

	if (avail)
		isdb2056_tsdev_term(tsdev);

	ref = isdb2056_unref(isdb2056);
	if (avail && ref <= 1)
		isdb2056_set_power(isdb2056, false);

	mutex_unlock(&isdb2056->lock);

	tsdev->open = false;

	mutex_unlock(&tsdev->lock);

	wake_up(&isdb2056->wait);

	dev_dbg(isdb2056->dev, "isdb2056_tsdev_release %d:%u: ok. ref count: %d\n", isdb2056->dev_idx, tsdev->id, ref);

	return 0;
}

static long isdb2056_tsdev_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long ret = -EIO;
	struct isdb2056_device *isdb2056;
	struct isdb2056_tsdev *tsdev;
	int avail;
	int dev_idx;
	unsigned int tsdev_id;
	unsigned long t;

	tsdev = file->private_data;
	if (!tsdev) {
		pr_err("isdb2056_tsdev_unlocked_ioctl: tsdev is NULL.\n");
		return -EFAULT;
	}

	isdb2056 = container_of(tsdev, struct isdb2056_device, tsdev[tsdev->id]);

	avail = atomic_read(&isdb2056->avail);

	dev_idx = isdb2056->dev_idx;
	tsdev_id = tsdev->id;

	mutex_lock(&tsdev->lock);

	switch (cmd) {
	case PTX_SET_CHANNEL:
	{
		struct ptx_freq freq;

		dev_dbg(isdb2056->dev, "isdb2056_tsdev_unlocked_ioctl %d:%u: PTX_SET_CHANNEL\n", dev_idx, tsdev_id);

		if (!avail) {
			ret = -EIO;
			break;
		}

		t = copy_from_user(&freq, (void *)arg, sizeof(freq));

		ret = isdb2056_tsdev_set_channel(tsdev, &freq);
		break;
	}

	case PTX_START_STREAMING:
		dev_dbg(isdb2056->dev, "isdb2056_tsdev_unlocked_ioctl %d:%u: PTX_START_STREAMING\n", dev_idx, tsdev_id);

		if (!avail) {
			ret = -EIO;
			break;
		}

		ret = isdb2056_tsdev_start_streaming(tsdev);
		break;

	case PTX_STOP_STREAMING:
		dev_dbg(isdb2056->dev, "isdb2056_tsdev_unlocked_ioctl %d:%u: PTX_STOP_STREAMING\n", dev_idx, tsdev_id);
		ret = isdb2056_tsdev_stop_streaming(tsdev, (avail) ? true : false);
		break;

	case PTX_GET_CNR:
	{
		int cn = 0;

		dev_dbg(isdb2056->dev, "isdb2056_tsdev_unlocked_ioctl %d:%u: PTX_GET_CNR\n", dev_idx, tsdev_id);

		if (!avail) {
			ret = -EIO;
			break;
		}

		ret = isdb2056_tsdev_get_cn(tsdev, (u32 *)&cn);
		if (!ret)
			t = copy_to_user((void *)arg, &cn, sizeof(cn));

		break;
	}

	case PTX_ENABLE_LNB_POWER:
		ret = -ENOSYS;
		break;

	case PTX_DISABLE_LNB_POWER:
		ret = -ENOSYS;
		break;

	default:
		dev_dbg(isdb2056->dev, "isdb2056_tsdev_unlocked_ioctl %d:%u: unknown ioctl 0x%08x\n", dev_idx, tsdev_id, cmd);
		ret = -ENOSYS;
		break;
	}

	mutex_unlock(&tsdev->lock);

	return ret;
}

static struct file_operations isdb2056_tsdev_fops = {
	.owner = THIS_MODULE,
	.open = isdb2056_tsdev_open,
	.read = isdb2056_tsdev_read,
	.release = isdb2056_tsdev_release,
	.unlocked_ioctl = isdb2056_tsdev_unlocked_ioctl
};

static void isdb2056_dev_workqueue_handler(struct work_struct *w)
{
	int ret = 0;
	struct isdb2056_device *isdb2056 = container_of(to_delayed_work(w), struct isdb2056_device, dev_work);
	struct it930x_bridge *it930x = &isdb2056->it930x;
	u8 val;

	ret = it930x_read_reg(it930x, 0x1222, &val);
	if (ret)
		return;

	if (atomic_read(&isdb2056->avail))
		queue_delayed_work(isdb2056->dev_wq, to_delayed_work(w), msecs_to_jiffies(10000));

	return;
}

static int isdb2056_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct device *dev = &intf->dev;
	int ret = 0, dev_idx = -1, i;
	struct usb_device *usbdev;
	struct isdb2056_device *isdb2056 = NULL;
	struct it930x_bridge *it930x;
	struct it930x_bus *bus;

	dev_dbg(dev, "isdb2056_probe: xfer_packets: %u\n", xfer_packets);

	mutex_lock(&glock);

	for (i = 0; i < MAX_DEVICE; i++) {
		if (!devs[i]) {
			dev_idx = i;
			break;
		}
	}

	dev_dbg(dev, "isdb2056_probe: dev_idx: %d\n", dev_idx);

	if (dev_idx == -1) {
		dev_err(dev, "Unused device index was not found.\n");
		ret = -ECANCELED;
		goto fail_before_base;
	}

	usbdev = interface_to_usbdev(intf);

	if (usbdev->speed < USB_SPEED_HIGH)
		dev_warn(dev, "This device is operating as USB 1.1 or less.\n");

	isdb2056 = kzalloc(sizeof(*isdb2056), GFP_KERNEL);
	if (!isdb2056) {
		dev_err(dev, "isdb2056_probe: kzalloc(sizeof(*isdb2056), GFP_KERNEL) failed.\n");
		ret = -ENOMEM;
		goto fail_before_base;
	}

	isdb2056->stream_context = (struct isdb2056_stream_context *)kzalloc(sizeof(*isdb2056->stream_context), GFP_KERNEL);
	if (!isdb2056->stream_context) {
		dev_err(dev, "isdb2056_probe: kzalloc(sizeof(*isdb2056->stream_context), GFP_KERNEL) failed.\n");
		ret = -ENOMEM;
		goto fail_before_base;
	}

	isdb2056->dev = dev;
	isdb2056->dev_idx = dev_idx;
	isdb2056->serial_number = 0;
	isdb2056->dev_id = 0;

	if (strlen(usbdev->serial) == 15) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,39)
		if (kstrtoull(usbdev->serial, 16, &isdb2056->serial_number))
			dev_err(dev, "isdb2056_probe: kstrtoull() failed.\n");
#else
		if (strict_strtoull(usbdev->serial, 16, &isdb2056->serial_number))
			dev_err(dev, "isdb2056_probe: strict_strtoull() failed.\n");
#endif
		else {
			isdb2056->dev_id = do_div(isdb2056->serial_number, 16);

			dev_dbg(dev, "isdb2056_probe: serial_number: %014llx\n", isdb2056->serial_number);
			dev_dbg(dev, "isdb2056_probe: dev_id: %u\n", isdb2056->dev_id);

			if (isdb2056->dev_id != 1 && isdb2056->dev_id != 2)
				dev_warn(dev, "isdb2056_probe: Unexpected device id: %u\n", isdb2056->dev_id);
		}
	} else
		dev_warn(dev, "isdb2056_probe: Invalid serial number length.\n");

	// Initialize isdb2056 structure

	ret = isdb2056_init(isdb2056);
	if (ret)
		goto fail_before_base;

	it930x = &isdb2056->it930x;
	bus = &it930x->bus;

	// Initialize bus operator

	bus->dev = dev;
	bus->type = IT930X_BUS_USB;
	bus->usb.dev = usbdev;
	bus->usb.ctrl_timeout = 3000;

	ret = it930x_bus_init(bus);
	if (ret)
		goto fail_before_bus;

	// Initialize bridge operator

	it930x->dev = dev;
	it930x->config.xfer_size = 188 * xfer_packets;
	it930x->config.i2c_speed = 0x07;

	ret = it930x_init(it930x);
	if (ret)
		goto fail_before_bridge;

	// Load config from eeprom

	ret = isdb2056_load_config(isdb2056);
	if (ret)
		goto fail;

	// Initialize IT930x bridge

	ret = it930x_load_firmware(it930x, FIRMWARE_FILENAME);
	if (ret)
		goto fail;

	ret = it930x_init_device(it930x);
	if (ret)
		goto fail;

	// GPIO configurations

	ret = it930x_set_gpio_mode(it930x, 3, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail;

	ret = it930x_write_gpio(it930x, 3, true);
	if (ret)
		goto fail;

	ret = it930x_set_gpio_mode(it930x, 2, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail;

	ret = it930x_write_gpio(it930x, 2, false);
	if (ret)
		goto fail;

#if 0
	ret = it930x_set_gpio_mode(it930x, 11, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail;

	// LNB power supply: off
	ret = it930x_write_gpio(it930x, 11, false);
	if (ret)
		goto fail;
#endif

	// cdev

	cdev_init(&isdb2056->cdev, &isdb2056_tsdev_fops);
	isdb2056->cdev.owner = THIS_MODULE;

	ret = cdev_add(&isdb2056->cdev, MKDEV(MAJOR(isdb2056_dev_first), MINOR(isdb2056_dev_first) + (dev_idx * TSDEV_NUM)), TSDEV_NUM);
	if (ret < 0) {
		dev_err(dev, "Couldn't add cdev to the system.\n");
		goto fail;
	}

	// create /dev/isdb2056video*
	for (i = 0; i < TSDEV_NUM; i++) {
		dev_info(dev, "tsdev %i: isdb2056video%u\n", i, (MINOR(isdb2056_dev_first) + (dev_idx * TSDEV_NUM) + i));
		device_create(isdb2056_class, &intf->dev, MKDEV(MAJOR(isdb2056_dev_first), (MINOR(isdb2056_dev_first) + (dev_idx * TSDEV_NUM) + i)), NULL, "isdb2056video%u", (MINOR(isdb2056_dev_first) + (dev_idx * TSDEV_NUM) + i));
	}

	devs[dev_idx] = isdb2056;

	mutex_unlock(&glock);

	get_device(dev);

	usb_set_intfdata(intf, isdb2056);

	INIT_DELAYED_WORK(&isdb2056->dev_work, isdb2056_dev_workqueue_handler);

	isdb2056->dev_wq = create_singlethread_workqueue("isdb2056_dev_workqueue");
	if (isdb2056->dev_wq)
		queue_delayed_work(isdb2056->dev_wq, &isdb2056->dev_work, msecs_to_jiffies(10000));

	return 0;

fail:
	it930x_term(it930x);
fail_before_bridge:
	it930x_bus_term(bus);
fail_before_bus:
	isdb2056_term(isdb2056);
fail_before_base:
	if (isdb2056) {
		if (isdb2056->stream_context)
			kfree(isdb2056->stream_context);

		kfree(isdb2056);
	}

	mutex_unlock(&glock);

	return ret;
}

static void isdb2056_disconnect(struct usb_interface *intf)
{
	int i, ref;
	struct isdb2056_device *isdb2056;

	isdb2056 = usb_get_intfdata(intf);
	if (!isdb2056)
		return;

	dev_dbg(isdb2056->dev, "isdb2056_disconnect: dev_idx: %d\n", isdb2056->dev_idx);

	usb_set_intfdata(intf, NULL);

	atomic_set(&isdb2056->avail, 0);
	mutex_lock(&isdb2056->lock);

	mutex_lock(&glock);

	devs[isdb2056->dev_idx] = NULL;

	// delete /dev/isdb2056video*
	for (i = 0; i < TSDEV_NUM; i++)
		device_destroy(isdb2056_class, MKDEV(MAJOR(isdb2056_dev_first), (MINOR(isdb2056_dev_first) + (isdb2056->dev_idx * TSDEV_NUM) + i)));

	mutex_unlock(&glock);

	cdev_del(&isdb2056->cdev);

	ref = isdb2056_unref(isdb2056);

	mutex_unlock(&isdb2056->lock);

	for (i = 0; i < TSDEV_NUM; i++) {
		struct isdb2056_tsdev *tsdev = &isdb2056->tsdev[i];

		mutex_lock(&tsdev->lock);
		isdb2056_tsdev_stop_streaming(tsdev, false);
		mutex_unlock(&tsdev->lock);
	}

	while (ref) {
		wait_event(isdb2056->wait, (ref != atomic_read(&isdb2056->ref)));
		ref = atomic_read(&isdb2056->ref);
	}

	if (isdb2056->dev_wq) {
		cancel_delayed_work_sync(&isdb2056->dev_work);
		flush_workqueue(isdb2056->dev_wq);
		destroy_workqueue(isdb2056->dev_wq);
		isdb2056->dev_wq = NULL;
	}

	put_device(isdb2056->dev);

	// uninitialize
	it930x_term(&isdb2056->it930x);
	it930x_bus_term(&isdb2056->it930x.bus);
	isdb2056_term(isdb2056);
	kfree(isdb2056->stream_context);
	kfree(isdb2056);

	return;
}

static int isdb2056_suspend(struct usb_interface *intf, pm_message_t message)
{
	return -ENOSYS;
}

static int isdb2056_resume(struct usb_interface *intf)
{
	return 0;
}

static struct usb_driver isdb2056_usb_driver = {
	.name = "isdb2056_drv",
	.probe = isdb2056_probe,
	.disconnect = isdb2056_disconnect,
	.suspend = isdb2056_suspend,
	.resume = isdb2056_resume,
	.id_table = isdb2056_usb_ids
};

int isdb2056_register(void)
{
	int ret = 0, i;

	for (i = 0; i < MAX_DEVICE; i++)
		devs[i] = NULL;

	ret = alloc_chrdev_region(&isdb2056_dev_first, 0, MAX_TSDEV, DEVICE_NAME);
	if (ret < 0) {
		pr_err("isdb2056_module_init: alloc_chrdev_region() failed.\n");
		return ret;
	}

	isdb2056_class = class_create(THIS_MODULE, "isdb2056");
	if (IS_ERR(isdb2056_class)) {
		pr_err("isdb2056_module_init: class_create() failed.\n");
		unregister_chrdev_region(isdb2056_dev_first, MAX_TSDEV);
		return PTR_ERR(isdb2056_class);
	}

	ret = usb_register(&isdb2056_usb_driver);
	if (ret) {
		pr_err("isdb2056_module_init: usb_register() failed.\n");
		class_destroy(isdb2056_class);
		unregister_chrdev_region(isdb2056_dev_first, MAX_TSDEV);
	}

	return ret;
}

void isdb2056_unregister(void)
{
	pr_debug("isdb2056_unregister\n");

	usb_deregister(&isdb2056_usb_driver);
	class_destroy(isdb2056_class);
	unregister_chrdev_region(isdb2056_dev_first, MAX_TSDEV);

	pr_debug("isdb2056_unregister: quit\n");
}
