// SPDX-License-Identifier: GPL-2.0-only
/*
 * Module parameter definitions (module_param.c)
 *
 * Copyright (c) 2019 nns779
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "module_param.h"

unsigned int xfer_packets = 816;
unsigned int urb_max_packets = 816;
unsigned int max_urbs = 6;
unsigned int tsdev_max_packets = 2048;
int psb_purge_timeout = 2000;
bool no_dma = false;
bool disable_multi_device_power_control = false;
bool s_tuner_no_sleep = false;

module_param(xfer_packets, uint, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(xfer_packets, "Number of transfer packets from the device. (default: 816)");

module_param(urb_max_packets, uint, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(urb_max_packets, "Maximum number of TS packets per URB. (default: 816)");

module_param(max_urbs, uint, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(max_urbs, "Maximum number of URBs. (default: 6)");

module_param(tsdev_max_packets, uint, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(tsdev_max_packets, "Maximum number of TS packets buffering in tsdev. (default: 2048)");

module_param(psb_purge_timeout, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

module_param(no_dma, bool, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

module_param(disable_multi_device_power_control, bool, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

module_param(s_tuner_no_sleep, bool, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
