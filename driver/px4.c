// px4.c

#include "print_format.h"

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/cdev.h>

#include "px4.h"
#include "ptx_ioctl.h"
#include "i2c_comm.h"
#include "it930x-config.h"
#include "it930x-bus.h"
#include "it930x.h"
#include "tc90522.h"
#if 0
#include "r850.h"
#endif
#include "rt710.h"
#include "ringbuffer.h"

#if !defined(FIRMWARE_FILENAME)
#define FIRMWARE_FILENAME	"it930x-firmware.bin"
#endif

#if !defined(MAX_DEVICE) || !MAX_DEVICE
#define MAX_DEVICE	16
#endif
#define TSDEV_NUM	4
#define MAX_TSDEV	(MAX_DEVICE * TSDEV_NUM)
#define DEVICE_NAME	"px4"

#define PID_PX_W3U4	0x083f
#define PID_PX_W3PE4	0x023f
#define PID_PX_Q3U4	0x084a
#define PID_PX_Q3PE4	0x024a

#define ISDB_T	0
#define ISDB_S	1

#define TS_SYNC_COUNT	4

struct px4_tsdev {
	unsigned int id;
	int isdb;	// ISDB_T or ISDB_S
	bool init;
	bool open;
	bool lnb_power;
	struct px4_device *px4;
	struct tc90522_demod tc90522;
	union {
#if 0
		struct r850_tuner r850;		// for ISDB-T
#endif
		struct rt710_tuner rt710;	// for ISDB-S
	} t;
	atomic_t streaming;	// 0: not streaming, !0: streaming
	struct ringbuffer rgbuf;
};

struct px4_device {
	atomic_t ref;		// reference counter
	atomic_t avail;		// availability flag
	wait_queue_head_t wait;
	struct mutex lock;
	int dev_idx;
	u16 vid;		// Vendor id
	u16 pid;		// Product id
	unsigned int dev_id;	// 1 or 2
	struct it930x_bridge it930x;
	struct cdev cdev;
	unsigned long streaming_count;
	struct px4_tsdev tsdev[TSDEV_NUM];
};

MODULE_AUTHOR("nns779");
MODULE_DESCRIPTION("PLEX PX-W3U4/W3PE4 Unofficial Linux driver");
MODULE_LICENSE("GPL v2");

MODULE_FIRMWARE(FIRMWARE_FILENAME);

static DEFINE_MUTEX(glock);
static struct class *px4_class = NULL;
static dev_t px4_dev_first;
static struct px4_device *devs[MAX_DEVICE];
static bool devs_reserve[MAX_DEVICE];
static unsigned int xfer_packets = 816;
static unsigned int max_urbs = 6;
static bool no_dma = false;

