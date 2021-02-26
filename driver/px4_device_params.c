// SPDX-Licence-Identifier: GPL-2.0-only
/*
 * Module parameter definitions (px4_device_params.c)
 *
 * Copyright (c) 2018-2021 nns779
 */

#include "px4_device_params.h"

#include <linux/kernel.h>
#include <linux/module.h>

struct px4_device_param_set px4_device_params = {
	.tsdev_max_packets = 2048,
	.psb_purge_timeout = 2000,
	.disable_multi_device_power_control = false,
	.s_tuner_no_sleep = false,
	.discard_null_packets = false
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

module_param_named(s_tuner_no_sleep, px4_device_params.s_tuner_no_sleep,
		   bool, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

module_param_named(discard_null_packets, px4_device_params.discard_null_packets,
		   bool, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
