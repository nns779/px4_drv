// SPDX-License-Identifier: GPL-2.0-only
/*
 * PX4 multi-device power manager definitions (px4_mldev.h)
 *
 * Copyright (c) 2018-2021 nns779
 */

#ifndef __PX4_MLDEV_H__
#define __PX4_MLDEV_H__

#include <linux/types.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>

#include "px4_device.h"

struct px4_mldev {
	struct kref kref;
	struct list_head list;
	struct mutex lock;
	unsigned long long serial_number;
	struct px4_device *dev[2];
	bool power_state[2];
	int (*backend_set_power)(struct px4_device *px4, bool state);
};

bool px4_mldev_search(unsigned long long serial_number,
		      struct px4_mldev **mldev);
int px4_mldev_alloc(struct px4_mldev **mldev, struct px4_device *px4,
		    int (*backend_set_power)(struct px4_device *, bool));
int px4_mldev_add(struct px4_mldev *mldev, struct px4_device *px4);
int px4_mldev_remove(struct px4_mldev *mldev, struct px4_device *px4);
int px4_mldev_set_power(struct px4_mldev *mldev, struct px4_device *px4,
			bool state);

#endif