module_param(xfer_packets, uint, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(xfer_packets, "Number of transfer packets from the device. (default: 816)");

module_param(max_urbs, uint, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(max_urbs, "Maximum number of URBs. (default: 6)");

module_param(no_dma, bool, S_IRUSR | S_IWUSR);

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
	int i;

	atomic_set(&px4->ref, 1);
	atomic_set(&px4->avail, 1);
	init_waitqueue_head(&px4->wait);
	mutex_init(&px4->lock);
	px4->streaming_count = 0;

	for (i = 0; i < TSDEV_NUM; i++) {
		struct px4_tsdev *tsdev = &px4->tsdev[i];

		tsdev->id = i;
		tsdev->init = false;
		tsdev->open = false;
		tsdev->lnb_power = false;
		tsdev->px4 = px4;
		atomic_set(&tsdev->streaming, 0);
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
	struct it930x_bridge *it930x = &px4->it930x;
	u8 tmp;
	int i;

	ret = it930x_read_reg(it930x, 0x4979, &tmp);
	if (ret) {
		pr_debug("px4_load_config: it930x_read_reg(0x4979) failed.\n");
		pr_notice("Couldn't load configuration from the device.\n");
		return ret;
	} else if (!tmp) {
		pr_warn("EEPROM is invalid.\n");
		return ret;
	} else {
		u8 buf[1];

		ret = it930x_read_reg(it930x, 0x49ac, &buf[0]);
		if (!ret)
			pr_info("IR mode: %x\n", buf[0]);
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

		tsdev->tc90522.i2c = &it930x->i2c_master[0];
		tsdev->tc90522.i2c_addr = input->i2c_addr;

		switch (tsdev->isdb) {
		case ISDB_S:
			tsdev->t.rt710.i2c = &tsdev->tc90522.i2c_master;
			tsdev->t.rt710.i2c_addr = 0x7a;
			break;

		case ISDB_T:
#if 0
			tsdev->t.r850.i2c = &tsdev->tc90522.i2c_master;
			tsdev->t.r850.i2c_addr = 0x7c;
#endif
			// not implemented
			break;
		}
	}

	it930x->input[4].enable = false;

	return 0;
}

static int px4_control_power(struct px4_device *px4, bool on)
{
	int ret = 0;
	struct it930x_bridge *it930x = &px4->it930x;

	if (on) {
		int i;

		ret = it930x_set_gpio(it930x, 7, false);
		if (ret)
			return ret;

		ret = it930x_set_gpio(it930x, 2, false);
		if (ret)
			return ret;

		msleep(10);

		ret = it930x_set_gpio(it930x, 2, true);
		if (ret)
			return ret;

		msleep(10);

		for (i = 0; i < TSDEV_NUM; i++) {
			px4->tsdev[i].init = false;

			if (!px4->tsdev[i].open) {
				switch (px4->tsdev[i].isdb) {
				case ISDB_S:
					ret = tc90522_sleep_s(&px4->tsdev[i].tc90522, true);
					break;

				case ISDB_T:
					ret = tc90522_sleep_t(&px4->tsdev[i].tc90522, true);
					break;

				default:
					break;
				}
			}

			if (ret) {
				// error
				it930x_set_gpio(it930x, 7, true);
				it930x_set_gpio(it930x, 2, false);
				break;
			}
		}
	} else {
		it930x_set_gpio(it930x, 7, true);
		it930x_set_gpio(it930x, 2, false);
	}

	return ret;
}

static int px4_control_lnb_power(struct px4_device *px4, bool on)
{
	int i;
	bool b = false;

	for (i = 0; i < TSDEV_NUM; i++) {
		if (px4->tsdev[i].lnb_power) {
			b = true;
			break;
		}
	}

	if ((b && !on) || (!b && on))
		return 0;

	return it930x_set_gpio(&px4->it930x, 11, (on) ? true : false);
}

static bool px4_ts_sync(u8 **buf, u32 *len, bool *sync_remain)
{
	bool ret = false;
	u8 *p = *buf;
	u32 remain = *len;
	bool b = false;

	while (remain) {
		u32 i;

		b = true;
		for (i = 0; i < TS_SYNC_COUNT; i++) {
			if (remain > (i * 188)) {
				if ((p[i * 188] & 0x8f) != 0x07) {
					b = false;
					break;
				}
			} else {
				break;
			}
		}

		if (i == TS_SYNC_COUNT) {
			// ok
			b = false;
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

static void px4_ts_write(struct px4_device *px4, u8 **buf, u32 *len)
{
	u8 *p = *buf;
	u32 remain = *len;

	while (remain >= 188 && ((p[0] & 0x8f) == 0x07)) {
		u8 id = (p[0] & 0x70) >> 4;

		if (id && id < 5) {
			p[0] = 0x47;
			ringbuffer_write(&px4->tsdev[id - 1].rgbuf, p, 188);
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
	struct px4_device *px4 = context;
	u8 *p = buf;
	u32 remain = len;
	bool sync_remain = false;

	while (remain) {
		if (!px4_ts_sync(&p, &remain, &sync_remain))
			break;

		px4_ts_write(px4, &p, &remain);
	}

	if (sync_remain)
		pr_debug("px4_on_stream: sync_remain remain: %u\n", remain);

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
	struct tc90522_demod *tc90522 = &tsdev->tc90522;

	if (tsdev->init)
		// already initialized
		return 0;

	switch (tsdev->isdb) {
	case ISDB_S:
	{
		ret = tc90522_write_regs(tc90522, tc_init_s, ARRAY_SIZE(tc_init_s));
		if (ret)
			break;

		// disable ts pins
		ret = tc90522_enable_ts_pins_s(tc90522, false);
		if (ret)
			break;

		// wake up
		ret = tc90522_sleep_s(tc90522, false);
		if (ret)
			break;

		ret = rt710_init(&tsdev->t.rt710);
		if (ret) {
			pr_debug("px4_tsdev_init: rt710_init() failed.\n");
			break;
		}

		break;
	}

	case ISDB_T:
	{
#if 0
		ret = tc90522_write_regs(tc90522, tc_init_t, ARRAY_SIZE(tc_init_t));
		if (ret)
			break;

		// disable ts pins
		ret = tc90522_enable_ts_pins_t(tc90522, false);
		if (ret)
			break;

		// wake up
		ret = tc90522_sleep_t(tc90522, false);
		if (ret)
			break;
#else
		// not implemented
		ret = -EIO;
#endif
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

static void px4_tsdev_uninit(struct px4_tsdev *tsdev)
{
	struct tc90522_demod *tc90522 = &tsdev->tc90522;

	switch (tsdev->isdb) {
	case ISDB_S:
		tc90522_sleep_s(tc90522, true);
		break;

	case ISDB_T:
		tc90522_sleep_t(tc90522, true);
		break;
	}

	return;
}

static int px4_tsdev_set_channel(struct px4_tsdev *tsdev, struct ptx_freq *freq)
{
	int ret = 0;
	struct tc90522_demod *tc90522 = &tsdev->tc90522;
	struct tc90522_regbuf regbuf_tc[2];
	u32 real_freq;

	pr_debug("px4_tsdev_set_channel: freq_no: %d, slot: %d\n", freq->freq_no, freq->slot);

	switch (tsdev->isdb) {
	case ISDB_S:
	{
		int i;
		bool tuner_locked;
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

		// set frequency

		ret = tc90522_set_agc_s(tc90522, false);
		if (ret)
			break;
		tc90522_regbuf_set_val(&regbuf_tc[0], 0x8e, 0x06/*0x02*/);
		tc90522_regbuf_set_val(&regbuf_tc[1], 0xa3, 0xf7);
		ret = tc90522_write_regs(tc90522, regbuf_tc, 2);
		if (ret)
			break;

		ret = rt710_set_params(&tsdev->t.rt710, real_freq, 28860, 4);
		if (ret) {
			pr_debug("px4_tsdev_set_channel: rt710_set_params(%u, 28860, 4) failed.\n", real_freq);
			break;
		}

		i = 50;
		while (i--) {
			ret = rt710_get_pll_locked(&tsdev->t.rt710, &tuner_locked);
			if (!ret && tuner_locked)
				break;

			msleep(10);
		}
		if (ret) {
			pr_debug("px4_tsdev_set_channel: rt710_get_pll_locked() failed.\n");
			break;
		} else {
			pr_debug("px4_tsdev_set_channel: rt710_get_pll_locked() locked: %d, count: %d\n", tuner_locked, i);
		}

		if (!tuner_locked) {
			// PLL error
			ret = -EIO;
			break;
		}

		ret = tc90522_set_agc_s(tc90522, true);
		if (ret)
			break;

		// set slot

		i = 50;
		while (i--) {
			ret = tc90522_tmcc_get_tsid_s(tc90522, freq->slot, &tsid);
			if (!ret || ret == -EINVAL)
				break;

			msleep(10);
		}
		if (ret) {
			pr_debug("px4_tsdev_set_channel: tc90522_tmcc_get_tsid_s() failed.\n");
			break;
		} else {
			pr_debug("px4_tsdev_set_channel: tc90522_tmcc_get_tsid_s() tsid: %04x, count: %d\n", tsid, i);
		}

		ret = tc90522_set_tsid_s(tc90522, tsid);
		if (ret)
			break;

		i = 50;
		while(i--) {
			ret = tc90522_get_tsid_s(tc90522, &tsid2);
			if (!ret && tsid2 == tsid)
				break;

			msleep(10);
		}

		pr_debug("px4_tsdev_set_channel: tc90522_get_tsid_s() tsid2: %04x, count: %d\n", tsid2, i);

		break;
	}

	case ISDB_T:
	{
		// not implemented
		ret = -EIO;
		break;
	}

	default:
		ret = -EIO;
		break;
	}

	return ret;
}

static int px4_tsdev_start_streaming(struct px4_tsdev *tsdev)
{
	int ret = 0;
	size_t buf_size;
	struct px4_device *px4 = tsdev->px4;
	struct it930x_bus *bus = &px4->it930x.bus;
	struct tc90522_demod *tc90522 = &tsdev->tc90522;

	if (atomic_read(&tsdev->streaming))
		// already started
		return 0;

	atomic_set(&tsdev->streaming, 1);

	if (!px4->streaming_count) {
		bus->usb.streaming_urb_num = max_urbs;
		bus->usb.streaming_no_dma = no_dma;

		pr_debug("px4_tsdev_start_streaming: max_urbs: %u, no_dma: %c\n", bus->usb.streaming_urb_num, (bus->usb.streaming_no_dma) ? 'Y' : 'N');
	}

	buf_size = bus->usb.streaming_xfer_size + bus->usb.streaming_urb_num;

	switch (tsdev->isdb) {
	case ISDB_S:
		// enable ts pins
		ret = tc90522_enable_ts_pins_s(tc90522, true);
		if (ret)
			// disable ts pins
			tc90522_enable_ts_pins_s(tc90522, false);
		break;

	case ISDB_T:
		// enable ts pins
		ret = tc90522_enable_ts_pins_t(tc90522, true);
		if (ret)
			// disable ts pins
			tc90522_enable_ts_pins_t(tc90522, false);
		break;

	default:
		ret = -EIO;
		break;
	}

	if (ret)
		goto fail;

	ret = ringbuffer_init(&tsdev->rgbuf, buf_size);
	if (ret)
		goto fail;

	if (!px4->streaming_count) {
		ret = it930x_bus_start_streaming(bus, px4_on_stream, px4);
		if (ret)
			goto fail_after_ringbuffer;
	}

	px4->streaming_count++;

	return ret;

fail_after_ringbuffer:
	ringbuffer_flush(&tsdev->rgbuf);
	ringbuffer_term(&tsdev->rgbuf);
fail:
	atomic_set(&tsdev->streaming, 0);
	return ret;
}

static int px4_tsdev_stop_streaming(struct px4_tsdev *tsdev, bool avail)
{
	int ret = 0;
	struct px4_device *px4 = tsdev->px4;
	struct tc90522_demod *tc90522 = &tsdev->tc90522;

	if (!atomic_read(&tsdev->streaming))
		// already stopped
		return 0;

	atomic_set(&tsdev->streaming, 0);

	px4->streaming_count--;
	if (!px4->streaming_count)
		it930x_bus_stop_streaming(&px4->it930x.bus);

	ringbuffer_flush(&tsdev->rgbuf);
	ringbuffer_term(&tsdev->rgbuf);

	if (!avail)
		return 0;

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

	return ret;
}

static int px4_tsdev_get_cn(struct px4_tsdev *tsdev, u32 *cn)
{
	int ret = 0;
	struct tc90522_demod *tc90522 = &tsdev->tc90522;

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
	int minor = iminor(inode) - MINOR(px4_dev_first);
	struct px4_device *px4;
	struct px4_tsdev *tsdev;

	mutex_lock(&glock);

	px4 = devs[minor / TSDEV_NUM];
	if (!px4) {
		pr_err("px4_tsdev_open: px4 is NULL.\n");
		mutex_unlock(&glock);
		return -EFAULT;
	}

	if (!atomic_read(&px4->avail)) {
		// not available
		mutex_unlock(&glock);
		return -EIO;
	}

	ref = px4_ref(px4);
	pr_debug("px4_tsdev_open: ref count: %d\n", ref);

	mutex_lock(&px4->lock);
	mutex_unlock(&glock);

	tsdev = &px4->tsdev[minor % TSDEV_NUM];

	if (tsdev->open) {
		// already used by another
		ret = -EIO;
		goto fail;
	}

	tsdev->open = true;

	if (ref == 2) {
		ret = px4_control_power(px4, true);
		if (ret)
			goto fail;
	}

	ret = px4_tsdev_init(tsdev);
	if (ret)
		goto fail_after_power;

	if (ref == 2) {
		// S0
		ret = tc90522_write_regs(&px4->tsdev[0].tc90522, tc_init_s0, ARRAY_SIZE(tc_init_s0));
		if (ret)
			goto fail_after_power;

		// T0
		ret = tc90522_write_regs(&px4->tsdev[2].tc90522, tc_init_t0, ARRAY_SIZE(tc_init_t0));
		if (ret)
			goto fail_after_power;
	}

	file->private_data = tsdev;

	mutex_unlock(&px4->lock);

	return 0;

fail_after_power:
	if (ref == 2)
		px4_control_power(px4, false);
fail:
	tsdev->open = false;
	mutex_unlock(&px4->lock);
	px4_unref(px4);

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

	px4 = tsdev->px4;

	if (!atomic_read(&px4->avail) || !atomic_read(&tsdev->streaming))
		return -EIO;

	rd = count;
	ret = ringbuffer_read_to_user(&tsdev->rgbuf, buf, &rd);

	return (ret) ? (ret) : (rd);
}

static int px4_tsdev_release(struct inode *inode, struct file *file)
{
	int avail, ref;
	struct px4_device *px4;
	struct px4_tsdev *tsdev;

	tsdev = file->private_data;
	if (!tsdev) {
		pr_err("px4_tsdev_release tsdev is NULL.\n");
		return -EFAULT;
	}

	px4 = tsdev->px4;
	avail = atomic_read(&px4->avail);

	mutex_lock(&px4->lock);

	px4_tsdev_stop_streaming(tsdev, (avail) ? true : false);

	if (avail)
		px4_tsdev_uninit(tsdev);

	tsdev->open = false;

	tsdev->lnb_power = false;
	if (avail)
		px4_control_lnb_power(px4, false);

	ref = px4_unref(px4);
	if (avail && ref == 1)
		px4_control_power(px4, false);

	mutex_unlock(&px4->lock);
	wake_up(&px4->wait);

	pr_debug("px4_tsdev_release: ref count: %d\n", ref);

	return 0;
}

static long px4_tsdev_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long ret = -EIO;
	struct px4_device *px4;
	struct px4_tsdev *tsdev;
	unsigned long t;

	tsdev = file->private_data;
	if (!tsdev) {
		pr_err("px4_tsdev_unlocked_ioctl: tsdev is NULL.\n");
		return -EFAULT;
	}

	px4 = tsdev->px4;

	if (!atomic_read(&px4->avail))
		return -EIO;

	mutex_lock(&px4->lock);

	switch (cmd) {
	case PTX_SET_CHANNEL:
	{
		struct ptx_freq freq;

		pr_debug("px4_tsdev_unlocked_ioctl: PTX_SET_CHANNEL\n");

		t = copy_from_user(&freq, (void *)arg, sizeof(freq));

		ret = px4_tsdev_set_channel(tsdev, &freq);
		break;
	}

	case PTX_START_STREAMING:
		pr_debug("px4_tsdev_unlocked_ioctl: PTX_START_STREAMING\n");
		ret = px4_tsdev_start_streaming(tsdev);
		break;

	case PTX_STOP_STREAMING:
		pr_debug("px4_tsdev_unlocked_ioctl: PTX_STOP_STREAMING\n");
		ret = px4_tsdev_stop_streaming(tsdev, true);
		break;

	case PTX_GET_CNR:
	{
		int cn = 0;

		pr_debug("px4_tsdev_unlocked_ioctl: PTX_GET_CNR\n");

		ret = px4_tsdev_get_cn(tsdev, (u32 *)&cn);
		if (!ret)
			t = copy_to_user((void *)arg, &cn, sizeof(cn));

		break;
	}

	case PTX_ENABLE_LNB_POWER:
	{
		int lnb;

		lnb = (int)arg;

		pr_debug("px4_tsdev_unlocked_ioctl: PTX_ENABLE_LNB_POWER lnb: %d\n", lnb);

		if (tsdev->isdb != ISDB_S) {
			ret = -EINVAL;
			break;
		}

#ifdef DISABLE_LNB_POWER_Q4
		if (px4->vid == 0x511 && (px4->pid == PID_PX_Q3U4 || px4->pid == PID_PX_Q3PE4)) {
			pr_warn("LNB power supply is disabled.\n");
			ret = -EINVAL;
			break;
		}
#endif

		if (lnb == 0) {
			// 0V
			tsdev->lnb_power = false;
		} else if (lnb == 2) {
			// 15V
			tsdev->lnb_power = true;
		} else {
			ret = -EINVAL;
			break;
		}

		ret = px4_control_lnb_power(px4, tsdev->lnb_power);
		break;
	}

	case PTX_DISABLE_LNB_POWER:
		pr_debug("px4_tsdev_unlocked_ioctl: PTX_DISABLE_LNB_POWER\n");

		if (tsdev->isdb != ISDB_S) {
			ret = -EINVAL;
			break;
		}

#ifdef DISABLE_LNB_POWER_Q4
		if (px4->vid == 0x511 && (px4->pid == PID_PX_Q3U4 || px4->pid == PID_PX_Q3PE4)) {
			ret = -EINVAL;
			break;
		}
#endif

		if (!tsdev->lnb_power) {
			ret = 0;
			break;
		}

		tsdev->lnb_power = false;
		ret= px4_control_lnb_power(px4, false);
		break;

	default:
		pr_debug("px4_tsdev_unlocked_ioctl: unknown ioctl %08x\n", cmd);
		ret = -ENOSYS;
		break;
	}

	mutex_unlock(&px4->lock);

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
	int ret = 0, dev_idx = -1, i;
	struct usb_device *usbdev;
	struct px4_device *px4 = NULL;
	struct it930x_bridge *it930x;
	struct it930x_bus *bus;

	pr_debug("px4_probe: xfer_packets: %u\n", xfer_packets);

	mutex_lock(&glock);

	for (i = 0; i < MAX_DEVICE; i++) {
		if (!devs[i] && !devs_reserve[i]) {
			dev_idx = i;
			devs_reserve[i] = true;
			break;
		}
	}

	mutex_unlock(&glock);

	pr_debug("px4_probe: dev_idx: %d\n", dev_idx);

	if (dev_idx == -1) {
		pr_err("Unused device index was not found.\n");
		ret = -ECANCELED;
		goto fail;
	}

	usbdev = interface_to_usbdev(intf);

	px4 = kzalloc(sizeof(*px4), GFP_KERNEL);
	if (!px4) {
		pr_err("px4_probe: kzalloc returns NULL.\n");
		ret = -ENOMEM;
		goto fail;
	}

	px4->dev_idx = dev_idx;
	px4->vid = id->idVendor;
	px4->pid = id->idProduct;
	px4->dev_id = 0;

	if (strlen(usbdev->serial) == 15)
		if (kstrtouint(&usbdev->serial[14], 16, &px4->dev_id))
			pr_debug("px4_probe: kstrtouint() failed.\n");
		else
			pr_debug("px4_probe: dev_id: %u\n", px4->dev_id);
	else
		pr_debug("px4_probe: the length of serial number is invalid.\n");

	it930x = &px4->it930x;
	bus = &it930x->bus;

	// Initialize px4 structure

	ret = px4_init(px4);
	if (ret)
		goto fail;

	// Initialize bus operator

	bus->type = IT930X_BUS_USB;
	bus->usb.dev = usbdev;
	bus->usb.ctrl_timeout = 3000;
	bus->usb.streaming_xfer_size = xfer_packets * 188;

	ret = it930x_bus_init(bus);
	if (ret)
		goto fail;

	// Load config from eeprom

	ret = px4_load_config(px4);
	if (ret)
		goto fail;

	// Initialize IT930x bridge

	ret = it930x_init(it930x);
	if (ret)
		goto fail;

	ret = it930x_load_firmware(it930x, FIRMWARE_FILENAME);
	if (ret)
		goto fail;

	ret = it930x_init_device(it930x);
	if (ret)
		goto fail;

	// Initialize tc90522 structure

	for (i = 0; i < TSDEV_NUM; i++)
		tc90522_init(&px4->tsdev[i].tc90522);

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

	it930x_purge_psb(it930x);

	// cdev

	cdev_init(&px4->cdev, &px4_tsdev_fops);
	px4->cdev.owner = THIS_MODULE;

	ret = cdev_add(&px4->cdev, MKDEV(MAJOR(px4_dev_first), MINOR(px4_dev_first) + (dev_idx * TSDEV_NUM)), TSDEV_NUM);
	if (ret < 0) {
		pr_err("Couldn't add cdev to the system.\n");
		goto fail;
	}

	mutex_lock(&glock);

	// create /dev/px4video*
	for (i = 0; i < TSDEV_NUM; i++) {
		pr_info("tsdev %i: px4video%u\n", i, (MINOR(px4_dev_first) + (dev_idx * TSDEV_NUM) + i));
		device_create(px4_class, &intf->dev, MKDEV(MAJOR(px4_dev_first), (MINOR(px4_dev_first) + (dev_idx * TSDEV_NUM) + i)), NULL, "px4video%u", (MINOR(px4_dev_first) + (dev_idx * TSDEV_NUM) + i));
	}

	devs[dev_idx] = px4;
	devs_reserve[dev_idx] = false;

	mutex_unlock(&glock);

	usb_set_intfdata(intf, px4);

	return 0;
fail:
	if (px4) {
		it930x_bus_term(&px4->it930x.bus);
		kfree(px4);
	}

	devs_reserve[dev_idx] = false;

	return ret;
}

static void px4_disconnect(struct usb_interface *intf)
{
	int i, ref;
	struct px4_device *px4;

	px4 = usb_get_intfdata(intf);
	if (!px4)
		return;

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

	while (ref) {
		wait_event(px4->wait, (ref != atomic_read(&px4->ref)));
		ref = atomic_read(&px4->ref);
	}

	// uninitialize
	it930x_bus_term(&px4->it930x.bus);
	kfree(px4);

	return;
}

static int px4_suspend(struct usb_interface *intf, pm_message_t message)
{
	return 0;
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

#ifdef PX4_DRIVER_VERSION
	pr_info(KBUILD_MODNAME " version " PX4_DRIVER_VERSION "\n");
#endif

	for (i = 0; i < MAX_DEVICE; i++) {
		devs[i] = NULL;
		devs_reserve[i] = false;
	}

	ret = alloc_chrdev_region(&px4_dev_first, 0, MAX_TSDEV, DEVICE_NAME);
	if (ret < 0) {
		pr_debug("px4_module_init: alloc_chrdev_region() failed.\n");
		return ret;
	}

	px4_class = class_create(THIS_MODULE, "px4");
	if (IS_ERR(px4_class)) {
		pr_debug("px4_module_init: class_create() failed.\n");
		return PTR_ERR(px4_class);
	}

	ret = usb_register(&px4_usb_driver);
	if (ret)
		pr_debug("px4_module_init: usb_register() failed.\n");

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
