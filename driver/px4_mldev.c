// SPDX-License-Identifier: GPL-2.0-only
/*
 * PX4 multi-device power manager (px4_mldev.c)
 *
 * Copyright (c) 2018-2020 nns779
 */

#include "print_format.h"
#include "px4_mldev.h"

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/device.h>

static LIST_HEAD(px4_mldev_list);
static DEFINE_MUTEX(px4_mldev_glock);

static unsigned int __px4_mldev_get_power_count(struct px4_mldev *mldev);

bool px4_mldev_search(unsigned long long serial_number,
		      struct px4_mldev **mldev)
{
	struct px4_mldev *m;

	*mldev = NULL;

	mutex_lock(&px4_mldev_glock);
	list_for_each_entry(m, &px4_mldev_list, list) {
		if (m->serial_number == serial_number) {
			*mldev = m;
			break;
		}
	}
	mutex_unlock(&px4_mldev_glock);

	return (*mldev) ? true : false;
}

int px4_mldev_alloc(struct px4_mldev **mldev, struct px4_device *px4,
		    int (*backend_set_power)(struct px4_device *, bool))
{
	unsigned int dev_id = px4->serial.dev_id - 1;
	struct px4_mldev *m;

	dev_dbg(px4->dev,
		"px4_mldev_alloc: serial_number: %014llu, dev_id: %u\n",
		px4->serial.serial_number, px4->serial.dev_id);

	if (dev_id > 1)
		return -EINVAL;

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;

	kref_init(&m->kref);
	mutex_init(&m->lock);
	m->serial_number = px4->serial.serial_number;
	m->dev[px4->serial.dev_id - 1] = px4;
	m->backend_set_power = backend_set_power;

	mutex_lock(&px4_mldev_glock);
	list_add_tail(&m->list, &px4_mldev_list);
	mutex_unlock(&px4_mldev_glock);

	*mldev = m;

	return 0;
}

static void px4_mldev_release(struct kref *kref)
{
	struct px4_mldev *mldev = container_of(kref, struct px4_mldev, kref);

	pr_debug("px4_mldev_release: serial_number: %014llu\n",
		 mldev->serial_number);

	mutex_lock(&px4_mldev_glock);
	list_del(&mldev->list);
	mutex_unlock(&px4_mldev_glock);

	mutex_destroy(&mldev->lock);
	kfree(mldev);

	return;
}

int px4_mldev_add(struct px4_mldev *mldev, struct px4_device *px4)
{
	int ret = 0;
	unsigned int dev_id = px4->serial.dev_id - 1;

	dev_dbg(px4->dev,
		"px4_mldev_add: serial_number: %014llu, dev_id: %u\n",
		mldev->serial_number, dev_id + 1);

	if (dev_id > 1)
		return -EINVAL;

	mutex_lock(&mldev->lock);

	if (kref_read(&mldev->kref) >= 2) {
		ret = -EINVAL;
		goto exit;
	}

	if (mldev->dev[dev_id]) {
		ret = -EALREADY;
		goto exit;
	}

	if (__px4_mldev_get_power_count(mldev))
		ret = mldev->backend_set_power(px4, true);

	if (ret)
		goto exit;

	kref_get(&mldev->kref);
	mldev->dev[dev_id] = px4;

exit:
	mutex_unlock(&mldev->lock);
	return ret;
}

int px4_mldev_remove(struct px4_mldev *mldev, struct px4_device *px4)
{
	unsigned int dev_id = px4->serial.dev_id - 1;

	dev_dbg(px4->dev,
		"px4_mldev_remove: serial_number: %014llu, dev_id: %u\n",
		mldev->serial_number, dev_id + 1);

	if (dev_id > 1)
		return -EINVAL;

	mutex_lock(&mldev->lock);

	if (mldev->dev[dev_id] != px4) {
		mutex_unlock(&mldev->lock);
		return -EINVAL;
	}

	if (__px4_mldev_get_power_count(mldev) && mldev->power_state[dev_id]) {
		mldev->backend_set_power(px4, false);
		mldev->power_state[dev_id] = false;
	}

	mldev->dev[dev_id] = NULL;

	if (kref_put(&mldev->kref, px4_mldev_release))
		return 0;

	mutex_unlock(&mldev->lock);
	return 0;
}

static unsigned int __px4_mldev_get_power_count(struct px4_mldev *mldev)
{
	return (!!mldev->power_state[0]) + (!!mldev->power_state[1]);
}

int px4_mldev_set_power(struct px4_mldev *mldev, struct px4_device *px4,
			bool state)
{
	int ret = 0, i;
	unsigned int dev_id = px4->serial.dev_id - 1;

	dev_dbg(px4->dev,
		"px4_mldev_set_power: serial_number: %014llu, dev_id: %u, state: %s\n",
		mldev->serial_number, dev_id + 1, (state) ? "true" : "false");
	dev_dbg(px4->dev,
		"px4_mldev_set_power: power_state: %s, %s\n",
		(mldev->power_state[0]) ? "true" : "false",
		(mldev->power_state[1]) ? "true" : "false");

	if (dev_id > 1)
		return -EINVAL;

	mutex_lock(&mldev->lock);

	if (!mldev->dev[dev_id]) {
		ret = -EINVAL;
		goto exit;
	}

	if (mldev->power_state[dev_id] == state)
		goto exit;

	if (!state)
		mldev->power_state[dev_id] = false;

	if (!__px4_mldev_get_power_count(mldev)) {
		for (i = 0; i < 2; i++) {
			if (!mldev->dev[i])
				continue;

			ret = mldev->backend_set_power(mldev->dev[i], state);
			if (ret)
				break;
		}
	}

	if (state && !ret)
		mldev->power_state[dev_id] = true;

exit:
	mutex_unlock(&mldev->lock);
	return ret;
}
