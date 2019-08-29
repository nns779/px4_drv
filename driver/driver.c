// SPDX-License-Identifier: GPL-2.0-only
/*
 * Module initiator of the driver (driver.c)
 *
 * Copyright (c) 2019 nns779
 */

#include "print_format.h"

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "driver.h"
#include "revision.h"
#include "firmware.h"
#include "px4.h"
#include "isdb2056.h"

MODULE_VERSION(DRIVER_VERSION);
MODULE_AUTHOR("nns779");
MODULE_DESCRIPTION("Unofficial Linux driver for PLEX PX-W3U4/Q3U4/W3PE4/Q3PE4 ISDB-T/S receivers");
MODULE_LICENSE("GPL v2");

MODULE_FIRMWARE(FIRMWARE_FILENAME);

static int driver_module_init(void)
{
	int ret = 0;

	pr_info(KBUILD_MODNAME
#ifdef DRIVER_VERSION
		" version " DRIVER_VERSION
#endif
#ifdef REVISION_NUMBER
#if defined(DRIVER_VERSION)
		","
#endif
		" rev: " REVISION_NUMBER
#endif
#ifdef COMMIT_HASH
#if defined(PX4_DRIVER_VERSION) || defined(REVISION_NUMBER)
		","
#endif
		" commit: " COMMIT_HASH
#endif
#ifdef REVISION_NAME
		" @ " REVISION_NAME
#endif
		"\n");

	ret = px4_register();
	if (ret)
		goto exit;

	ret = isdb2056_register();
	if (ret)
		goto exit;

exit:
	return ret;
}

static void driver_module_exit(void)
{
	px4_unregister();
	isdb2056_unregister();
}

module_init(driver_module_init);
module_exit(driver_module_exit);
