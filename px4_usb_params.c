// SPDX-Licence-Identifier: GPL-2.0-only
/*
 * Module parameter definitions (px4_usb_params.c)
 *
 * Copyright (c) 2018-2021 nns779
 */

#include "px4_usb_params.h"

#include <linux/kernel.h>
#include <linux/module.h>

struct px4_usb_param_set px4_usb_params = {
	.ctrl_timeout = 3000,
	.xfer_packets = 816,
	.urb_max_packets = 816,
	.max_urbs = 6,
	.no_dma = false
};

module_param_named(ctrl_timeout, px4_usb_params.ctrl_timeout,
		   int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(ctrl_timeout,
		 "Time in msecs to wait for the message to complete " \
		 "before timing out (if 0 the wait is forever). (default: 3000)");

module_param_named(xfer_packets, px4_usb_params.xfer_packets,
		   uint, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(xfer_packets,
		 "Number of transfer packets from the device. (default: 816)");

module_param_named(urb_max_packets, px4_usb_params.urb_max_packets,
		   uint, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(urb_max_packets,
		 "Maximum number of TS packets per URB. (default: 816)");

module_param_named(max_urbs, px4_usb_params.max_urbs,
		   uint, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(max_urbs, "Maximum number of URBs. (default: 6)");

module_param_named(no_dma, px4_usb_params.no_dma,
		   bool, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
