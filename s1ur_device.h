// SPDX-License-Identifier: GPL-2.0-only
/*
 * PTX driver definitions for PLEX PX-S1UR device (s1ur_device.h)
 *
 * Copyright (c) 2023 techma.
 */

#ifndef __S1UR_DEVICE_H__
#define __S1UR_DEVICE_H__

#include <linux/atomic.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/device.h>

#include "ptx_chrdev.h"
#include "it930x.h"
#include "tc90522.h"
#include "r850.h"

#define S1UR_CHRDEV_NUM	1

struct s1ur_chrdev {
	struct ptx_chrdev *chrdev;
	struct tc90522_demod tc90522_t;
	struct tc90522_demod tc90522_s;
	struct r850_tuner r850;
	/*struct rt710_tuner rt710;*/
};

struct s1ur_device {
	struct kref kref;
	atomic_t available;
	struct device *dev;
	struct completion *quit_completion;
	struct ptx_chrdev_group *chrdev_group;
	struct s1ur_chrdev chrdevs1ur;
	struct it930x_bridge it930x;
	void *stream_ctx;
};

int s1ur_device_init(struct s1ur_device *s1ur, struct device *dev,
			 struct ptx_chrdev_context *chrdev_ctx,
			 struct completion *quit_completion);
void s1ur_device_term(struct s1ur_device *s1ur);

#endif
