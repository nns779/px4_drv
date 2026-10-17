// SPDX-License-Identifier: GPL-2.0-only
/*
 * PTX driver definitions for DIGIBEST ISDBT2071 (DTV03A-1TU) device (isdbt2071_device.h)
 *
 * Copyright (c) 2023 techma.
 */

#ifndef __ISDBT2071_DEVICE_H__
#define __ISDBT2071_DEVICE_H__

#include <linux/atomic.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/device.h>

#include "ptx_chrdev.h"
#include "px4_card.h"
#include "it930x.h"
#include "tc90522.h"
#include "r850.h"

#define ISDBT2071_CHRDEV_NUM	1

struct isdbt2071_chrdev {
	struct ptx_chrdev *chrdev;
	struct tc90522_demod tc90522_t;
	struct tc90522_demod tc90522_s;
	struct r850_tuner r850;
};

struct isdbt2071_device {
	struct kref kref;
	/* チューナーと内蔵カードリーダーで共有する基板電源の状態を守る */
	struct mutex lock;
	atomic_t available;
	bool tuner_open;
	/* 内蔵カードリーダーが基板の電源を使っているか (チューナーを閉じても、カードの使用中は電源を切らない) */
	bool card_powered;
	struct px4_card *card;
	struct device *dev;
	struct completion *quit_completion;
	struct ptx_chrdev_group *chrdev_group;
	struct isdbt2071_chrdev chrdev2071;
	struct it930x_bridge it930x;
	void *stream_ctx;
};

int isdbt2071_device_init(struct isdbt2071_device *isdbt2071, struct device *dev,
			struct ptx_chrdev_context *chrdev_ctx,
			struct completion *quit_completion);
void isdbt2071_device_term(struct isdbt2071_device *isdbt2071);

#endif
