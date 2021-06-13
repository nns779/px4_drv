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

enum px4_mldev_mode {
	PX4_MLDEV_ALL_MODE = 0,
	PX4_MLDEV_S_ONLY_MODE,
	PX4_MLDEV_S0_ONLY_MODE,
	PX4_MLDEV_S1_ONLY_MODE,
};

struct px4_mldev {
	struct kref kref;
	struct list_head list;
	struct mutex lock;
	enum px4_mldev_mode mode;
	unsigned long long serial_number;
	struct px4_device *dev[2];
	bool power_state[2];
	bool chrdev_state[2][4];
	int (*backend_set_power)(struct px4_device *px4, bool state);
};

bool px4_mldev_search(unsigned long long serial_number,
		      struct px4_mldev **mldev);
int px4_mldev_alloc(struct px4_mldev **mldev, enum px4_mldev_mode mode,
		    struct px4_device *px4,
		    int (*backend_set_power)(struct px4_device *, bool));
int px4_mldev_add(struct px4_mldev *mldev, struct px4_device *px4);
int px4_mldev_remove(struct px4_mldev *mldev, struct px4_device *px4);
int px4_mldev_set_power(struct px4_mldev *mldev, struct px4_device *px4,
			unsigned int chrdev_id, bool state, bool *first);

#endif
