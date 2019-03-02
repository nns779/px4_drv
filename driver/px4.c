// px4.c

#include "print_format.h"

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

#include "px4.h"
#include "revision.h"
#include "ptx_ioctl.h"
#include "i2c_comm.h"
#include "it930x-config.h"
#include "it930x-bus.h"
#include "it930x.h"
#include "tc90522.h"
#include "r850_lite.h"
#include "r850_channel.h"
#include "rt710.h"
#include "ringbuffer.h"

#if !defined(FIRMWARE_FILENAME)
#define FIRMWARE_FILENAME	"it930x-firmware.bin"
#endif

#if !defined(MAX_DEVICE) || !MAX_DEVICE
#undef MAX_DEVICE
#define MAX_DEVICE	16
#endif
#define TSDEV_NUM	4
#define MAX_TSDEV	(MAX_DEVICE * TSDEV_NUM)
#define DEVICE_NAME	"px4"

#define PID_PX_W3U4	0x083f
#define PID_PX_W3PE4	0x023f
#define PID_PX_Q3U4	0x084a
#define PID_PX_Q3PE4	0x024a

#define ISDB_T	1
#define ISDB_S	2

#define TS_SYNC_COUNT	4
#define TS_SYNC_SIZE	(188 * TS_SYNC_COUNT)

struct px4_tsdev {
	struct mutex lock;
	unsigned int id;
	int isdb;				// ISDB_T or ISDB_S
	bool init;
	bool open;
	bool lnb_power;
	struct tc90522_demod tc90522;
	union {
		struct r850_tuner r850;		// for ISDB-T
		struct rt710_tuner rt710;	// for ISDB-S
	} t;
	atomic_t streaming;			// 0: not streaming, !0: streaming
	struct ringbuffer *ringbuf;
};

struct px4_stream_context {
	struct ringbuffer *ringbuf[TSDEV_NUM];
	u8 remain_buf[TS_SYNC_SIZE];
	size_t remain_len;
};

struct px4_device {
	struct device *dev;
	atomic_t ref;				// reference counter
	atomic_t avail;				// availability flag
	wait_queue_head_t wait;
	struct mutex lock;
	int dev_idx;
	unsigned int dev_id;			// 1 or 2
	struct it930x_bridge it930x;
	struct cdev cdev;
	unsigned int lnb_power_count;
	unsigned int streaming_count;
	struct px4_tsdev tsdev[TSDEV_NUM];
	struct px4_stream_context *stream_context;
};

MODULE_VERSION(PX4_DRIVER_VERSION);
MODULE_AUTHOR("nns779");
MODULE_DESCRIPTION("PLEX PX-W3U4/W3PE4/Q3PE4 Unofficial Linux driver");
MODULE_LICENSE("GPL v2");

MODULE_FIRMWARE(FIRMWARE_FILENAME);

static DEFINE_MUTEX(glock);
static struct class *px4_class = NULL;
static dev_t px4_dev_first;
static struct px4_device *devs[MAX_DEVICE];
static bool devs_reserve[MAX_DEVICE];
static unsigned int xfer_packets = 816;
static unsigned int max_urbs = 6;
static unsigned int tsdev_max_packets = 2048;
static bool no_dma = false;
static bool s_agc_negative_mode = false;
static bool s_vga_atten = false;
static bool s_low_fine_gain = false;

