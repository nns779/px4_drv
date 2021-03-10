// SPDX-License-Identifier: GPL-2.0-only
/*
 * PTX driver definitions for PLEX PX-MLT series devices (pxmlt_device.c)
 *
 * Copyright (c) 2018-2021 nns779
 */

#ifndef __PXMLT_DEVICE_H__
#define __PXMLT_DEVICE_H__

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/device.h>

#include "ptx_chrdev.h"
#include "it930x.h"
#include "cxd2856er.h"
#include "cxd2858er.h"

#define PXMLT_CHRDEV_MAX_NUM	5

#define PXMLT5_CHRDEV_NUM	5
#define PXMLT8_CHRDEV_NUM	8
#define ISDB6014_4TS_CHRDEV_NUM	4

enum pxmlt_model {
	PXMLT5U_MODEL = 0,
	PXMLT5PE_MODEL,
	PXMLT8PE3_MODEL,
	PXMLT8PE5_MODEL,
	ISDB6014_4TS_MODEL
};

struct pxmlt_device;

struct pxmlt_chrdev {
	struct ptx_chrdev *chrdev;
	struct pxmlt_device *parent;
	bool lnb_power;
	struct mutex *tuner_lock;
	struct cxd2856er_demod cxd2856er;
	struct cxd2858er_tuner cxd2858er;
};

struct pxmlt_device {
	struct mutex lock;
	struct kref kref;
	atomic_t available;
	struct device *dev;
	struct completion *quit_completion;
	unsigned int open_count;
	unsigned int lnb_power_count;
	unsigned int streaming_count;
	struct mutex tuner_lock[2];
	struct ptx_chrdev_group *chrdev_group;
	int chrdevm_num;
	struct pxmlt_chrdev chrdevm[PXMLT_CHRDEV_MAX_NUM];
	struct it930x_bridge it930x;
	void *stream_ctx;
};

int pxmlt_device_init(struct pxmlt_device *pxmlt, struct device *dev,
		      enum pxmlt_model model,
		      struct ptx_chrdev_context *chrdev_ctx,
		      struct completion *quit_completion);
void pxmlt_device_term(struct pxmlt_device *pxmlt);

#endif
