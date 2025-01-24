// SPDX-Licence-Identifier: GPL-2.0-only
/*
 * Module parameter definitions (px4_device_params.c)
 *
 * Copyright (c) 2018-2021 nns779
 */

#include "px4_device_params.h"

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/module.h>

static const struct {
	enum px4_mldev_mode mode;
	char str[8];
} mldev_mode_table[] = {
	{ PX4_MLDEV_ALL_MODE, "all" },
	{ PX4_MLDEV_S_ONLY_MODE, "s-only" },
	{ PX4_MLDEV_S0_ONLY_MODE, "s0-only" },
	{ PX4_MLDEV_S1_ONLY_MODE, "s1-only" },
};

struct px4_device_param_set px4_device_params = {
	.tsdev_max_packets = 2048,
	.psb_purge_timeout = 2000,
	.disable_multi_device_power_control = false,
	.multi_device_power_control_mode = PX4_MLDEV_ALL_MODE,
	.s_tuner_no_sleep = false,
	.discard_null_packets = false
};

static int set_multi_device_power_control_mode(const char *val,
					       const struct kernel_param *kp)
{
	int i;
	enum px4_mldev_mode mode;

	for (i = 0; i < ARRAY_SIZE(mldev_mode_table); i++) {
		if (sysfs_streq(val, mldev_mode_table[i].str)) {
			mode = mldev_mode_table[i].mode;
			break;
		}
	}

	if (i == ARRAY_SIZE(mldev_mode_table))
		return -EINVAL;

	px4_device_params.multi_device_power_control_mode = mode;
	return 0;
}

static int get_multi_device_power_control_mode(char *buffer,
					       const struct kernel_param *kp)
{
	enum px4_mldev_mode mode = px4_device_params.multi_device_power_control_mode;

	if (mode < PX4_MLDEV_ALL_MODE && mode > PX4_MLDEV_S1_ONLY_MODE)
		return -EINVAL;

	return scnprintf(buffer, 4096, "%s\n", mldev_mode_table[mode].str);
}

static const struct kernel_param_ops multi_device_power_control_mode_ops = {
	.set = set_multi_device_power_control_mode,
	.get = get_multi_device_power_control_mode
};

module_param_named(tsdev_max_packets, px4_device_params.tsdev_max_packets,
		   uint, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(tsdev_max_packets,
		 "Maximum number of TS packets buffering in tsdev. (default: 2048)");

module_param_named(psb_purge_timeout, px4_device_params.psb_purge_timeout,
		   int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

module_param_named(disable_multi_device_power_control,
		   px4_device_params.disable_multi_device_power_control,
		   bool, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

module_param_cb(multi_device_power_control_mode, 
		&multi_device_power_control_mode_ops,
		NULL, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

module_param_named(s_tuner_no_sleep, px4_device_params.s_tuner_no_sleep,
		   bool, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

module_param_named(discard_null_packets, px4_device_params.discard_null_packets,
		   bool, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