module_param(xfer_packets, uint, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(xfer_packets, "Number of transfer packets from the device. (default: 816)");

module_param(max_urbs, uint, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(max_urbs, "Maximum number of URBs. (default: 6)");

module_param(tsdev_max_packets, uint, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(tsdev_max_packets, "Maximum number of packets buffering in tsdev. (default: 2048)");

module_param(no_dma, bool, S_IRUSR | S_IWUSR);

module_param(s_agc_negative_mode, bool, S_IRUSR | S_IWUSR);
module_param(s_vga_atten, bool, S_IRUSR | S_IWUSR);
module_param(s_low_fine_gain, bool, S_IRUSR | S_IWUSR);

static const struct usb_device_id px4_usb_ids[] = {
	{ USB_DEVICE(0x0511, PID_PX_W3U4) },
	{ USB_DEVICE(0x0511, PID_PX_W3PE4) },
	{ USB_DEVICE(0x0511, PID_PX_Q3U4) },
	{ USB_DEVICE(0x0511, PID_PX_Q3PE4) },
	{ 0 },
};

MODULE_DEVICE_TABLE(usb, px4_usb_ids);

static int px4_init(struct px4_device *px4)
{
	int ret = 0;
	int i;

	atomic_set(&px4->ref, 1);
	atomic_set(&px4->avail, 1);
	init_waitqueue_head(&px4->wait);
	mutex_init(&px4->lock);
	px4->lnb_power_count = 0;
	px4->streaming_count = 0;

	for (i = 0; i < TSDEV_NUM; i++) {
		struct px4_tsdev *tsdev = &px4->tsdev[i];

		mutex_init(&tsdev->lock);
		tsdev->id = i;
		tsdev->init = false;
		tsdev->open = false;
		tsdev->lnb_power = false;
		atomic_set(&tsdev->streaming, 0);

		ret = ringbuffer_create(&tsdev->ringbuf);
		if (ret)
			break;

		ret = ringbuffer_alloc(tsdev->ringbuf, 188 * tsdev_max_packets);
		if (ret)
			break;

		px4->stream_context->ringbuf[i] = tsdev->ringbuf;
	}

	return ret;
}

static int px4_term(struct px4_device *px4)
{
	int i;

	for (i = 0; i < TSDEV_NUM; i++) {
		struct px4_tsdev *tsdev = &px4->tsdev[i];

		ringbuffer_destroy(tsdev->ringbuf);
	}

	return 0;
}

static int px4_ref(struct px4_device *px4)
{
	return atomic_add_return(1, &px4->ref);
}

static int px4_unref(struct px4_device *px4)
{
	return atomic_sub_return(1, &px4->ref);
}

static int px4_load_config(struct px4_device *px4)
{
	int ret = 0;
	struct device *dev = px4->dev;
	struct it930x_bridge *it930x = &px4->it930x;
	u8 tmp;
	int i;

	ret = it930x_read_reg(it930x, 0x4979, &tmp);
	if (ret) {
		dev_err(dev, "px4_load_config: it930x_read_reg(0x4979) failed.\n");
		return ret;
	} else if (!tmp) {
		dev_warn(dev, "EEPROM is invalid.\n");
		return ret;
	}

	px4->tsdev[0].isdb = ISDB_S;
	px4->tsdev[1].isdb = ISDB_S;
	px4->tsdev[2].isdb = ISDB_T;
	px4->tsdev[3].isdb = ISDB_T;

	it930x->input[0].i2c_addr = 0x22;
	it930x->input[1].i2c_addr = 0x26;
	it930x->input[2].i2c_addr = 0x20;
	it930x->input[3].i2c_addr = 0x24;

	for (i = 0; i < TSDEV_NUM; i++) {
		struct it930x_stream_input *input = &it930x->input[i];
		struct px4_tsdev *tsdev = &px4->tsdev[i];

		input->enable = true;
		input->is_parallel = false;
		input->port_number = i + 1;
		input->slave_number = i;
		input->i2c_bus = 2;
		input->packet_len = 188;
		input->sync_byte = ((i + 1) << 4) | 0x07;	// 0x17 0x27 0x37 0x47

		tsdev->tc90522.dev = dev;
		tsdev->tc90522.i2c = &it930x->i2c_master[0];
		tsdev->tc90522.i2c_addr = input->i2c_addr;
		tsdev->tc90522.is_secondary = (i % 2) ? true : false;

		switch (tsdev->isdb) {
		case ISDB_S:
			tsdev->t.rt710.dev = dev;
			tsdev->t.rt710.i2c = &tsdev->tc90522.i2c_master;
			tsdev->t.rt710.i2c_addr = 0x7a;
			break;

		case ISDB_T:
			tsdev->t.r850.dev = dev;
			tsdev->t.r850.i2c = &tsdev->tc90522.i2c_master;
			tsdev->t.r850.i2c_addr = 0x7c;
			break;
		}
	}

	it930x->input[4].enable = false;

	return 0;
}

static int px4_set_power(struct px4_device *px4, bool on)
{
	int ret = 0, i;
	struct it930x_bridge *it930x = &px4->it930x;

	if (on) {
		ret = it930x_set_gpio(it930x, 7, false);
		if (ret)
			return ret;

		msleep(100);

		ret = it930x_set_gpio(it930x, 2, false);
		if (ret)
			return ret;

		msleep(10);

		ret = it930x_set_gpio(it930x, 2, true);
		if (ret)
			return ret;

		msleep(10);

		for (i = 0; i < TSDEV_NUM; i++) {
			struct px4_tsdev *t = &px4->tsdev[i];

			ret = tc90522_init(&t->tc90522);
			if (ret) {
				dev_err(px4->dev, "px4_set_power: tc90522_init(%d) failed. (ret: %d)\n", i, ret);
				break;
			}

			switch (t->isdb) {
			case ISDB_S:
				ret = rt710_init(&t->t.rt710);
				if (ret)
					dev_err(px4->dev, "px4_set_power: rt710_init(%d) failed. (ret: %d)\n", i, ret);
				break;

			case ISDB_T:
				ret = r850_init(&t->t.r850);
				if (ret)
					dev_err(px4->dev, "px4_set_power: r850_init(%d) failed. (ret: %d)\n", i, ret);
				break;

			default:
				break;
			}
		}
	} else {
		for (i = 0; i < TSDEV_NUM; i++) {
			struct px4_tsdev *t = &px4->tsdev[i];

			switch (t->isdb) {
			case ISDB_S:
				rt710_term(&t->t.rt710);
				break;

			case ISDB_T:
				r850_term(&t->t.r850);
				break;

			default:
				break;
			}

			tc90522_term(&t->tc90522);
		}
		it930x_set_gpio(it930x, 7, true);
		msleep(50);
		it930x_set_gpio(it930x, 2, false);
	}

	return ret;
}

static bool px4_ts_sync(u8 **buf, u32 *len, bool *sync_remain)
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

static void px4_ts_write(struct ringbuffer **ringbuf, u8 **buf, u32 *len)
{
	u8 *p = *buf;
	u32 remain = *len;

	while (remain >= 188 && ((p[0] & 0x8f) == 0x07)) {
		u8 id = (p[0] & 0x70) >> 4;

		if (id && id < 5) {
			p[0] = 0x47;
			ringbuffer_write_atomic(ringbuf[id - 1], p, 188);
		} else {
			pr_debug("px4_ts_write: unknown id %d\n", id);
		}

		p += 188;
		remain -= 188;
	}

	*buf = p;
	*len = remain;

	return;
}

static int px4_on_stream(void *context, void *buf, u32 len)
{
	struct px4_stream_context *stream_context = context;
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

			if (px4_ts_sync(&context_remain_buf, &context_remain_len, &sync_remain)) {
				px4_ts_write(stream_context->ringbuf, &context_remain_buf, &context_remain_len);

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
		if (!px4_ts_sync(&p, &remain, &sync_remain))
			break;

		px4_ts_write(stream_context->ringbuf, &p, &remain);
	}

	if (sync_remain) {
		memcpy(stream_context->remain_buf, p, remain);
		stream_context->remain_len = remain;
	}

	return 0;
}

struct tc90522_regbuf tc_init_s[] = {
	{ 0x15, NULL, { 0x00 } },
	{ 0x1d, NULL, { 0x00 } },
	{ 0x04, NULL, { 0x02 } }
};

struct tc90522_regbuf tc_init_t[] = {
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

// This function must be called after power on.
static int px4_tsdev_init(struct px4_tsdev *tsdev)
{
	int ret = 0;
	struct px4_device *px4 = container_of(tsdev, struct px4_device, tsdev[tsdev->id]);
	struct tc90522_demod *tc90522 = &tsdev->tc90522;

	if (tsdev->init)
		// already initialized
		return 0;

	switch (tsdev->isdb) {
	case ISDB_S:
	{
		ret = tc90522_write_regs(tc90522, tc_init_s, ARRAY_SIZE(tc_init_s));
		if (ret) {
			dev_err(px4->dev, "px4_tsdev_init %d:%u: tc90522_write_regs(tc_init_s) failed. (ret: %d)\n", px4->dev_idx, tsdev->id, ret);
			break;
		}

		// disable ts pins
		ret = tc90522_enable_ts_pins_s(tc90522, false);
		if (ret) {
			dev_err(px4->dev, "px4_tsdev_init %d:%u: tc90522_enable_ts_pins_s(false) failed. (ret: %d)\n", px4->dev_idx, tsdev->id, ret);
			break;
		}

		// wake up
		ret = tc90522_sleep_s(tc90522, false);
		if (ret) {
			dev_err(px4->dev, "px4_tsdev_init %d:%u: tc90522_sleep_s(false) failed. (ret: %d)\n", px4->dev_idx, tsdev->id, ret);
			break;
		}

		break;
	}

	case ISDB_T:
	{
		ret = tc90522_write_regs(tc90522, tc_init_t, ARRAY_SIZE(tc_init_t));
		if (ret) {
			dev_err(px4->dev, "px4_tsdev_init %d:%u: tc90522_write_regs(tc_init_t) failed. (ret: %d)\n", px4->dev_idx, tsdev->id, ret);
			break;
		}

		// disable ts pins
		ret = tc90522_enable_ts_pins_t(tc90522, false);
		if (ret) {
			dev_err(px4->dev, "px4_tsdev_init %d:%u: tc90522_enable_ts_pins_s(false) failed. (ret: %d)\n", px4->dev_idx, tsdev->id, ret);
			break;
		}

		// wake up
		ret = tc90522_sleep_t(tc90522, false);
		if (ret) {
			dev_err(px4->dev, "px4_tsdev_init %d:%u: tc90522_sleep_s(false) failed. (ret: %d)\n", px4->dev_idx, tsdev->id, ret);
			break;
		}

#if 0
		ret = r850_wakeup(&tsdev->t.r850);
		if (ret) {
			dev_err(px4->dev, "px4_tsdev_init %d:%u: r850_wakeup() failed. (ret: %d)\n", px4->dev_idx, tsdev->id, ret);
			break;
		}
#endif

		if (tsdev->id == 3) {
			struct px4_tsdev *tsdev_t0 = &container_of(tsdev, struct px4_device, tsdev[tsdev->id])->tsdev[2];

			if (!tsdev_t0->init) {
				u8 regs[2][R850_NUM_REGS - 0x08];

				dev_dbg(px4->dev, "px4_tsdev_init %d:%u: init t0\n", px4->dev_idx, tsdev->id);

				ret = px4_tsdev_init(tsdev_t0);
				if (ret) {
					dev_err(px4->dev, "px4_tsdev_init %d:%u(*): px4_tsdev_init() 2 failed.\n", px4->dev_idx, 2);
					break;
				}

				r850_channel_get_regs(63, regs);

				ret = r850_write_config_regs(&tsdev_t0->t.r850, regs[0]);
				if (ret) {
					dev_err(px4->dev, "px4_tsdev_init %d:%u(*): r850_write_config_regs() 2 failed.\n", px4->dev_idx, 2);
					break;
				}
			}
		}
		break;
	}

	default:
		ret = -EIO;
		break;
	}

	if (!ret)
		tsdev->init = true;

	return ret;
}

static void px4_tsdev_term(struct px4_tsdev *tsdev)
{
	struct px4_device *px4 = container_of(tsdev, struct px4_device, tsdev[tsdev->id]);
	struct tc90522_demod *tc90522 = &tsdev->tc90522;

	if (!tsdev->init)
		return;

	switch (tsdev->isdb) {
	case ISDB_S:
		rt710_sleep(&tsdev->t.rt710);
		tc90522_sleep_s(tc90522, true);
		break;

	case ISDB_T:
#if 0
		r850_sleep(&tsdev->t.r850);
		tc90522_sleep_t(tc90522, true);
#endif

		if (tsdev->id == 3) {
			struct px4_tsdev *tsdev_t0 = &container_of(tsdev, struct px4_device, tsdev[tsdev->id])->tsdev[2];

			if (!tsdev_t0->open && tsdev_t0->init) {
				dev_dbg(px4->dev, "px4_tsdev_term %d:%u: term t0\n", px4->dev_idx, tsdev->id);
				px4_tsdev_term(tsdev_t0);
			}
		}
		break;

	default:
		break;
	}

	tsdev->init = false;

	return;
}

static int px4_tsdev_set_channel(struct px4_tsdev *tsdev, struct ptx_freq *freq)
{
	struct px4_device *px4 = container_of(tsdev, struct px4_device, tsdev[tsdev->id]);
	int ret = 0, dev_idx = px4->dev_idx;
	unsigned int tsdev_id = tsdev->id;
	struct tc90522_demod *tc90522 = &tsdev->tc90522;
	struct tc90522_regbuf regbuf_tc[3];
	u32 real_freq;

	dev_dbg(px4->dev, "px4_tsdev_set_channel %d:%u: freq_no: %d, slot: %d\n", dev_idx, tsdev_id, freq->freq_no, freq->slot);

	switch (tsdev->isdb) {
	case ISDB_S:
	{
		int i;
		struct rt710_tuner *rt710 = &tsdev->t.rt710;
		bool tuner_locked, demod_locked;
		s32 ss = 0;
		u16 tsid, tsid2;

		if (freq->freq_no < 0) {
			ret = -EINVAL;
			break;
		} else if (freq->freq_no < 12) {
			if (freq->slot >= 8) {
				ret = -EINVAL;
				break;
			}
			real_freq = 1049480 + (38360 * freq->freq_no);
		} else if (freq->freq_no < 24) {
			real_freq = 1613000 + (40000 * (freq->freq_no - 12));
		} else {
			ret = -EINVAL;
			break;
		}

		mutex_lock(&px4->lock);

		rt710->config.agc_mode = (s_agc_negative_mode) ? RT710_AGC_NEGATIVE : RT710_AGC_POSITIVE;
		rt710->config.vga_atten_mode = (s_vga_atten) ? RT710_VGA_ATTEN_ON : RT710_VGA_ATTEN_OFF;
		rt710->config.fine_gain = (s_low_fine_gain) ? RT710_FINE_GAIN_LOW : RT710_FINE_GAIN_HIGH;

		dev_dbg(px4->dev, "px4_tsdev_set_channel %d:%u: rt710: agc_mode: %d, vga_atten_mode: %d, fine_gain: %d\n", dev_idx, tsdev_id, rt710->config.agc_mode, rt710->config.vga_atten_mode, rt710->config.fine_gain);

		// set frequency

		ret = tc90522_set_agc_s(tc90522, false);
		if (ret) {
			mutex_unlock(&px4->lock);
			dev_err(px4->dev, "px4_tsdev_set_channel %d:%u: tc90522_set_agc_s(false) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}
		tc90522_regbuf_set_val(&regbuf_tc[0], 0x8e, 0x06/*0x02*/);
		tc90522_regbuf_set_val(&regbuf_tc[1], 0xa3, 0xf7);
		ret = tc90522_write_regs(tc90522, regbuf_tc, 2);
		if (ret) {
			mutex_unlock(&px4->lock);
			dev_err(px4->dev, "px4_tsdev_set_channel %d:%u: tc90522_write_regs() failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = rt710_set_params(rt710, real_freq, 28860, 4);
		mutex_unlock(&px4->lock);
		if (ret) {
			dev_err(px4->dev, "px4_tsdev_set_channel %d:%u: rt710_set_params(%u, 28860, 4) failed. (ret: %d)\n", dev_idx, tsdev_id, real_freq, ret);
			break;
		}

		i = 50;
		while (i--) {
			mutex_lock(&px4->lock);
			ret = rt710_is_pll_locked(rt710, &tuner_locked);
			mutex_unlock(&px4->lock);
			if (!ret && tuner_locked)
				break;

			msleep(10);
		}

		if (ret) {
			dev_err(px4->dev, "px4_tsdev_set_channel %d:%u: rt710_is_pll_locked() failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		mutex_lock(&px4->lock);
		rt710_get_rf_signal_strength(rt710, &ss);
		mutex_unlock(&px4->lock);

		dev_dbg(px4->dev, "px4_tsdev_set_channel %d:%u: PLL is locked. count: %d, signal strength: %ddBm\n", dev_idx, tsdev_id, i, ss);

		if (!tuner_locked) {
			// PLL error
			ret = -EIO;
			break;
		}

		mutex_lock(&px4->lock);
		ret = tc90522_set_agc_s(tc90522, true);
		mutex_unlock(&px4->lock);
		if (ret) {
			dev_err(px4->dev, "px4_tsdev_set_channel %d:%u: tc90522_set_agc_s(true) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		// check lock

		i = 300;
		while (i--) {
			mutex_lock(&px4->lock);
			ret = tc90522_is_signal_locked_s(tc90522, &demod_locked);
			mutex_unlock(&px4->lock);
			if (!ret && demod_locked)
				break;

			msleep(10);
		}

		if (ret) {
			dev_dbg(px4->dev, "px4_tsdev_set_channel %d:%u: tc90522_is_signal_locked_s() failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		dev_dbg(px4->dev, "px4_tsdev_set_channel %d:%u: tc90522_is_signal_locked_s() locked: %d, count: %d\n", dev_idx, tsdev_id, demod_locked, i);

		if (!demod_locked) {
			ret = -EAGAIN;
			break;
		}

		// set slot

		i = 100;
		while (i--) {
			mutex_lock(&px4->lock);
			ret = tc90522_tmcc_get_tsid_s(tc90522, freq->slot, &tsid);
			mutex_unlock(&px4->lock);
			if ((!ret && tsid) || ret == -EINVAL)
				break;

			msleep(10);
		}

		if (ret) {
			dev_err(px4->dev, "px4_tsdev_set_channel %d:%u: tc90522_tmcc_get_tsid_s() failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		dev_dbg(px4->dev, "px4_tsdev_set_channel %d:%u: tc90522_tmcc_get_tsid_s() tsid: 0x%04x, count: %d\n", dev_idx, tsdev_id, tsid, i);

		if (!tsid) {
			ret = -EAGAIN;
			break;
		}

		mutex_lock(&px4->lock);
		ret = tc90522_set_tsid_s(tc90522, tsid);
		mutex_unlock(&px4->lock);
		if (ret) {
			dev_err(px4->dev, "px4_tsdev_set_channel %d:%u: tc90522_set_tsid_s(0x%x) failed. (ret: %d)\n", dev_idx, tsdev_id, tsid, ret);
			break;
		}

		// check slot

		i = 100;
		while(i--) {
			mutex_lock(&px4->lock);
			ret = tc90522_get_tsid_s(tc90522, &tsid2);
			mutex_unlock(&px4->lock);
			if (!ret && tsid2 == tsid)
				break;

			msleep(10);
		}

		dev_dbg(px4->dev, "px4_tsdev_set_channel %d:%u: tc90522_get_tsid_s() tsid2: 0x%04x, count: %d\n", dev_idx, tsdev_id, tsid2, i);

		if (tsid2 != tsid) {
			ret = -EAGAIN;
			break;
		}

		break;
	}

	case ISDB_T:
	{
		int i;
		bool tuner_locked, demod_locked;
		u8 regs[2][R850_NUM_REGS - 0x08];

		if ((freq->freq_no >= 3 && freq->freq_no <= 12) || (freq->freq_no >= 22 && freq->freq_no <= 62)) {
			// CATV C13-C22ch, C23-63ch
#if 0
			real_freq = 93143 + freq->freq_no * 6000 + freq->slot/* addfreq */;

			if (freq->freq_no == 12)
				real_freq += 2000;
#else
			ret = r850_channel_get_regs(freq->freq_no, regs);
			if (ret)
				break;
#endif
		} else if (freq->freq_no >= 63 && freq->freq_no <= 102) {
			// UHF 13-52ch
#if 0
			real_freq = 95143 + freq->freq_no * 6000 + freq->slot/* addfreq */;
#else
			ret = r850_channel_get_regs(freq->freq_no, regs);
			if (ret)
				break;
#endif
		} else {
			// Unknown channel
			ret = -EINVAL;
			break;
		}

		// set frequency

		mutex_lock(&px4->lock);
		tc90522_regbuf_set_val(&regbuf_tc[0], 0x47, 0x30);
		ret = tc90522_write_regs(tc90522, regbuf_tc, 1);
		if (ret) {
			mutex_unlock(&px4->lock);
			dev_err(px4->dev, "px4_tsdev_set_channel %d:%u: tc90522_write_regs() 1 failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = tc90522_set_agc_t(tc90522, false);
		if (ret) {
			mutex_unlock(&px4->lock);
			dev_err(px4->dev, "px4_tsdev_set_channel %d:%u: tc90522_set_agc_t(false) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		tc90522_regbuf_set_val(&regbuf_tc[0], 0x76, 0x0c);
		ret = tc90522_write_regs(tc90522, regbuf_tc, 1);
		if (ret) {
			mutex_unlock(&px4->lock);
			dev_err(px4->dev, "px4_tsdev_set_channel %d:%u: tc90522_write_regs() 2 failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		ret = r850_write_config_regs(&tsdev->t.r850, regs[0]);
		mutex_unlock(&px4->lock);
		if (ret) {
			dev_err(px4->dev, "px4_tsdev_set_channel %d:%u: r850_write_config_regs() 1 failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		msleep(40);

		mutex_lock(&px4->lock);
		ret = r850_write_config_regs(&tsdev->t.r850, regs[1]);
		mutex_unlock(&px4->lock);
		if (ret) {
			dev_err(px4->dev, "px4_tsdev_set_channel %d:%u: r850_write_config_regs() 2 failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		i = 50;
		while (i--) {
			mutex_lock(&px4->lock);
			ret = r850_is_pll_locked(&tsdev->t.r850, &tuner_locked);
			mutex_unlock(&px4->lock);
			if (!ret && tuner_locked)
				break;

			msleep(10);
		}

		if (ret) {
			dev_err(px4->dev, "px4_tsdev_set_channel %d:%u: r850_is_pll_locked() failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		dev_dbg(px4->dev, "px4_tsdev_set_channel %d:%u: PLL is locked. count: %d\n", dev_idx, tsdev_id, i);

		if (!tuner_locked) {
			// PLL error
			ret = -EAGAIN;
			break;
		}

		mutex_lock(&px4->lock);
		ret = tc90522_set_agc_t(tc90522, true);
		if (ret) {
			mutex_unlock(&px4->lock);
			dev_err(px4->dev, "px4_tsdev_set_channel %d:%u: tc90522_set_agc_t(true) failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		tc90522_regbuf_set_val(&regbuf_tc[0], 0x71, 0x21);
		tc90522_regbuf_set_val(&regbuf_tc[1], 0x72, 0x25);
		tc90522_regbuf_set_val(&regbuf_tc[2], 0x75, 0x08);
		ret = tc90522_write_regs(tc90522, regbuf_tc, 3);
		mutex_unlock(&px4->lock);
		if (ret) {
			dev_err(px4->dev, "px4_tsdev_set_channel %d:%u: tc90522_write_regs() 3 failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		// check lock

		i = 300;
		while (i--) {
			mutex_lock(&px4->lock);
			ret = tc90522_is_signal_locked_t(tc90522, &demod_locked);
			mutex_unlock(&px4->lock);
			if (!ret && demod_locked)
				break;

			msleep(10);
		}

		if (ret) {
			dev_err(px4->dev, "px4_tsdev_set_channel %d:%u: tc90522_is_signal_locked_t() failed. (ret: %d)\n", dev_idx, tsdev_id, ret);
			break;
		}

		dev_dbg(px4->dev, "px4_tsdev_set_channel %d:%u: tc90522_is_signal_locked_t() locked: %d, count: %d\n", dev_idx, tsdev_id, demod_locked, i);

		if (!demod_locked) {
			ret = -EAGAIN;
			break;
		}

		if (i > 265)
			msleep((i - 265) * 10);

		break;
	}

	default:
		ret = -EIO;
		break;
	}

	if (!ret)
		dev_dbg(px4->dev, "px4_tsdev_set_channel %d:%u: succeeded.\n", dev_idx, tsdev_id);
	else
		dev_dbg(px4->dev, "px4_tsdev_set_channel %d:%u: failed. (ret: %d)\n", dev_idx, tsdev_id, ret);

	return ret;
}

static int px4_tsdev_start_streaming(struct px4_tsdev *tsdev)
{
	int ret = 0;
	struct px4_device *px4 = container_of(tsdev, struct px4_device, tsdev[tsdev->id]);
	struct it930x_bus *bus = &px4->it930x.bus;
	struct tc90522_demod *tc90522 = &tsdev->tc90522;
	unsigned int ringbuffer_size;
	unsigned int streaming_count;

	if (atomic_read(&tsdev->streaming))
		// already started
		return 0;

	atomic_set(&tsdev->streaming, 1);

	mutex_lock(&px4->lock);

	if (!px4->streaming_count) {
		bus->usb.streaming_urb_num = max_urbs;
		bus->usb.streaming_no_dma = no_dma;

		dev_dbg(px4->dev, "px4_tsdev_start_streaming %d:%u: max_urbs: %u, no_dma: %c\n", px4->dev_idx, tsdev->id, bus->usb.streaming_urb_num, (bus->usb.streaming_no_dma) ? 'Y' : 'N');

		ret = it930x_purge_psb(&px4->it930x);
		if (ret) {
			dev_err(px4->dev, "px4_tsdev_start_streaming %d:%u: it930x_purge_psb() failed. (ret: %d)\n", px4->dev_idx, tsdev->id, ret);
			goto fail;
		}
	}

	ringbuffer_size = 188 * tsdev_max_packets;
	dev_dbg(px4->dev, "px4_tsdev_start_streaming %d:%u: size of ringbuffer: %u\n", px4->dev_idx, tsdev->id, ringbuffer_size);

	switch (tsdev->isdb) {
	case ISDB_S:
		// enable ts pins
		ret = tc90522_enable_ts_pins_s(tc90522, true);
		if (ret) {
			dev_err(px4->dev, "px4_tsdev_start_streaming %d:%u: tc90522_enable_ts_pins_s(true) failed. (ret: %d)\n", px4->dev_idx, tsdev->id, ret);

			// disable ts pins
			tc90522_enable_ts_pins_s(tc90522, false);
		}
		break;

	case ISDB_T:
		// enable ts pins
		ret = tc90522_enable_ts_pins_t(tc90522, true);
		if (ret) {
			dev_err(px4->dev, "px4_tsdev_start_streaming %d:%u: tc90522_enable_ts_pins_t(true) failed. (ret: %d)\n", px4->dev_idx, tsdev->id, ret);

			// disable ts pins
			tc90522_enable_ts_pins_t(tc90522, false);
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

	if (!px4->streaming_count) {
		px4->stream_context->remain_len = 0;

		dev_dbg(px4->dev, "px4_tsdev_start_streaming %d:%u: starting...\n", px4->dev_idx, tsdev->id);
		ret = it930x_bus_start_streaming(bus, px4_on_stream, px4->stream_context);
		if (ret) {
			dev_err(px4->dev, "px4_tsdev_start_streaming %d:%u: it930x_bus_start_streaming() failed. (ret: %d)\n", px4->dev_idx, tsdev->id, ret);
			goto fail_after_ringbuffer;
		}
	}

	px4->streaming_count++;
	streaming_count = px4->streaming_count;

	mutex_unlock(&px4->lock);

	dev_dbg(px4->dev, "px4_tsdev_start_streaming %d:%u: streaming_count: %u\n", px4->dev_idx, tsdev->id, streaming_count);

	return ret;

fail_after_ringbuffer:
	ringbuffer_stop(tsdev->ringbuf);
fail:
	mutex_unlock(&px4->lock);
	atomic_set(&tsdev->streaming, 0);

	dev_err(px4->dev, "px4_tsdev_start_streaming %d:%u: failed. (ret: %d)\n", px4->dev_idx, tsdev->id, ret);

	return ret;
}

static int px4_tsdev_stop_streaming(struct px4_tsdev *tsdev, bool avail)
{
	int ret = 0;
	struct px4_device *px4 = container_of(tsdev, struct px4_device, tsdev[tsdev->id]);
	struct tc90522_demod *tc90522 = &tsdev->tc90522;
	unsigned int streaming_count;

	if (!atomic_read(&tsdev->streaming))
		// already stopped
		return 0;

	atomic_set(&tsdev->streaming, 0);

	mutex_lock(&px4->lock);

	px4->streaming_count--;
	if (!px4->streaming_count) {
		dev_dbg(px4->dev, "px4_tsdev_stop_streaming %d:%u: stopping...\n", px4->dev_idx, tsdev->id);
		it930x_bus_stop_streaming(&px4->it930x.bus);
	}
	streaming_count = px4->streaming_count;

	ringbuffer_stop(tsdev->ringbuf);

	if (!avail) {
		mutex_unlock(&px4->lock);
		return 0;
	}

	switch (tsdev->isdb) {
	case ISDB_S:
		// disable ts pins
		ret = tc90522_enable_ts_pins_s(tc90522, false);
		break;

	case ISDB_T:
		// disable ts pins
		ret = tc90522_enable_ts_pins_t(tc90522, false);
		break;

	default:
		ret = -EIO;
		break;
	}

	mutex_unlock(&px4->lock);

	dev_dbg(px4->dev, "px4_tsdev_stop_streaming %d:%u: streaming_count: %u\n", px4->dev_idx, tsdev->id, streaming_count);

	return ret;
}

static int px4_tsdev_get_cn(struct px4_tsdev *tsdev, u32 *cn)
{
	int ret = 0;
	struct px4_device *px4 = container_of(tsdev, struct px4_device, tsdev[tsdev->id]);
	struct tc90522_demod *tc90522 = &tsdev->tc90522;

	mutex_lock(&px4->lock);

	switch (tsdev->isdb) {
	case ISDB_S:
		ret = tc90522_get_cn_s(tc90522, (u16 *)cn);
		break;

	case ISDB_T:
		ret = tc90522_get_cndat_t(tc90522, cn);
		break;

	default:
		ret = -EIO;
		break;
	}

	mutex_unlock(&px4->lock);

	return ret;
}

static int px4_tsdev_set_lnb_power(struct px4_tsdev *tsdev, bool enable)
{
	int ret = 0;
	struct px4_device *px4 = container_of(tsdev, struct px4_device, tsdev[tsdev->id]);

	if ((tsdev->lnb_power && enable) || (!tsdev->lnb_power && !enable))
		return 0;

	mutex_lock(&px4->lock);

	if (enable) {
		if (!px4->lnb_power_count)
			ret = it930x_set_gpio(&px4->it930x, 11, true);

		px4->lnb_power_count++;
	} else {
		if (px4->lnb_power_count == 1)
			ret = it930x_set_gpio(&px4->it930x, 11, false);

		px4->lnb_power_count--;
	}

	mutex_unlock(&px4->lock);

	tsdev->lnb_power = enable;

	return ret;
}

struct tc90522_regbuf tc_init_s0[] = {
	{ 0x07, NULL, { 0x31 } },
	{ 0x08, NULL, { 0x77 } }
};

struct tc90522_regbuf tc_init_t0[] = {
	{ 0x0e, NULL, { 0x77 } },
	{ 0x0f, NULL, { 0x13 } }
};

static int px4_tsdev_open(struct inode *inode, struct file *file)
{
	int ret = 0, ref;
	int minor = (iminor(inode) - MINOR(px4_dev_first));
	int dev_idx = (minor / TSDEV_NUM);
	unsigned int tsdev_id = (minor % TSDEV_NUM);
	struct px4_device *px4;
	struct px4_tsdev *tsdev;

	mutex_lock(&glock);

	px4 = devs[dev_idx];
	if (!px4) {
		pr_err("px4_tsdev_open %d:%u: px4 is NULL.\n", dev_idx, tsdev_id);
		mutex_unlock(&glock);
		return -EFAULT;
	}

	if (!atomic_read(&px4->avail)) {
		// not available
		mutex_unlock(&glock);
		return -EIO;
	}

	tsdev = &px4->tsdev[tsdev_id];

	mutex_lock(&tsdev->lock);
	mutex_unlock(&glock);

	if (tsdev->open) {
		// already used by another
		ret = -EALREADY;
		mutex_unlock(&tsdev->lock);
		goto fail;
	}

	mutex_lock(&px4->lock);

	ref = px4_ref(px4);
	if (ref <= 1) {
		ret = -ECANCELED;
		mutex_unlock(&px4->lock);
		mutex_unlock(&tsdev->lock);
		goto fail;
	}

	dev_dbg(px4->dev, "px4_tsdev_open %d:%u: ref count: %d\n", dev_idx, tsdev_id, ref);

	if (ref == 2) {
		int i;

		ret = px4_set_power(px4, true);
		if (ret) {
			dev_err(px4->dev, "px4_tsdev_open %d:%u: px4_set_power(true) failed.\n", dev_idx, tsdev_id);
			goto fail_after_ref;
		}

		for (i = 0; i < TSDEV_NUM; i++) {
			struct px4_tsdev *t = &px4->tsdev[i];

			if (i == tsdev->id)
				continue;

			if (!t->open) {
				switch (t->isdb) {
				case ISDB_S:
					ret = rt710_sleep(&tsdev->t.rt710);
					if (ret) {
						dev_err(px4->dev, "px4_tsdev_open %d:%u: rt710_sleep(%d) failed. (ret: %d)\n", dev_idx, tsdev_id, i, ret);
						break;
					}

					ret = tc90522_sleep_s(&t->tc90522, true);
					if (ret) {
						dev_err(px4->dev, "px4_tsdev_open %d:%u: tc90522_sleep_s(%d, true) failed. (ret: %d)\n", dev_idx, tsdev_id, i, ret);
						break;
					}

					break;

				case ISDB_T:
#if 0
					ret = r850_sleep(&tsdev->t.r850);
					if (ret) {
						dev_err(px4->dev, "px4_tsdev_open %d:%u: rt850_sleep(%d) failed. (ret: %d)\n", dev_idx, tsdev_id, i, ret);
						break;
					}
#endif

					ret = tc90522_sleep_t(&t->tc90522, true);
					if (ret) {
						dev_err(px4->dev, "px4_tsdev_open %d:%u: tc90522_sleep_t(%d, true) failed. (ret: %d)\n", dev_idx, tsdev_id, i, ret);
						break;
					}
					
					break;

				default:
					break;
				}
			}

			if (ret)
				break;
		}

		if (i < TSDEV_NUM)
			goto fail_after_power;
	}

	ret = px4_tsdev_init(tsdev);
	if (ret) {
		dev_err(px4->dev, "px4_tsdev_open %d:%u: px4_tsdev_init() failed.\n", dev_idx, tsdev_id);
		goto fail_after_power;
	}

	if (ref == 2) {
		// S0
		ret = tc90522_write_regs(&px4->tsdev[0].tc90522, tc_init_s0, ARRAY_SIZE(tc_init_s0));
		if (ret) {
			dev_err(px4->dev, "px4_tsdev_open %d:%u: tc90522_write_regs(tc_init_s0) failed.\n", dev_idx, tsdev_id);
			goto fail_after_power;
		}

		// T0
		ret = tc90522_write_regs(&px4->tsdev[2].tc90522, tc_init_t0, ARRAY_SIZE(tc_init_t0));
		if (ret) {
			dev_err(px4->dev, "px4_tsdev_open %d:%u: tc90522_write_regs(tc_init_t0) failed.\n", dev_idx, tsdev_id);
			goto fail_after_power;
		}
	}

	tsdev->open = true;

	file->private_data = tsdev;

	mutex_unlock(&px4->lock);
	mutex_unlock(&tsdev->lock);

	dev_dbg(px4->dev, "px4_tsdev_open %d:%u: ok\n", dev_idx, tsdev_id);

	return 0;

fail_after_power:
	if (ref == 2)
		px4_set_power(px4, false);
fail_after_ref:
	px4_unref(px4);
	mutex_unlock(&px4->lock);
	mutex_unlock(&tsdev->lock);
fail:
	dev_err(px4->dev, "px4_tsdev_open %d:%u: failed. (ret: %d)\n", dev_idx, tsdev_id, ret);

	return ret;
}

static ssize_t px4_tsdev_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	int ret = 0;
	struct px4_device *px4;
	struct px4_tsdev *tsdev;
	size_t rd;

	tsdev = file->private_data;
	if (!tsdev) {
		pr_err("px4_tsdev_read: tsdev is NULL.\n");
		return -EFAULT;
	}

	px4 = container_of(tsdev, struct px4_device, tsdev[tsdev->id]);

	rd = count;
	ret = ringbuffer_read_user(tsdev->ringbuf, buf, &rd);

	return (ret) ? (ret) : (rd);
}

static int px4_tsdev_release(struct inode *inode, struct file *file)
{
	int avail, ref;
	struct px4_device *px4;
	struct px4_tsdev *tsdev;

	tsdev = file->private_data;
	if (!tsdev) {
		pr_err("px4_tsdev_release: tsdev is NULL.\n");
		return -EFAULT;
	}

	px4 = container_of(tsdev, struct px4_device, tsdev[tsdev->id]);
	avail = atomic_read(&px4->avail);

	dev_dbg(px4->dev, "px4_tsdev_release %d:%u: avail: %d\n", px4->dev_idx, tsdev->id, avail);

	mutex_lock(&tsdev->lock);

	px4_tsdev_stop_streaming(tsdev, (avail) ? true : false);

	if (avail) {
		if (tsdev->isdb == ISDB_S)
			px4_tsdev_set_lnb_power(tsdev, false);

		px4_tsdev_term(tsdev);
	}

	mutex_lock(&px4->lock);

	ref = px4_unref(px4);
	if (avail && ref <= 1)
		px4_set_power(px4, false);

	mutex_unlock(&px4->lock);

	tsdev->open = false;

	mutex_unlock(&tsdev->lock);

	wake_up(&px4->wait);

	dev_dbg(px4->dev, "px4_tsdev_release %d:%u: ok. ref count: %d\n", px4->dev_idx, tsdev->id, ref);

	return 0;
}

static long px4_tsdev_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long ret = -EIO;
	struct px4_device *px4;
	struct px4_tsdev *tsdev;
	int avail;
	int dev_idx;
	unsigned int tsdev_id;
	unsigned long t;

	tsdev = file->private_data;
	if (!tsdev) {
		pr_err("px4_tsdev_unlocked_ioctl: tsdev is NULL.\n");
		return -EFAULT;
	}

	px4 = container_of(tsdev, struct px4_device, tsdev[tsdev->id]);

	avail = atomic_read(&px4->avail);

	dev_idx = px4->dev_idx;
	tsdev_id = tsdev->id;

	mutex_lock(&tsdev->lock);

	switch (cmd) {
	case PTX_SET_CHANNEL:
	{
		struct ptx_freq freq;

		dev_dbg(px4->dev, "px4_tsdev_unlocked_ioctl %d:%u: PTX_SET_CHANNEL\n", dev_idx, tsdev_id);

		if (!avail) {
			ret = -EIO;
			break;
		}

		t = copy_from_user(&freq, (void *)arg, sizeof(freq));

		ret = px4_tsdev_set_channel(tsdev, &freq);
		break;
	}

	case PTX_START_STREAMING:
		dev_dbg(px4->dev, "px4_tsdev_unlocked_ioctl %d:%u: PTX_START_STREAMING\n", dev_idx, tsdev_id);

		if (!avail) {
			ret = -EIO;
			break;
		}

		ret = px4_tsdev_start_streaming(tsdev);
		break;

	case PTX_STOP_STREAMING:
		dev_dbg(px4->dev, "px4_tsdev_unlocked_ioctl %d:%u: PTX_STOP_STREAMING\n", dev_idx, tsdev_id);
		ret = px4_tsdev_stop_streaming(tsdev, (avail) ? true : false);
		break;

	case PTX_GET_CNR:
	{
		int cn = 0;

		dev_dbg(px4->dev, "px4_tsdev_unlocked_ioctl %d:%u: PTX_GET_CNR\n", dev_idx, tsdev_id);

		if (!avail) {
			ret = -EIO;
			break;
		}

		ret = px4_tsdev_get_cn(tsdev, (u32 *)&cn);
		if (!ret)
			t = copy_to_user((void *)arg, &cn, sizeof(cn));

		break;
	}

	case PTX_ENABLE_LNB_POWER:
	{
		int lnb;
		bool b;

		lnb = (int)arg;

		dev_dbg(px4->dev, "px4_tsdev_unlocked_ioctl %d:%u: PTX_ENABLE_LNB_POWER lnb: %d\n", dev_idx, tsdev_id, lnb);

		if (tsdev->isdb != ISDB_S) {
			ret = -EINVAL;
			break;
		}

		if (!avail) {
			ret = -EIO;
			break;
		}

		if (lnb == 0) {
			// 0V
			b = false;
		} else if (lnb == 2) {
			// 15V
			b = true;
		} else {
			ret = -EINVAL;
			break;
		}

		ret = px4_tsdev_set_lnb_power(tsdev, b);
		break;
	}

	case PTX_DISABLE_LNB_POWER:
		dev_dbg(px4->dev, "px4_tsdev_unlocked_ioctl %d:%u: PTX_DISABLE_LNB_POWER\n", dev_idx, tsdev_id);

		if (tsdev->isdb != ISDB_S) {
			ret = -EINVAL;
			break;
		}

		if (!avail) {
			ret = -EIO;
			break;
		}

		ret = px4_tsdev_set_lnb_power(tsdev, false);
		break;

	default:
		dev_dbg(px4->dev, "px4_tsdev_unlocked_ioctl %d:%u: unknown ioctl 0x%08x\n", dev_idx, tsdev_id, cmd);
		ret = -ENOSYS;
		break;
	}

	mutex_unlock(&tsdev->lock);

	return ret;
}

static struct file_operations px4_tsdev_fops = {
	.owner = THIS_MODULE,
	.open = px4_tsdev_open,
	.read = px4_tsdev_read,
	.release = px4_tsdev_release,
	.unlocked_ioctl = px4_tsdev_unlocked_ioctl
};

static int px4_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct device *dev = &intf->dev;
	int ret = 0, dev_idx = -1, i;
	struct usb_device *usbdev;
	struct px4_device *px4 = NULL;
	struct it930x_bridge *it930x;
	struct it930x_bus *bus;

	dev_dbg(dev, "px4_probe: xfer_packets: %u\n", xfer_packets);

	mutex_lock(&glock);

	for (i = 0; i < MAX_DEVICE; i++) {
		if (!devs[i] && !devs_reserve[i]) {
			dev_idx = i;
			devs_reserve[i] = true;
			break;
		}
	}

	mutex_unlock(&glock);

	dev_dbg(dev, "px4_probe: dev_idx: %d\n", dev_idx);

	if (dev_idx == -1) {
		dev_err(dev, "Unused device index was not found.\n");
		ret = -ECANCELED;
		goto fail_before_base;
	}

	usbdev = interface_to_usbdev(intf);

	if (usbdev->speed < USB_SPEED_HIGH)
		dev_warn(dev, "This device is operating as USB 1.1 or less.\n");

	px4 = kzalloc(sizeof(*px4), GFP_KERNEL);
	if (!px4) {
		dev_err(dev, "px4_probe: kzalloc() failed.\n");
		ret = -ENOMEM;
		goto fail_before_base;
	}

	px4->stream_context = (struct px4_stream_context *)kzalloc(sizeof(*px4->stream_context), GFP_ATOMIC);
	if (!px4->stream_context) {
		dev_err(dev, "px4_probe: kzalloc() failed. (2)\n");
		ret = -ENOMEM;
		goto fail_before_base;
	}

	px4->dev = dev;
	px4->dev_idx = dev_idx;
	px4->dev_id = 0;

	if (strlen(usbdev->serial) == 15)
		if (kstrtouint(&usbdev->serial[14], 16, &px4->dev_id))
			dev_err(dev, "px4_probe: kstrtouint() failed.\n");
		else
			dev_dbg(dev, "px4_probe: dev_id: %u\n", px4->dev_id);
	else
		dev_warn(dev, "px4_probe: the length of serial number is invalid.\n");

	// Initialize px4 structure

	ret = px4_init(px4);
	if (ret)
		goto fail_before_base;

	it930x = &px4->it930x;
	bus = &it930x->bus;

	// Initialize bus operator

	bus->dev = dev;
	bus->type = IT930X_BUS_USB;
	bus->usb.dev = usbdev;
	bus->usb.ctrl_timeout = 3000;
	bus->usb.streaming_xfer_size = xfer_packets * 188;

	ret = it930x_bus_init(bus);
	if (ret)
		goto fail_before_bus;

	// Initialize bridge operator

	it930x->dev = dev;

	ret = it930x_init(it930x);
	if (ret)
		goto fail_before_bridge;

	// Load config from eeprom

	ret = px4_load_config(px4);
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

	ret = it930x_set_gpio(it930x, 7, true);
	if (ret)
		goto fail;

	ret = it930x_set_gpio(it930x, 2, false);
	if (ret)
		goto fail;

	// LNB power supply: off
	ret = it930x_set_gpio(it930x, 11, false);
	if (ret)
		goto fail;

	// cdev

	cdev_init(&px4->cdev, &px4_tsdev_fops);
	px4->cdev.owner = THIS_MODULE;

	ret = cdev_add(&px4->cdev, MKDEV(MAJOR(px4_dev_first), MINOR(px4_dev_first) + (dev_idx * TSDEV_NUM)), TSDEV_NUM);
	if (ret < 0) {
		dev_err(dev, "Couldn't add cdev to the system.\n");
		goto fail;
	}

	mutex_lock(&glock);

	// create /dev/px4video*
	for (i = 0; i < TSDEV_NUM; i++) {
		dev_info(dev, "tsdev %i: px4video%u\n", i, (MINOR(px4_dev_first) + (dev_idx * TSDEV_NUM) + i));
		device_create(px4_class, &intf->dev, MKDEV(MAJOR(px4_dev_first), (MINOR(px4_dev_first) + (dev_idx * TSDEV_NUM) + i)), NULL, "px4video%u", (MINOR(px4_dev_first) + (dev_idx * TSDEV_NUM) + i));
	}

	devs[dev_idx] = px4;
	devs_reserve[dev_idx] = false;

	mutex_unlock(&glock);

	get_device(dev);

	usb_set_intfdata(intf, px4);

	return 0;

fail:
	it930x_term(it930x);
fail_before_bridge:
	it930x_bus_term(bus);
fail_before_bus:
	px4_term(px4);
fail_before_base:
	if (px4) {
		if (px4->stream_context)
			kfree(px4->stream_context);

		kfree(px4);
	}

	if (dev_idx != -1) {
		mutex_lock(&glock);
		devs_reserve[dev_idx] = false;
		mutex_unlock(&glock);
	}

	return ret;
}

static void px4_disconnect(struct usb_interface *intf)
{
	int i, ref;
	struct px4_device *px4;

	px4 = usb_get_intfdata(intf);
	if (!px4)
		return;

	dev_dbg(px4->dev, "px4_disconnect: dev_idx: %d\n", px4->dev_idx);

	usb_set_intfdata(intf, NULL);

	atomic_set(&px4->avail, 0);
	mutex_lock(&px4->lock);

	mutex_lock(&glock);

	devs[px4->dev_idx] = NULL;

	// delete /dev/px4video*
	for (i = 0; i < TSDEV_NUM; i++)
		device_destroy(px4_class, MKDEV(MAJOR(px4_dev_first), (MINOR(px4_dev_first) + (px4->dev_idx * TSDEV_NUM) + i)));

	mutex_unlock(&glock);

	cdev_del(&px4->cdev);

	ref = px4_unref(px4);

	mutex_unlock(&px4->lock);

	for (i = 0; i < TSDEV_NUM; i++) {
		struct px4_tsdev *tsdev = &px4->tsdev[i];

		mutex_lock(&tsdev->lock);
		px4_tsdev_stop_streaming(tsdev, false);
		mutex_unlock(&tsdev->lock);
	}

	while (ref) {
		wait_event(px4->wait, (ref != atomic_read(&px4->ref)));
		ref = atomic_read(&px4->ref);
	}

	put_device(px4->dev);

	// uninitialize
	it930x_term(&px4->it930x);
	it930x_bus_term(&px4->it930x.bus);
	px4_term(px4);
	kfree(px4->stream_context);
	kfree(px4);

	return;
}

static int px4_suspend(struct usb_interface *intf, pm_message_t message)
{
	return -ENOSYS;
}

static int px4_resume(struct usb_interface *intf)
{
	return 0;
}

static struct usb_driver px4_usb_driver = {
	.name = "px4_drv",
	.probe = px4_probe,
	.disconnect = px4_disconnect,
	.suspend = px4_suspend,
	.resume = px4_resume,
	.id_table = px4_usb_ids
};

static int px4_module_init(void)
{
	int ret = 0, i;

	pr_info(KBUILD_MODNAME
#ifdef PX4_DRIVER_VERSION
		" version " PX4_DRIVER_VERSION
#endif
#ifdef REVISION_NUMBER
#if defined(PX4_DRIVER_VERSION)
		","
#endif
		" rev: " REVISION_NUMBER
#endif
#ifdef COMMIT_HASH
#if defined(PX4_DRIVER_VERSION) || defined(REVISION_NUMBER)
		","
#endif
		" commit: " COMMIT_HASH
#endif
#ifdef REVISION_NAME
		" @ " REVISION_NAME
#endif
		"\n");

	for (i = 0; i < MAX_DEVICE; i++) {
		devs[i] = NULL;
		devs_reserve[i] = false;
	}

	ret = alloc_chrdev_region(&px4_dev_first, 0, MAX_TSDEV, DEVICE_NAME);
	if (ret < 0) {
		pr_err("px4_module_init: alloc_chrdev_region() failed.\n");
		return ret;
	}

	px4_class = class_create(THIS_MODULE, "px4");
	if (IS_ERR(px4_class)) {
		pr_err("px4_module_init: class_create() failed.\n");
		unregister_chrdev_region(px4_dev_first, MAX_TSDEV);
		return PTR_ERR(px4_class);
	}

	ret = usb_register(&px4_usb_driver);
	if (ret) {
		pr_err("px4_module_init: usb_register() failed.\n");
		class_destroy(px4_class);
		unregister_chrdev_region(px4_dev_first, MAX_TSDEV);
	}

	return ret;
}

static void px4_module_exit(void)
{
	pr_debug("px4_module_exit\n");

	usb_deregister(&px4_usb_driver);
	class_destroy(px4_class);
	unregister_chrdev_region(px4_dev_first, MAX_TSDEV);

	pr_debug("px4_module_exit: quit\n");
}

module_init(px4_module_init);
module_exit(px4_module_exit);
