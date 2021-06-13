// SPDX-License-Identifier: GPL-2.0-only
/*
 * PX4 multi-device power manager (px4_mldev.c)
 *
 * Copyright (c) 2018-2021 nns779
 */

#include "print_format.h"
#include "px4_mldev.h"

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/device.h>

static LIST_HEAD(px4_mldev_list);
static DEFINE_MUTEX(px4_mldev_glock);

static bool px4_mldev_get_chrdev_status(struct px4_mldev *mldev,
				       unsigned int dev_id);
static bool px4_mldev_is_power_interlocking_required(struct px4_mldev *mldev,
						     unsigned int dev_id);

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

int px4_mldev_alloc(struct px4_mldev **mldev,  enum px4_mldev_mode mode,
		    struct px4_device *px4,
		    int (*backend_set_power)(struct px4_device *, bool))
{
	int i, j;
	unsigned int dev_id = px4->serial.dev_id - 1;
	struct px4_mldev *m;

	dev_dbg(px4->dev,
		"px4_mldev_alloc: serial_number: %014llu, dev_id: %u\n",
		px4->serial.serial_number, dev_id + 1);

	if (dev_id > 1)
		return -EINVAL;

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;

	kref_init(&m->kref);
	mutex_init(&m->lock);
	m->mode = mode;
	m->serial_number = px4->serial.serial_number;
	for (i = 0; i < 2; i++) {
		m->dev[i] = (i == dev_id) ? px4 : NULL;
		m->power_state[i] = false;
		for (j = 0; j < 4; j++)
			m->chrdev_state[i][j] = false;
	}
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
	int ret = 0, i;
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

	mldev->power_state[dev_id] = false;
	for (i = 0; i < 4; i++)
		mldev->chrdev_state[dev_id][i] = false;

	if (px4_mldev_is_power_interlocking_required(mldev, (dev_id) ? 0 : 1)) {
		ret = mldev->backend_set_power(px4, true);
		if (ret)
			goto exit;

		mldev->power_state[dev_id] = true;
	}

	mldev->dev[dev_id] = px4;
	kref_get(&mldev->kref);

exit:
	mutex_unlock(&mldev->lock);
	return ret;
}

int px4_mldev_remove(struct px4_mldev *mldev, struct px4_device *px4)
{
	int i;
	unsigned int dev_id = px4->serial.dev_id - 1;
	unsigned int other_dev_id = (dev_id) ? 1 : 0;

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

	if (mldev->power_state[dev_id])
		mldev->backend_set_power(px4, false);

	mldev->dev[dev_id] = NULL;
	mldev->power_state[dev_id] = false;
	for (i = 0; i < 4; i++)
		mldev->chrdev_state[dev_id][i] = false;

	if (mldev->dev[other_dev_id] &&
	    !px4_mldev_get_chrdev_status(mldev, other_dev_id) &&
	    mldev->power_state[other_dev_id]) {
		mldev->backend_set_power(mldev->dev[other_dev_id], false);
		mldev->power_state[other_dev_id] = false;
	}

	if (kref_put(&mldev->kref, px4_mldev_release))
		return 0;

	mutex_unlock(&mldev->lock);
	return 0;
}

static bool px4_mldev_get_chrdev_status(struct px4_mldev *mldev,
				       unsigned int dev_id)
{
	bool *state = mldev->chrdev_state[dev_id];
	return (state[0] || state[1] || state[2] || state[3]);
}

static bool px4_mldev_is_power_interlocking_required(struct px4_mldev *mldev,
						     unsigned int dev_id)
{
	bool ret = false;
	bool *state = mldev->chrdev_state[dev_id];

	switch (mldev->mode) {
	case PX4_MLDEV_S_ONLY_MODE:
		ret = state[0] || state[1];
		break;

	case PX4_MLDEV_S0_ONLY_MODE:
		ret = state[0];
		break;

	case PX4_MLDEV_S1_ONLY_MODE:
		ret = state[1];
		break;

	default:
		ret = state[0] || state[1] || state[2] || state[3];
		break;
	}

	return ret;
}

int px4_mldev_set_power(struct px4_mldev *mldev, struct px4_device *px4,
			unsigned int chrdev_id, bool state, bool *first)
{
	int ret = 0;
	unsigned int dev_id = px4->serial.dev_id - 1;
	unsigned int other_dev_id = (dev_id) ? 0 : 1;

	dev_dbg(px4->dev,
		"px4_mldev_set_power: serial_number: %014llu, dev_id: %u, chrdev_id: %u state: %s\n",
		mldev->serial_number, dev_id, chrdev_id,
		(state) ? "true" : "false");
	dev_dbg(px4->dev,
		"px4_mldev_set_power: power_state: %s, %s\n",
		(mldev->power_state[0]) ? "true" : "false",
		(mldev->power_state[1]) ? "true" : "false");
	dev_dbg(px4->dev,
		"px4_mldev_set_power: chrdev_state[%u][%u]: %s\n",
		dev_id, chrdev_id, 
		(mldev->chrdev_state[dev_id][chrdev_id]) ? "true" : "false");

	if (dev_id > 1 || chrdev_id > 3)
		return -EINVAL;

	mutex_lock(&mldev->lock);

	if (mldev->dev[dev_id] != px4) {
		ret = -EINVAL;
		goto exit;
	}

	if (mldev->chrdev_state[dev_id][chrdev_id] == state)
		goto exit;

	if (!state)
		mldev->chrdev_state[dev_id][chrdev_id] = false;

	if (!px4_mldev_get_chrdev_status(mldev, dev_id)) {
		if (mldev->power_state[dev_id] != state &&
		    (state || !px4_mldev_is_power_interlocking_required(mldev, other_dev_id))) {
			ret = mldev->backend_set_power(mldev->dev[dev_id],
						       state);
			if (ret && state)
				goto exit;

			mldev->power_state[dev_id] = state;
		}

		if (state && first)
			*first = true;
	}

	if (state)
		mldev->chrdev_state[dev_id][chrdev_id] = true;

	if (mldev->dev[other_dev_id]) {
		bool interlocking = px4_mldev_is_power_interlocking_required(mldev, dev_id);

		dev_dbg(px4->dev,
			"px4_mldev_set_power: interlocking: %s\n",
			(interlocking) ? "true" : "false");

		if (interlocking == state &&
		    mldev->power_state[other_dev_id] != interlocking &&
		    (state || !px4_mldev_get_chrdev_status(mldev, other_dev_id))) {
			ret = mldev->backend_set_power(mldev->dev[other_dev_id],
						       state);
			if (!ret || !state)
				mldev->power_state[other_dev_id] = state;
		}
	}

exit:
	mutex_unlock(&mldev->lock);
	return ret;
}
