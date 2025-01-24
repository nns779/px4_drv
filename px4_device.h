// SPDX-License-Identifier: GPL-2.0-only
/*
 * PTX driver definitions for PLEX PX4/PX5 series devices (px4_device.h)
 *
 * Copyright (c) 2018-2021 nns779
 */

#ifndef __PX4_DEVICE_H__
#define __PX4_DEVICE_H__

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/device.h>

#include "px4_mldev.h"
#include "ptx_chrdev.h"
#include "it930x.h"
#include "tc90522.h"
#include "r850.h"
#include "rt710.h"

#define PX4_CHRDEV_NUM			4

struct px4_device;

struct px4_chrdev {
	struct ptx_chrdev *chrdev;
	struct px4_device *parent;
	bool lnb_power;
	struct tc90522_demod tc90522;
	union {
		struct r850_tuner r850;
		struct rt710_tuner rt710;
	} tuner;
};

struct px4_serial_number {
	unsigned long long serial_number;
	unsigned int dev_id;
};

struct px4_device {
	struct mutex lock;
	struct kref kref;
	atomic_t available;
	struct device *dev;
	struct px4_serial_number serial;
	struct px4_mldev *mldev;
	struct completion *quit_completion;
	unsigned int open_count;
	unsigned int lnb_power_count;
	unsigned int streaming_count;
	struct ptx_chrdev_group *chrdev_group;
	struct px4_chrdev chrdev4[PX4_CHRDEV_NUM];
	struct it930x_bridge it930x;
	void *stream_ctx;
};

int px4_device_init(struct px4_device *px4, struct device *dev,
		    const char *dev_serial, bool use_mldev,
		    struct ptx_chrdev_context *chrdev_ctx,
		    struct completion *quit_completion);
void px4_device_term(struct px4_device *px4);

#endif
